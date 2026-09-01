// ChannelRack implementation. See channelrack.h for why a channel switch is a priming problem
// rather than a mixing problem.

#include "channelrack.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <thread>

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

    for (int c = 0; c < kChannelCount; ++c) {
        mEngine[c].prepare(maxNativeFrames, mNativeSampleRate);
        // prepare() rebuilds each engine's own state, so whatever it had applied of the output
        // section is gone with it. Rewinding the applied generation is what makes every channel
        // re-read it on its next visit; the processor also republishes after prepare(), and the
        // two together mean neither ordering can leave an engine at its default compensation.
        mAppliedOutputGen[c].store(mOutputGen.load(std::memory_order_relaxed) - 1u,
                                   std::memory_order_relaxed);
    }

    // The ring is a power of two so the wrap is a mask. Sized by engineconfig against one whole
    // receptive field plus what arrives while the catch-up drains it — see kInputRingSamples.
    static_assert((engine::kInputRingSamples & (engine::kInputRingSamples - 1)) == 0,
                  "the input ring is indexed with a mask, so its size must be a power of two");
    mRing.assign(static_cast<size_t>(engine::kInputRingSamples), 0.0);
    mWritePos = 0;
    mWriteHead.store(0, std::memory_order_relaxed);
    mCursor = 0;

    mFeed.assign(static_cast<size_t>(engine::kChunk), 0.0);
    mSink.assign(std::max(frames, static_cast<size_t>(engine::kChunk)), 0.0);
    mMix.assign(frames, 0.0);
    mMixPtr = mMix.data();

    // The worker's own staging, separate from the audio thread's. Two threads feeding two
    // different engines through the same scratch buffer would be a data race in the one place
    // that looks most like shared plumbing.
    mPrimeFeed.assign(static_cast<size_t>(engine::kChunk), 0.0);
    mPrimeSink.assign(static_cast<size_t>(engine::kChunk), 0.0);

    // The sounding channel belongs to the audio thread from the start; everything else belongs to
    // the worker. prepare() runs before start(), so there is no thread to race with here.
    for (int c = 0; c < kChannelCount; ++c) {
        mOwner[c].store(c == mFrom ? kOwnerRT : kOwnerWorker, std::memory_order_relaxed);
        mPrimedThrough[c].store(0, std::memory_order_relaxed);
        mPrimedFrom[c].store(0, std::memory_order_relaxed);
        mWarm[c].store(false, std::memory_order_relaxed);
        mRestPrewarmPub[c].store(0, std::memory_order_relaxed);
        mRestIndexPub[c].store(-1, std::memory_order_relaxed);
    }
    mWorstPrimeLag.store(0, std::memory_order_relaxed);

    const double fadeSamples = std::max(1.0, engine::kChannelFadeMs * 0.001 * mNativeSampleRate);
    mFadeStep = 1.0 / fadeSamples;

    // dB per second -> a per-sample amplitude multiplier, and its reciprocal for the way down.
    mLevelStepUp = std::pow(10.0, engine::kLevelRampDbPerSec / (20.0 * mNativeSampleRate));
    mLevelStepDown = 1.0 / mLevelStepUp;
    for (int c = 0; c < kChannelCount; ++c)
        mLevelCurrent[c] = mLevelTarget[c];

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

    // One prime thread for all three idle channels rather than one each. Three models running
    // continuously is RTF ~0.15 on this machine, which leaves the thread better than 80% idle,
    // and three threads would triple the wake count and the scheduling jitter to buy nothing.
    if (!mPrimeThread.joinable()) {
        mPrimeRunning.store(true, std::memory_order_release);
        mPrimeThread = std::thread([this] { primeLoop(); });
    }
}

void ChannelRack::stop()
{
    // The prime thread first: it touches the engines, and the loaders own the models underneath
    // them. Stopping in the other order would leave it feeding a bank being torn down.
    mPrimeRunning.store(false, std::memory_order_release);
    if (mPrimeThread.joinable())
        mPrimeThread.join();

    for (int c = 0; c < kChannelCount; ++c)
        mLoader[c].stop();
}

//------------------------------------------------------------------------
// One worker per channel, all four building at once. That is what makes the build breadth-first
// without any cross-channel scheduling: each worker serves its own channel's dial position first
// (the engine hints it every block, sounding or not), so every channel becomes switchable after
// roughly one model's build time rather than after all thirty-five are done.
void ChannelRack::loadChannel(Channel ch, const std::string &path, bool isDirectory, double slim,
                              int maxBufferSize)
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    mLoadRequested[i] = true;
    if (isDirectory)
        mLoader[i].loadDirectory(path, slim, maxBufferSize);
    else
        mLoader[i].loadFile(path, slim, maxBufferSize);
}

