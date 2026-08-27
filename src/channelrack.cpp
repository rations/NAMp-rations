// ChannelRack implementation. See channelrack.h for why a channel switch is a priming problem
// rather than a mixing problem.

#include "channelrack.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace Rations
{

//------------------------------------------------------------------------
ChannelRack::ChannelRack()
{
    for (int c = 0; c < kChannelCount; ++c)
        mEngine[c].setLoader(&mLoader[c]);
}

//------------------------------------------------------------------------
ChannelRack::~ChannelRack()
{
    // Join every worker before anything it could still be writing into goes away. Doing this only
    // in the processor's terminate() is not enough: a host may destroy a component it never
    // initialised.
    stop();
    releaseBanks();
}

//------------------------------------------------------------------------
void ChannelRack::prepare(int maxNativeFrames, double nativeSampleRate)
{
    mNativeSampleRate = nativeSampleRate > 0.0 ? nativeSampleRate : kNativeSampleRate;
    const size_t frames = static_cast<size_t>(std::max(maxNativeFrames, engine::kChunk));

    for (int c = 0; c < kChannelCount; ++c)
        mEngine[c].prepare(maxNativeFrames, mNativeSampleRate);

    // The ring is a power of two so the wrap is a mask. Sized by engineconfig against one whole
    // receptive field plus what arrives while the catch-up drains it — see kInputRingSamples.
    static_assert((engine::kInputRingSamples & (engine::kInputRingSamples - 1)) == 0,
                  "the input ring is indexed with a mask, so its size must be a power of two");
    mRing.assign(static_cast<size_t>(engine::kInputRingSamples), 0.0);
    mWritePos = 0;
    mCursor = 0;

    mFeed.assign(static_cast<size_t>(engine::kChunk), 0.0);
    mSink.assign(std::max(frames, static_cast<size_t>(engine::kChunk)), 0.0);
    mMix.assign(frames, 0.0);
    mMixPtr = mMix.data();

    const double fadeSamples = std::max(1.0, engine::kChannelFadeMs * 0.001 * mNativeSampleRate);
    mFadeStep = 1.0 / fadeSamples;

    mFading = false;
    mFadeMix = 0.0;
    mTarget = kNoChannel;
    mPrimed = false;
}

//------------------------------------------------------------------------
void ChannelRack::start()
{
    for (int c = 0; c < kChannelCount; ++c)
        mLoader[c].start();
}

void ChannelRack::stop()
{
    for (int c = 0; c < kChannelCount; ++c)
        mLoader[c].stop();
}

//------------------------------------------------------------------------
// One worker per channel, all four building at once. That is what makes the build breadth-first
// without any cross-channel scheduling: each worker serves its own channel's dial position first
// (the engine hints it every block, sounding or not), so every channel becomes switchable after
// roughly one model's build time rather than after all thirty-five are done.
void ChannelRack::loadChannel(Channel ch, const std::string &dir, double slim, int maxBufferSize)
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    mLoadRequested[i] = true;
    mLoader[i].loadDirectory(dir, slim, maxBufferSize);
}

//------------------------------------------------------------------------
void ChannelRack::setOutputMode(int mode, double calLevelDbu)
{
    for (int c = 0; c < kChannelCount; ++c)
        mEngine[c].setOutputMode(mode, calLevelDbu);
}

//------------------------------------------------------------------------
std::vector<std::string> ChannelRack::captureNames(Channel ch) const
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    return mLoader[i].captureNames();
}

//------------------------------------------------------------------------
void ChannelRack::releaseBanks()
{
    for (int c = 0; c < kChannelCount; ++c)
        ModelBank::destroyBank(mEngine[c].releaseBank());
}

//------------------------------------------------------------------------
void ChannelRack::pollBanks()
{
    for (int c = 0; c < kChannelCount; ++c)
        mBankChanged[c] = mEngine[c].pollBank();
}

//------------------------------------------------------------------------
void ChannelRack::setPositionNorm(Channel ch, double norm)
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    mEngine[i].setPositionNorm(norm);
}

//------------------------------------------------------------------------
void ChannelRack::requestChannel(Channel ch)
{
    mRequested = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
}

//------------------------------------------------------------------------
double ChannelRack::activeIndexNorm() const
{
    return mEngine[mFading ? mTo : mFrom].activeIndexNorm();
}