//------------------------------------------------------------------------
// A Slim change rebuilds every entry of every loaded bank, because this plug-in bakes the size
// choice in at construction: it picks the submodel itself in buildCaptureModel rather than going
// through the DSP core's container, so there is no size to set on a built model. ModelBank's
// Rebuild job does it from the parsed sources it already holds, so there is no file I/O and no
// JSON re-parse — but there is a create_dsp and a Reset per capture, and Reset prewarms.
//
// Two costs, and both are visible to the player rather than merely paid. The new bank is published
// before any entry is ready, so each channel falls to its ramped-silence gate until its priority
// entry lands. And a republish is one of the things that breaks a channel's unbroken receptive
// field, so all four go cold and the next switch takes the full on-thread catch-up. That is the
// right trade for a control set once when a rig is assembled, and it is the reason the editor
// sends this on the knob's RELEASE rather than on every step of a drag.
//
// Channels that were never loaded are skipped rather than asked: a Rebuild on a bank with no
// sources returns without publishing, so it would be harmless — but "harmless" is not a reason to
// wake three idle workers.
void ChannelRack::setSlim(double slim, int maxBufferSize)
{
    for (int i = 0; i < kChannelCount; ++i)
        if (mLoadRequested[i])
            mLoader[i].setSlim(slim, maxBufferSize);
}

//------------------------------------------------------------------------
// PUBLISHED, not applied — see setPositionNorm below, which does the same thing for the same
// reason. Callable from either thread, because a radio click on the settings page arrives as a host
// parameter change on the audio thread while a load or a state restore arrives on the message
// thread, and neither of them may write into an engine the prime worker owns.
//
// Release on the generation counter, acquire on the read in applyOutputMode: the three values above
// must be visible to any thread that observes the new generation.
void ChannelRack::setOutputMode(int mode, double calLevelDbu, bool calibrateInput)
{
    mOutputMode.store(mode, std::memory_order_relaxed);
    mCalLevelDbu.store(calLevelDbu, std::memory_order_relaxed);
    mCalibrateInput.store(calibrateInput, std::memory_order_relaxed);
    mOutputGen.fetch_add(1, std::memory_order_release);
}

//------------------------------------------------------------------------
bool ChannelRack::applyOutputMode(int channel)
{
    const unsigned gen = mOutputGen.load(std::memory_order_acquire);
    if (mAppliedOutputGen[channel].load(std::memory_order_relaxed) == gen)
        return false;
    mAppliedOutputGen[channel].store(gen, std::memory_order_relaxed);
    mEngine[channel].setOutputMode(mOutputMode.load(std::memory_order_relaxed),
                                   mCalLevelDbu.load(std::memory_order_relaxed),
                                   mCalibrateInput.load(std::memory_order_relaxed));
    return true;
}

//------------------------------------------------------------------------
std::vector<std::string> ChannelRack::captureNames(Channel ch) const
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    return mLoader[i].captureNames();
}

//------------------------------------------------------------------------
CaptureLevels ChannelRack::captureLevels(Channel ch) const
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    return mLoader[i].captureLevels();
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
    // Only the channels this thread owns. A worker-owned engine has its bank polled by the
    // worker, in the same tick that it primes it - which is also where a republish makes that
    // channel go cold, so the two can never be observed out of step.
    for (int c = 0; c < kChannelCount; ++c)
        mBankChanged[c] = rtOwns(c) ? mEngine[c].pollBank() : false;
}

//------------------------------------------------------------------------
// PUBLISHED, not applied. The processor hands the rack all four dial positions every block, and
// three of those engines belong to the worker; writing straight into them from the audio thread
// is exactly the shared access the ownership rule exists to forbid. Each position is stored here
// and applied by whichever thread owns that engine.
void ChannelRack::setPositionNorm(Channel ch, double norm)
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    mPosNorm[i].store(norm, std::memory_order_relaxed);

    // A channel in the middle of a switch keeps the position beginCatchup() gave it and does not
    // follow its dial until the switch is over. D5 says an incoming channel snaps to a whole
    // capture so that exactly ONE branch needs priming rather than two, and applying a moving dial
    // to it breaks that invariant silently: the engine un-detents, binds a second branch, and the
    // fade that follows costs three models instead of two in a block that has no room for a third.
    //
    // Found by measurement, not by reading. The hole predates the prime worker - the budget bounds
    // the catch-up and has never bounded the fade - but it was unreachable while a switch under a
    // moving dial was held for over a second, because the auto-detent always collapsed the extra
    // branch first. Making switches fast is what made it reachable, and it xruns.
    //
    // Freezing the dial for the ~13 ms a switch takes costs nothing anybody can hear: the capture
    // the switch lands on is chosen from this same published value at the moment of the claim.
    const bool midSwitch = (i == mTarget) || (mFading && i == mTo);
    if (rtOwns(i) && !midSwitch)
        mEngine[i].setPositionNorm(norm);
}

//------------------------------------------------------------------------
// The per-channel trim. Stored as a target; the ramp toward it runs in applyLevel() below.
void ChannelRack::setLevel(Channel ch, double linearGain)
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    mLevelTarget[i] = linearGain;
}

//------------------------------------------------------------------------
// One channel's trim, ramped, applied to that channel's own output block.
//
// The ramp exists for the slider being dragged, not for the switch: a channel that is about to
// start sounding has its ramp SNAPPED to target when the fade begins (see startFadeIfReady), so a
// channel change opens at the level the user set rather than sliding up to it over 15 ms.
void ChannelRack::applyLevel(int channel, NAM_SAMPLE *buf, int numFrames)
{
    double g = mLevelCurrent[channel];
    const double target = mLevelTarget[channel];
    if (g == target) {
        if (g == 1.0)
            return; // unity and not moving: the ordinary case costs one compare
        for (int i = 0; i < numFrames; ++i)
            buf[i] *= g;
        return;
    }
    // Multiplicative, so the ramp covers a constant number of dB per second: the same musical
    // change takes the same time wherever in the range it starts, and this file needs to know
    // nothing about what the trim's range in dB actually is - that lives with the parameter.
    for (int i = 0; i < numFrames; ++i) {
        if (g < target)
            g = std::min(target, g * mLevelStepUp);
        else
            g = std::max(target, g * mLevelStepDown);
        buf[i] *= g;
    }
    mLevelCurrent[channel] = g;
}

//------------------------------------------------------------------------
bool ChannelRack::warm(Channel ch) const
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    return mWarm[i].load(std::memory_order_acquire);
}

//------------------------------------------------------------------------
// The audio thread asking for a channel. Never waits: a claim that has not been acknowledged
// yet returns false and the caller holds the switch for another block, which is the same thing
// the rack already does when the target capture is not built.
bool ChannelRack::claimForRT(int c)
{
    int owner = mOwner[c].load(std::memory_order_acquire);
    if (owner == kOwnerRT)
        return true;
    if (owner == kOwnerWorker) {
        // Only ever Worker -> ClaimedByRT from this thread. compare_exchange rather than a bare
        // store so a release the worker made in the same instant cannot be overwritten.
        mOwner[c].compare_exchange_strong(owner, kOwnerClaimedByRT, std::memory_order_acq_rel,
                                          std::memory_order_acquire);
    }
    return false;
}

//------------------------------------------------------------------------
// Handing a channel back. Its engine is exact as of the write head at this instant, so the
// worker is told where it got to and continues from there rather than priming from scratch.
void ChannelRack::releaseToWorker(int c)
{
    // ONLY a channel this thread actually owns. A channel in kOwnerClaimedByRT is a claim in
    // flight, not an unused engine: releasing it would undo the claim that the switch is waiting
    // on, and since the claim is re-made next block the two would alternate forever and the
    // switch would never arrive. The offline switch proof caught exactly that, and nothing about
    // reading this loop did.
    if (mOwner[c].load(std::memory_order_relaxed) != kOwnerRT)
        return;
    // Deliberately conservative: the priming run restarts NOW rather than claiming the engine is
    // already exact. It usually is - a channel that has just finished fading out was sounding, so
    // it has heard everything - but a channel that was claimed and then turned out to be
    // unusable was not fed at all while the audio thread held it, and the two cases are not
    // distinguishable here. Re-priming a receptive field costs the worker about seven
    // milliseconds; getting this wrong costs a click.
    mPrimedThrough[c].store(mWritePos, std::memory_order_relaxed);
    mPrimedFrom[c].store(mWritePos, std::memory_order_relaxed);
    mWarm[c].store(false, std::memory_order_relaxed);
    mOwner[c].store(kOwnerWorker, std::memory_order_release);
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
    // Released AFTER the samples are written, so a worker that acquires this head is guaranteed
    // to see every sample it counts. This store is the entire signal from the audio thread to the
    // prime worker - there is no condition variable, no notify and no syscall on this path.
    mWriteHead.store(mWritePos, std::memory_order_release);
}