bool ChannelRack::playable() const
{
    return mEngine[mFrom].playable();
}

// Averaged over the channels that were ASKED to load, not over four. A channel whose directory
// is missing contributes 0 and holds the figure below 1 for good, which is the honest report:
// that channel really is not loaded, and a switch to it really will be held. Averaging over four
// regardless would be the same number; averaging over the ones that published would let the bar
// read 100% while three channels were still scanning, and then fall back.
float ChannelRack::progress() const
{
    float sum = 0.0f;
    int counted = 0;
    for (int c = 0; c < kChannelCount; ++c) {
        if (!mLoadRequested[c])
            continue;
        sum += mLoader[c].progress();
        ++counted;
    }
    return counted > 0 ? sum / static_cast<float>(counted) : 0.0f;
}

//------------------------------------------------------------------------
void ChannelRack::writeRing(const NAM_SAMPLE *src, int numFrames)
{
    const size_t mask = mRing.size() - 1;
    for (int i = 0; i < numFrames; ++i)
        mRing[static_cast<size_t>(mWritePos + i) & mask] = src[i];
    mWritePos += numFrames;
}

//------------------------------------------------------------------------
void ChannelRack::feedFromRing(CrossfadeEngine &target, long long from, long long count)
{
    const size_t mask = mRing.size() - 1;
    long long pos = from;
    long long left = count;
    while (left > 0) {
        const int n = static_cast<int>(std::min<long long>(left, engine::kChunk));
        for (int i = 0; i < n; ++i)
            mFeed[static_cast<size_t>(i)] = mRing[static_cast<size_t>(pos + i) & mask];
        NAM_SAMPLE *in = mFeed.data();
        NAM_SAMPLE *out = mSink.data();
        // The output goes nowhere. What matters is that the model's convolution history now holds
        // these samples, which is the whole of the state a feed-forward network has.
        target.processNative(&in, &out, n);
        pos += n;
        left -= n;
    }
}

//------------------------------------------------------------------------
void ChannelRack::reverseFade()
{
    std::swap(mFrom, mTo);
    // Complement rather than restart. At the instant of the reversal the output is
    // (1 - w)*from + w*to, and after the swap it is (1 - (1 - w))*to + (1 - w)*from — the same
    // sample. A restart from 0 would step by whatever the fade had already travelled.
    mFadeMix = 1.0 - mFadeMix;
}

//------------------------------------------------------------------------
void ChannelRack::beginCatchup(int ch, int numFrames)
{
    // Never start a catch-up while a fade is running, and this is a budget rule rather than a
    // tidiness one: a fade already costs two models, and a catch-up completing underneath it
    // would leave a third being kept exact in the same block. The fade is ten milliseconds. The
    // request stays live and the catch-up begins as soon as it ends, so nothing is queued and
    // nothing is lost — and it makes "a fade is running" and "a catch-up is in flight" mutually
    // exclusive, which is what lets the budget below be reasoned about at all.
    if (mFading)
        return;

    // R comes off the entry the worker read it off the model. Zero means the capture this
    // channel's dial names is not built yet, and the answer to that is to HOLD the request: the
    // audio does not move, the LED does not move, and the next block tries again. Sounding a
    // neighbouring capture instead would put an amp under the player's hands that they did not
    // select, which is worse than a switch that takes another instant to arrive.
    const int prewarm = mEngine[ch].restPrewarmSamples();
    if (prewarm <= 0)
        return;

    mEngine[ch].snapToRest();
    mTarget = ch;
    mPrimed = false;
    // Remembered, because a held catch-up parks the cursor at exactly this lag every block and
    // the entry it was read from may not be the one bound by then.
    mCatchupLag = prewarm;

    // Lag the cursor one whole receptive field behind the live block. Before the block, not
    // including it: the block itself is fed at the end of the catch-up, so the last sample the
    // incoming model sees is the same last sample the outgoing one saw.
    long long start = mWritePos - numFrames - prewarm;
    const long long oldest = mWritePos - static_cast<long long>(mRing.size());
    if (start < oldest)
        start = oldest; // defensive; kInputRingSamples is sized so this cannot bind
    if (start < 0)
        start = 0; // nothing before the session began, and the ring is zero-filled, which is
                   // exactly the state a model Reset at t = 0 has
    mCursor = start;
}