//------------------------------------------------------------------------
void ChannelRack::feedFromRing(CrossfadeEngine &target, long long from, long long count,
                               NAM_SAMPLE *feed, NAM_SAMPLE *sink)
{
    const size_t mask = mRing.size() - 1;
    long long pos = from;
    long long left = count;
    while (left > 0) {
        const int n = static_cast<int>(std::min<long long>(left, engine::kChunk));
        for (int i = 0; i < n; ++i)
            feed[static_cast<size_t>(i)] = mRing[static_cast<size_t>(pos + i) & mask];
        NAM_SAMPLE *in = feed;
        NAM_SAMPLE *out = sink;
        // The output goes nowhere. What matters is that the model's convolution history now holds
        // these samples, which is the whole of the state a feed-forward network has.
        target.processNative(&in, &out, n);
        pos += n;
        left -= n;
    }
}

//------------------------------------------------------------------------
// The prime worker. One thread, all the channels the audio thread is not using, keeping each one
// fed with the same input the audio thread is hearing so that its convolution history is current
// and a switch to it has nothing left to prime.
//
// It is woken by nothing. RT publishes the ring's write head and the worker watches it on a tick
// well under a block period, which keeps the entire coupling between the two threads to one
// release store and one acquire load. A condition variable would put a futex on the audio path
// to save a thread wake that costs nothing.
void ChannelRack::primeLoop()
{
    while (mPrimeRunning.load(std::memory_order_acquire)) {
        const long long head = mWriteHead.load(std::memory_order_acquire);

        for (int c = 0; c < kChannelCount; ++c) {
            const int owner = mOwner[c].load(std::memory_order_acquire);

            // A claim is acknowledged BETWEEN channels and never inside one. That is the whole
            // safety property: at the instant the audio thread observes kOwnerRT, this thread is
            // provably not inside that engine, and it will not enter it again because the loop
            // above will not see kOwnerWorker for it any more.
            if (owner == kOwnerClaimedByRT) {
                // Warmth is deliberately NOT cleared here. The engine is exact as of
                // mPrimedThrough[c] at this instant, and that fact is the entire value of the
                // handover: the audio thread reads both after it observes kOwnerRT, and the
                // release store below is what makes them visible to it. Phase A cleared warmth
                // here to keep the handover conservative while only the concurrency was under
                // test; leaving it set is the change that makes the switch fast.
                mOwner[c].store(kOwnerRT, std::memory_order_release);
                continue;
            }
            if (owner != kOwnerWorker)
                continue;

            primeChannel(c, head);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(engine::kPrimeTickUs));
    }
}

//------------------------------------------------------------------------
// One idle channel, brought up to the write head. Everything the audio thread would have done for
// this engine is done here instead, because the audio thread is not allowed to touch it.
void ChannelRack::primeChannel(int c, long long head)
{
    // Anything that makes the history this engine has accumulated worthless. Collected as one
    // flag because they all have the same consequence - the priming run has to start again from a
    // whole receptive field - and handling them separately is how one of them gets forgotten.
    bool invalidated = false;

    // A republished bank replaces every model pointer in this channel.
    if (mEngine[c].pollBank())
        invalidated = true;

    // The output section, applied here for the same reason the dial is: this engine belongs to
    // this thread. It counts as an invalidation because of the INPUT half of it — turning input
    // calibration on or off, or moving the calibration level, changes the level of the samples
    // this engine's models are fed, so every sample primed before it was primed against input
    // that no longer exists. A channel that reported itself warm on that work would complete its
    // switch and be wrong, which is a correctness problem rather than a slow one.
    if (applyOutputMode(c))
        invalidated = true;

    // The dial, applied here rather than by the audio thread that was handed it. An idle channel
    // whose dial has crossed into a different capture is now bound to a model with no history at
    // all, which is the same situation as never having been primed.
    const int wasIndex = mEngine[c].restIndex();
    mEngine[c].setPositionNorm(mPosNorm[c].load(std::memory_order_relaxed));
    mEngine[c].hintPriority();

    const int prewarm = mEngine[c].restPrewarmSamples();
    const int index = mEngine[c].restIndex();
    mRestPrewarmPub[c].store(prewarm, std::memory_order_relaxed);
    mRestIndexPub[c].store(index, std::memory_order_relaxed);
    if (index != wasIndex)
        invalidated = true;

    // Nothing to bind yet: the capture this channel's dial names has not been built. Priming an
    // engine with no model would feed the ring to nobody.
    if (prewarm <= 0 || index < 0) {
        mWarm[c].store(false, std::memory_order_relaxed);
        mPrimedThrough[c].store(head, std::memory_order_relaxed);
        mPrimedFrom[c].store(head, std::memory_order_relaxed);
        return;
    }

    // Park on the capture a switch would land on. An idle channel is always at rest, so this
    // costs one bound branch and there is exactly one model to keep warm rather than two.
    mEngine[c].snapToRest();

    long long from = mPrimedThrough[c].load(std::memory_order_relaxed);

    // How far behind the worker has fallen. Recorded rather than reasoned about, because it is
    // the number kInputRingSamples has to be sized against: a worker lapped by the ring has lost
    // the history it needed and the channel it was keeping warm goes cold.
    const long long lag = head - from;
    long long worst = mWorstPrimeLag.load(std::memory_order_relaxed);
    while (lag > worst &&
           !mWorstPrimeLag.compare_exchange_weak(worst, lag, std::memory_order_relaxed))
        ;

    // Lapped, or never started. The samples this engine still needed have been written over, so
    // the only recoverable state is the last receptive field.
    const long long oldest = head - static_cast<long long>(mRing.size());
    if (from < oldest || from > head)
        invalidated = true;

    if (invalidated) {
        from = std::max<long long>(0, head - prewarm);
        mPrimedFrom[c].store(from, std::memory_order_relaxed);
        mWarm[c].store(false, std::memory_order_relaxed);
    }

    const long long count = head - from;
    if (count > 0) {
        feedFromRing(mEngine[c], from, count, mPrimeFeed.data(), mPrimeSink.data());
        mPrimedThrough[c].store(head, std::memory_order_relaxed);
    }

    // Warm once the engine has consumed an UNBROKEN receptive field ending at the write head.
    // Measured from where the current run began, not from the last tick: the worker normally
    // feeds a few hundred samples per tick, so comparing a tick's own gap against the receptive
    // field would mean warmth was never reached at all - which is exactly what the first version
    // of this did, silently, and the race test caught by asking whether anything was ever warm.
    const long long run = head - mPrimedFrom[c].load(std::memory_order_relaxed);
    if (run >= prewarm)
        mWarm[c].store(mEngine[c].fullyOpen(), std::memory_order_release);
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

    // Is this channel even switchable? Asked of the worker's published copy rather than of the
    // engine, because the engine is not ours yet and the answer decides whether to claim it at
    // all. Zero means the capture this channel's dial names is not built, and the answer to that
    // is to HOLD the request: the audio does not move, the LED does not move, and the next block
    // tries again. Sounding a neighbouring capture instead would put an amp under the player's
    // hands that they did not select, which is worse than a switch that arrives an instant late.
    if (!rtOwns(ch) && mRestPrewarmPub[ch].load(std::memory_order_relaxed) <= 0)
        return;

    // Take the engine off the worker. Never waits: until the worker acknowledges, the switch is
    // simply held for another block - the same hold as a capture that is not built yet, and a
    // path this function already had.
    if (!claimForRT(ch))
        return;

    const int prewarm = mEngine[ch].restPrewarmSamples();
    if (prewarm <= 0)
        return;

    // What the worker achieved before it let go, read only now that this thread owns the engine.
    // Both were published before the worker's release store on mOwner, and claimForRT() observed
    // that store with acquire, so these two reads see a consistent snapshot of what it left.
    const bool wasWarm = mWarm[ch].load(std::memory_order_relaxed);
    const long long primedThrough = mPrimedThrough[ch].load(std::memory_order_relaxed);
    const int primedIndex = mEngine[ch].restIndex();
    mWarm[ch].store(false, std::memory_order_relaxed); // consumed; this thread owns it now

    // The dial this channel was handed while the worker owned it. Applied now that it is ours,
    // so a switch lands on the capture the player's dial actually names.
    mEngine[ch].setPositionNorm(mPosNorm[ch].load(std::memory_order_relaxed));
    mEngine[ch].snapToRest();
    mTarget = ch;
    mPrimed = false;
    // Remembered, because a held catch-up parks the cursor at exactly this lag every block and
    // the entry it was read from may not be the one bound by then.
    mCatchupLag = prewarm;

    // A dial that moved between the worker's last tick and this claim binds a DIFFERENT capture,
    // and bindBranches only preserves a branch's priming when the index is unchanged. So the
    // model this thread now holds may be a fresh one with no history at all, and the warmth the
    // worker reported was about a model that is no longer bound. Compared rather than assumed,
    // because the failure it prevents is silent: an unprimed model faded into as though it were
    // exact, which is the click this whole design exists to avoid.
    const bool stillTheSameCapture = mEngine[ch].restIndex() == primedIndex;

    long long start;
    if (wasWarm && stillTheSameCapture && primedThrough <= mWritePos) {
        // The worker has already fed this model every sample up to primedThrough, so it is exact
        // there and the only thing left is the handful of samples that arrived while the handover
        // was happening - a block or two, against a receptive field of six thousand. This one
        // line is what turns a 340 ms switch into a fast one; everything else in this phase
        // exists to make it safe to write.
        start = primedThrough;
    } else {
        // Cold: no warmth, a rebound capture, or a published position this thread cannot trust.
        // Lag the cursor one whole receptive field behind the live block, which is exactly what
        // this function did before there was a worker. Slower, and correct.
        start = mWritePos - numFrames - prewarm;
    }

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
        const int restIndex = rtOwns(want) ? mEngine[want].restIndex()
                                           : mRestIndexPub[want].load(std::memory_order_relaxed);
        if (restIndex < 0)
            return; // held
        if (!claimForRT(want))
            return; // held one block while the worker lets go
        mEngine[want].setPositionNorm(mPosNorm[want].load(std::memory_order_relaxed));
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
    assert(!mFading &&
           "a catch-up and a fade are running at once; the model budget is not bounded");

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
        feedFromRing(mEngine[mTarget], mCursor, feed, mFeed.data(), mSink.data());
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
    // The incoming channel starts at the level the player set for it, not at whatever its ramp
    // was left holding when it last stopped sounding. Ramping here would put a 15 ms level slide
    // on top of every channel change, which is the one thing this feature must not add to the
    // switch.
    mLevelCurrent[mTo] = mLevelTarget[mTo];
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

    // The output section, for the channels this thread owns. The worker does the same for its own
    // in primeChannel, so between them every engine picks up a change within a block or a tick;
    // neither thread ever writes into an engine the other holds.
    for (int c = 0; c < kChannelCount; ++c) {
        if (rtOwns(c))
            applyOutputMode(c);
    }

    resolveRequest(numFrames);
    startFadeIfReady();

    // The sounding channel processes the live block. No idle channel is processed on THIS thread,
    // which is what keeps the audio thread's steady-state cost at one model however many banks
    // are resident. The prime worker does feed the idle three, on its own thread, so that they
    // are already exact when the footswitch is stomped.
    mEngine[mFrom].processNative(in, out, numFrames);
    applyLevel(mFrom, dst, numFrames);

    if (mFading) {
        NAM_SAMPLE *mix = mMixPtr;
        mEngine[mTo].processNative(in, &mix, numFrames);
        // Each channel is trimmed BEFORE the fade mixes them, so a switch between two channels
        // the player has set to different levels travels between those levels over the fade
        // rather than stepping between them when mFrom changes.
        applyLevel(mTo, mMix.data(), numFrames);
        mixFade(dst, numFrames);
    }

    runCatchup(numFrames);

    // Hand back everything this thread is no longer using. Three cases end up here: the channel
    // that has just finished fading out, a catch-up that was abandoned when the player stomped
    // somewhere else, and a claim that turned out not to be usable because the capture was not
    // built after all. All three are the same act - this thread is done with that engine - so
    // they are one loop rather than three release calls scattered through the branches above,
    // which is how a channel ends up owned by nobody and silently never primed again.
    //
    // Keeping idle channels building the capture their own dial names moved here with them: the
    // worker hints priority for what it owns, in the same tick it primes it.
    for (int c = 0; c < kChannelCount; ++c) {
        if (c == mFrom || (mFading && c == mTo) || c == mTarget)
            continue;
        // The channel the player is ASKING for is never handed back, even though nothing is using
        // it yet. A claim is acknowledged by the worker within half a millisecond and a block is
        // nearly three, so without this the acknowledgement usually lands before the end of the
        // very block that made the claim - and this loop would then hand the engine straight back,
        // the next block would claim it again, and the switch would never arrive. The offline
        // switch proof caught it; the loop reads perfectly correct without it.
        if (c == mRequested)
            continue;
        releaseToWorker(c);
    }
}

} // namespace Rations