//------------------------------------------------------------------------
// A switch requested during a switch is resolved in place, never queued and never by leaving a
// half-primed model bound. The outgoing channel keeps sounding through every branch below, so
// there is no point at which the plug-in is silent.
void ChannelRack::resolveRequest(int numFrames)
{
    const int want = mRequested;

    // Where the rack is already headed. Written as one expression so "a switch is in flight" and
    // "this is where it is going" cannot disagree.
    const int destination = mTarget != kNoChannel ? mTarget : (mFading ? mTo : mFrom);
    if (want == destination)
        return;

    // Nothing has been heard yet — the plug-in is still coming up, or every capture the current
    // channel could sound is still being built. There is no click to prevent when there is no
    // sound to click, so adopt the channel outright rather than spending a catch-up on it.
    if (!mFading && mTarget == kNoChannel && !mEngine[mFrom].sounding()) {
        if (mEngine[want].restIndex() < 0)
            return; // held
        mEngine[want].snapToRest();
        mFrom = want;
        mTo = want;
        return;
    }

    // The request is for the channel currently fading OUT. It never stopped sounding, so it needs
    // no priming: reverse the fade and drop whatever catch-up was in flight. This costs nothing.
    if (mFading && want == mFrom) {
        reverseFade();
        mTarget = kNoChannel;
        mPrimed = false;
        return;
    }

    // The request is for the channel already fading IN. It is exact and on its way; the only
    // thing to undo is a catch-up that was heading somewhere else.
    if (mFading && want == mTo) {
        mTarget = kNoChannel;
        mPrimed = false;
        return;
    }

    // The request is for the channel that is simply sounding, and a catch-up was heading
    // elsewhere. Nothing of that channel has been heard, so there is nothing to unwind.
    if (!mFading && want == mFrom) {
        mTarget = kNoChannel;
        mPrimed = false;
        return;
    }

    // Somewhere new. Start, or restart, the catch-up toward it. An in-flight catch-up is
    // abandoned exactly by ceasing to feed it — the engine is left alone and nothing is
    // destroyed on this thread.
    //
    // A fade already running is deliberately LEFT TO FINISH rather than abandoned, which is the
    // one place this qualifies the "abandon the incoming engine" rule. Mid-fade the output is a
    // mix of two channels, and there is no third signal to cut to that is not a step: reverting
    // to the outgoing channel steps by the fade's travel, and cutting to the new channel is the
    // unprimed hard switch this whole design exists to avoid. So the fade completes on its own
    // 10 ms and the new fade starts from wherever it landed. The new request is still acted on
    // immediately — its catch-up begins in this same block, in parallel — so nothing is queued;
    // only the tail of a ramp already in flight plays out.
    beginCatchup(want, numFrames);
}

//------------------------------------------------------------------------
void ChannelRack::runCatchup(int numFrames)
{
    if (mTarget == kNoChannel)
        return;
    // beginCatchup() refuses to start one while a fade is running, and startFadeIfReady() clears
    // the target in the same breath as it starts a fade, so these two are never both live.
    assert(!mFading && "a catch-up and a fade are running at once; the model budget is not bounded");

    // What the audio thread is already committed to this block: one model for the sounding channel
    // at its detent, two while its dial is moving. The catch-up gets what is left of the budget
    // and not a sample more — see kSwitchModelBudget for why this is a total rather than a rate,
    // and for the measurement that set it.
    const double rate =
        engine::kSwitchModelBudget - static_cast<double>(mEngine[mFrom].boundBranches());

    if (rate <= engine::kCatchupMinRate) {
        // No headroom to catch up with. HOLD the switch: the outgoing channel keeps sounding and
        // the request stays live, exactly as it does when the target capture is not built yet.
        // Park the cursor one receptive field behind the write head so no history is lost and the
        // catch-up resumes at full speed the moment the headroom returns — which it does as soon
        // as the dial stops moving and the auto-detent collapses the second branch.
        long long start = mWritePos - mCatchupLag;
        if (start < 0)
            start = 0;
        if (start > mCursor)
            mCursor = start;
        return;
    }

    // The cursor consumes `rate` samples for every sample of real time. The write head advances by
    // numFrames in that same time, so the gap between them — one whole receptive field at the
    // moment of the stomp — closes at (rate - 1) samples per sample, and the catch-up lasts
    // R / ((rate - 1) * Fs).
    //
    // Feeding this block's live audio is NOT a separate step. It is simply the newest part of what
    // the cursor reaches, and treating it as separate is how this was wrong first time round: draw
    // (rate - 1) blocks from the ring and then feed the live block only once the ring is empty,
    // and the cursor advances by n * (rate - 1) per block against a write head advancing by n, so
    // the gap closes at (rate - 2) samples per sample — zero at rate 2, where the catch-up then
    // never finishes at all. The offline proof caught that; nothing about reading the code did.
    const long long budget =
        std::max<long long>(1, std::llround(static_cast<double>(numFrames) * rate));
    const long long feed = std::min(mWritePos - mCursor, budget);
    if (feed > 0) {
        feedFromRing(mEngine[mTarget], mCursor, feed);
        mCursor += feed;
    }
    if (mCursor < mWritePos)
        return; // still behind the live block; more of the ring next time

    // Drained. The incoming model has now seen exactly the input history the outgoing one has, so
    // from here it is bit-identical to one that had been running all session.
    //
    // The engine's own ready gate ramps in over the bypass ramp length the first time a channel
    // produces anything, and it has been ramping throughout the catch-up. Requiring it to be fully
    // open before the fade begins keeps "the fade runs between two exact signals" a structural
    // property rather than an arithmetic coincidence between two constants.
    mPrimed = mEngine[mTarget].fullyOpen();
}

//------------------------------------------------------------------------
void ChannelRack::startFadeIfReady()
{
    if (mTarget == kNoChannel || !mPrimed || mFading)
        return;
    mTo = mTarget;
    mFadeMix = 0.0;
    mFading = true;
    mTarget = kNoChannel;
    mPrimed = false;
}

//------------------------------------------------------------------------
void ChannelRack::mixFade(NAM_SAMPLE *dst, int numFrames)
{
    // Amplitude-complementary (a + b = 1). Both signals are the true responses of their models to
    // the same input by the time this runs, so the fade is not hiding anything and the only thing
    // the law has to do is not bulge in the middle.
    double w = mFadeMix;
    for (int i = 0; i < numFrames; ++i) {
        w += mFadeStep;
        if (w > 1.0)
            w = 1.0; // snapped to exactly 1.0, never left as accumulated residue
        dst[i] = (1.0 - w) * dst[i] + w * mMix[static_cast<size_t>(i)];
    }
    mFadeMix = w;

    if (mFadeMix >= 1.0) {
        mFrom = mTo;
        mFading = false;
        mFadeMix = 0.0;
    }
}

//------------------------------------------------------------------------
void ChannelRack::processNative(NAM_SAMPLE **in, NAM_SAMPLE **out, int numFrames)
{
    if (numFrames <= 0)
        return;
    NAM_SAMPLE *dst = out[0];

    // History first: everything below reads the ring, this block's own samples included.
    writeRing(in[0], numFrames);

    // A republished bank replaces every model pointer in that channel, so a switch in flight
    // toward it has been priming objects that are no longer bound. In this build each channel's
    // directory is loaded exactly once, at initialize(), so this cannot fire — but "cannot" is a
    // property of a call site somewhere else, and the failure it would cause is a silent
    // unprimed model, so it is handled here rather than assumed away.
    for (int c = 0; c < kChannelCount; ++c) {
        if (!mBankChanged[c])
            continue;
        if (mTarget == c) {
            mTarget = kNoChannel; // resolveRequest restarts it below, from the ring
            mPrimed = false;
        }
        if (mFading && mTo == c)
            reverseFade(); // fade back to the channel that is unaffected
    }

    resolveRequest(numFrames);
    startFadeIfReady();

    // The sounding channel processes the live block. Idle channels are not processed at all —
    // that is what makes four resident banks cost memory and nothing else.
    mEngine[mFrom].processNative(in, out, numFrames);

    if (mFading) {
        NAM_SAMPLE *mix = mMixPtr;
        mEngine[mTo].processNative(in, &mix, numFrames);
        mixFade(dst, numFrames);
    }

    runCatchup(numFrames);

    // Keep every channel nobody is listening to building the capture its own dial names, so a
    // switch lands on the right capture rather than waiting for it. setPriority() otherwise only
    // happens inside processNative(), which an idle engine never reaches.
    for (int c = 0; c < kChannelCount; ++c)
        if (c != mFrom && c != mTo && c != mTarget)
            mEngine[c].hintPriority();
}

} // namespace Rations
