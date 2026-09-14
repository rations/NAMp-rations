// CrossfadeEngine implementation. See crossfadeengine.h for why position is continuous.

#include "crossfadeengine.h"
#include "modelbank.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace Rations
{

namespace
{

inline double dbToLinear(double db)
{
    return std::pow(10.0, db / 20.0);
}

inline double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

} // namespace

//------------------------------------------------------------------------
void CrossfadeEngine::prepare(int maxNativeFrames, double nativeSampleRate)
{
    const size_t n = static_cast<size_t>(std::max(maxNativeFrames, engine::kChunk));
    mScratchA.assign(n, 0.0);
    mScratchB.assign(n, 0.0);
    mScratchIn.assign(n, 0.0);
    mNativeSampleRate = nativeSampleRate > 0.0 ? nativeSampleRate : kNativeSampleRate;

    // The gate that opens when the first entry becomes playable. Reuses the bypass ramp length —
    // it only has to be long enough not to click.
    const double readySamples = std::max(1.0, engine::kBypassRampMs * 0.001 * mNativeSampleRate);
    mReadyStep = 1.0 / readySamples;
}

//------------------------------------------------------------------------
double CrossfadeEngine::primedFraction(const Branch &branch)
{
    if (!branch.bound() || branch.prewarmSamples <= 0)
        return 1.0;
    return clamp01(static_cast<double>(branch.samplesLive) /
                   static_cast<double>(branch.prewarmSamples));
}

//------------------------------------------------------------------------
double CrossfadeEngine::entryGain(const BankEntry &entry) const
{
    if (mOutputMode == 1 && entry.hasLoudness) {
        // Normalized: bring each capture's measured loudness to a common target, per branch.
        return dbToLinear(engine::kNormalizedTargetDb - entry.loudnessDb);
    }
    if (mOutputMode == 2 && entry.hasOutputLevel) {
        // Calibrated: the capture's stated output level against the user's input calibration.
        return dbToLinear(entry.outputLevelDbu - mCalLevelDbu);
    }
    return 1.0;
}

//------------------------------------------------------------------------
// The input half of calibration. Independent of the output mode on purpose, and that is not an
// oversight in the design it is copied from: a capture states what level it was FED separately from
// what level it puts out, plenty of captures state one and not the other, and a player who has told
// the plug-in what their interface does should have that honoured whichever way they take the
// output. The upstream plug-in gates these two on different metadata fields for the same reason.
double CrossfadeEngine::entryInputGain(const BankEntry &entry) const
{
    if (!mCalibrateInput || !entry.hasInputLevel)
        return 1.0;
    return dbToLinear(mCalLevelDbu - entry.inputLevelDbu);
}

void CrossfadeEngine::refreshGains()
{
    if (!mBank)
        return;
    if (mA.bound()) {
        mA.gain = entryGain(mBank->entries[mA.index]);
        mA.inputGain = entryInputGain(mBank->entries[mA.index]);
    }
    if (mB.bound()) {
        mB.gain = entryGain(mBank->entries[mB.index]);
        mB.inputGain = entryInputGain(mBank->entries[mB.index]);
    }
}

//------------------------------------------------------------------------
void CrossfadeEngine::setOutputMode(int mode, double calLevelDbu, bool calibrateInput)
{
    mOutputMode = mode;
    mCalLevelDbu = calLevelDbu;
    mCalibrateInput = calibrateInput;
    refreshGains();
}

//------------------------------------------------------------------------
void CrossfadeEngine::bindOne(Branch &branch, int index)
{
    if (!mBank || index < 0 || index >= mBank->count || !mBank->entryReady(index)) {
        branch.index = -1;
        branch.model = nullptr;
        branch.samplesLive = 0;
        branch.prewarmSamples = 0;
        branch.gain = 1.0;
        branch.inputGain = 1.0;
        return;
    }
    if (branch.index == index && branch.model != nullptr)
        return; // already live; keep its accumulated priming
    const BankEntry &entry = mBank->entries[index];
    branch.index = index;
    branch.model = entry.model.get();
    branch.samplesLive = 0;
    branch.prewarmSamples = entry.prewarmSamples;
    branch.gain = entryGain(entry);
    branch.inputGain = entryInputGain(entry);
}

//------------------------------------------------------------------------
void CrossfadeEngine::bindBranches(int k, bool wantUpper)
{
    if (!mBank || mBank->count <= 0) {
        mA = Branch();
        mB = Branch();
        return;
    }
    const int last = mBank->count - 1;
    const int a = std::min(std::max(k, 0), last);
    // At a whole-numbered position with nowhere to travel, the upper branch carries weight zero
    // and is not needed. Dropping it is what makes the plug-in cost ONE model at rest and two only
    // while the knob is actually moving — the entire affordability argument for the feature. It is
    // safe precisely because its weight is exactly zero here, so nothing is being cut off.
    const int b = wantUpper ? std::min(a + 1, last) : a;

    // A branch that is already playing the entry we need keeps its accumulated priming; this is
    // what makes an integer crossing free. Crossing up, the old B becomes the new A and is already
    // fully live; only the far branch is new, and it enters at weight zero.
    if (mA.index != a) {
        if (mB.index == a && mB.bound()) {
            const Branch carried = mB;
            mB = mA;
            mA = carried;
        }
    }
    if (mA.index != a)
        bindOne(mA, a);
    if (b != a) {
        if (mB.index != b)
            bindOne(mB, b);
    } else {
        // Either at rest on a whole number, or at the top of the bank: nothing above to fade into.
        mB = Branch();
    }
}

//------------------------------------------------------------------------
bool CrossfadeEngine::pollBank()
{
    // Retry a bank that could not be handed back last time before taking on anything new.
    if (mHolding && mLoader && mLoader->retire(mHolding))
        mHolding = nullptr;

    if (!mLoader)
        return false;
    Bank *incoming = mLoader->takePending();
    if (!incoming)
        return false;

    Bank *outgoing = mBank;
    mBank = incoming;
    mA = Branch();
    mB = Branch();
    mHaveReady = false;
    // Do NOT slew up from zero to wherever the knob is. Slewing exists to smooth a knob being
    // turned; a bank arriving is not a knob turn. Restoring a project saved with the Gain knob at
    // capture 8 must come up ON capture 8, not sweep through the seven below it first.
    mSnapPosition = true;
    mDetenting = false;
    mIdleSamples = 0;

    if (outgoing) {
        // The audio thread never calls a destructor: that is a free(), which takes the allocator
        // lock. Hand it back, and keep hold of it if the queue is momentarily full.
        if (!mLoader->retire(outgoing)) {
            if (mHolding)
                mLoader->retire(mHolding); // best effort; at worst one bank waits a block longer
            mHolding = outgoing;
        }
    }
    return true;
}

//------------------------------------------------------------------------
Bank *CrossfadeEngine::releaseBank()
{
    Bank *bank = mBank;
    mBank = nullptr;
    mA = Branch();
    mB = Branch();
    if (mHolding) {
        ModelBank::destroyBank(mHolding);
        mHolding = nullptr;
    }
    return bank;
}

//------------------------------------------------------------------------
void CrossfadeEngine::setPositionNorm(double norm)
{
    const int count = mBank ? mBank->count : 0;
    if (count <= 1) {
        mKnobPos = 0.0;
        return;
    }
    mKnobPos = clamp01(norm) * static_cast<double>(count - 1);
}

//------------------------------------------------------------------------
bool CrossfadeEngine::hasInputLevel() const
{
    return mA.bound() && mBank && mBank->entries[mA.index].hasInputLevel;
}

double CrossfadeEngine::inputLevelDbu() const
{
    return hasInputLevel() ? mBank->entries[mA.index].inputLevelDbu : 0.0;
}

//------------------------------------------------------------------------
// The capture this engine's own dial names, and only if it is built. Deliberately NOT the
// nearest-built fallback that processNative() uses while a bank fills in behind the knob: that
// fallback exists so the channel already sounding keeps sounding, and applying it here would let
// a footswitch land the player on a capture they did not select.
int CrossfadeEngine::restIndex() const
{
    if (!mBank || mBank->count <= 0)
        return -1;
    int i = static_cast<int>(std::llround(mKnobPos));
    i = std::min(std::max(i, 0), mBank->count - 1);
    return mBank->entryReady(i) ? i : -1;
}

//------------------------------------------------------------------------
int CrossfadeEngine::restPrewarmSamples() const
{
    const int i = restIndex();
    return i < 0 ? 0 : mBank->entries[i].prewarmSamples;
}

//------------------------------------------------------------------------
void CrossfadeEngine::snapToRest()
{
    const int i = restIndex();
    if (i < 0)
        return;

    // Land ON the capture rather than travelling to it, and hold there. mPos is taken from the
    // index rather than from the knob because an idle channel is at rest by definition: the dial
    // may sit at 3.4, but nothing has been sweeping it, so the capture it names is 3 and exactly
    // one branch needs priming. Reusing the detent state to hold it there is not a shortcut — it
    // is the same state the auto-detent leaves behind, so the knob moving releases it in the one
    // place that already knows how.
    mSnapPosition = false;
    mPos = static_cast<double>(i);
    mLastKnob = mKnobPos;
    mIdleSamples = 0;
    mDetenting = false;
    mDetented = true;
    mDetentTarget = mPos;

    bindBranches(i, false);
}

//------------------------------------------------------------------------
void CrossfadeEngine::hintPriority()
{
    if (!mLoader || !mBank || mBank->count <= 0)
        return;
    int i = static_cast<int>(std::llround(mKnobPos));
    i = std::min(std::max(i, 0), mBank->count - 1);
    mLoader->setPriority(i);
}

//------------------------------------------------------------------------

double CrossfadeEngine::activeIndexNorm() const
{
    const int count = mBank ? mBank->count : 0;
    if (count <= 1)
        return 0.0;
    const double nearest = std::round(mPos);
    return clamp01(nearest / static_cast<double>(count - 1));
}

//------------------------------------------------------------------------
void CrossfadeEngine::processNative(NAM_SAMPLE **in, NAM_SAMPLE **out, int numFrames)
{
    const NAM_SAMPLE *src = in[0];
    NAM_SAMPLE *dst = out[0];

    // Highest entry the worker has finished. The knob is clamped to it, so a bank that is still
    // filling in behind the position plays what it has instead of dropping out.
    const int highest = mBank ? mBank->highestReady() : -1;
    mHaveReady = highest >= 0;
    if (!mHaveReady) {
        // Ramp to silence, never to dry signal: dry would be the wrong level and would jump the
        // moment a model landed.
        for (int i = 0; i < numFrames; ++i) {
            mReadyMix = std::max(0.0, mReadyMix - mReadyStep);
            dst[i] = src[i] * mReadyMix;
        }
        return;
    }

    if (mSnapPosition) {
        mSnapPosition = false;
        mPos = mKnobPos;
        mLastKnob = mKnobPos;
    }

    if (mBank && mBank->count > 1) {
        const double ceiling = static_cast<double>(highest);
        if (mKnobPos > ceiling)
            mKnobPos = ceiling;
        if (mPos > ceiling)
            mPos = ceiling;
    }

    // Slew rate: at least kSlewPerIndexMs, but never fast enough that crossing one whole index
    // takes less than a receptive field. Derived from the entries themselves rather than assumed,
    // because the entire crossfade argument is stated in terms of that number.
    const long long prewarm = std::max<long long>(mA.prewarmSamples, mB.prewarmSamples);
    if (prewarm != mPrewarmForSlew || mSlewStep == 0.0) {
        mPrewarmForSlew = prewarm;
        const double byMs = engine::kSlewPerIndexMs * 0.001 * mNativeSampleRate;
        const double perIndex = std::max(byMs, static_cast<double>(prewarm));
        mSlewStep = 1.0 / std::max(1.0, perIndex);
    }

    const long long idleLimit =
        static_cast<long long>(engine::kDetentIdleMs * 0.001 * mNativeSampleRate);
    const double glideSamples = std::max(1.0, engine::kDetentGlideMs * 0.001 * mNativeSampleRate);

    int done = 0;
    while (done < numFrames) {
        // --- decide where we are heading, and how fast -------------------------------------
        if (mKnobPos != mLastKnob) {
            // The knob moved: abandon any detent and follow it again.
            mLastKnob = mKnobPos;
            mIdleSamples = 0;
            mDetenting = false;
            mDetented = false;
        }

        double target = mKnobPos;
        double step = mSlewStep;
        if (!mDetenting && !mDetented && mPos == mKnobPos && mIdleSamples >= idleLimit &&
            mPos != std::round(mPos)) {
            // Auto-detent. The knob has been still long enough; settle onto a real capture so the
            // second branch can be dropped and the plug-in goes back to costing one model. The
            // glide is deliberately longer than the receptive field, so the capture being settled
            // onto is fully primed well before it reaches full weight.
            mDetenting = true;
            mDetentTarget = std::round(mPos);
            mDetentStep = std::max(1e-12, std::fabs(mDetentTarget - mPos) / glideSamples);
        }
        if (mDetenting || mDetented) {
            // Once detented, STAY there until the knob itself moves. Reverting to the raw knob
            // position would slew straight back off the capture, undoing the detent every time it
            // completed and leaving the plug-in permanently paying for two models.
            target = mDetentTarget;
            step = mDetenting ? mDetentStep : mSlewStep;
        }

        // At rest exactly on a capture, the upper branch has weight zero and is dropped.
        const bool atRest = (mPos == target) && (mPos == std::round(mPos));

        // --- bind branches for the current integer cell -------------------------------------
        // Whether an upper branch is wanted decides which cell the position names, so atRest is
        // settled FIRST and k derived from it. With an upper branch the cell is the interval
        // [k, k+1] the position sits inside, so k is a floor and cannot exceed count-2. Without
        // one there is no interval — the position IS a capture — and clamping to count-2 plays
        // the entry BELOW the one the knob is pointing at. That clamp used to be unconditional,
        // which made the top of the bank sound its neighbour: the position reached N-1, f was
        // computed as exactly the 1.0 that would have selected it, and was then discarded because
        // the branch that would have carried it was never bound. It stepped audibly on landing,
        // because on the way up that entry was already at full weight. The ready-clamp below had
        // the same shape and did the same thing, transiently, at the highest built entry while a
        // bank was still filling.
        int k;
        if (mBank->count <= 1) {
            k = 0;
        } else if (atRest) {
            k = std::min(std::max(static_cast<int>(std::llround(mPos)), 0), mBank->count - 1);
            k = std::min(k, std::max(highest, 0)); // clamp to what is actually built
        } else {
            k = std::min(std::max(static_cast<int>(std::floor(mPos)), 0), mBank->count - 2);
            k = std::min(k, std::max(highest - 1, 0));
        }
        bindBranches(k, !atRest);

        // If the branch we need is not ready yet, fall back to the nearest one that is, rather
        // than dropping out.
        if (!mA.bound()) {
            bindOne(mA, highest);
            mB = Branch();
        }

        // At rest, the branch left standing must be the capture the position NAMES — stated
        // against mPos, not against k, so that a mistake in deriving k is what it catches. The
        // assertion further down guards the crossing; only this one guards the binding, and
        // without it a position can quietly sound its neighbour. The second disjunct is the
        // fallback just above: when the named entry is not built yet, the highest built one is
        // deliberately sounded in its place.
        assert((!atRest || !mA.bound() ||
                mA.index == std::min(std::max(static_cast<int>(std::llround(mPos)), 0), highest) ||
                mA.index == highest) &&
               "at rest the bound branch is not the capture the position names");

        double f = mPos - static_cast<double>(k);
        if (!mB.bound())
            f = 0.0;
        f = clamp01(f);

        // --- the priming invariant ----------------------------------------------------------
        // A branch may not carry full weight until it has been fed one whole receptive field of
        // live input, or its contribution would not yet be the model's true response. Because the
        // slew rate above already guarantees one index takes at least that long, this clamp is
        // inactive during an ordinary sweep; it earns its keep on the first block after a branch
        // is bound, and after a jump to a distant position.
        const double fMax = primedFraction(mB);
        const double fMin = 1.0 - primedFraction(mA);
        if (fMin <= fMax)
            f = std::min(std::max(f, fMin), fMax);

        // --- how many samples may share this sub-chunk ---------------------------------------
        // Bounded by: the fixed model chunk (so no model is ever handed more frames than it was
        // Reset with), what is left of the block, and the distance to the next integer crossing
        // or to the target, so that k is constant and the weight ramp is linear across the
        // sub-chunk.
        int n = std::min(engine::kChunk, numFrames - done);
        const double delta = target - mPos;
        if (std::fabs(delta) > 1e-12 && step > 0.0) {
            const double toTarget = std::ceil(std::fabs(delta) / step);
            n = static_cast<int>(std::min<double>(n, std::max(1.0, toTarget)));
            const double edge = delta > 0.0 ? std::floor(mPos) + 1.0 : std::floor(mPos);
            const double toEdge = std::fabs(edge - mPos);
            if (toEdge > 0.0) {
                const double samples = std::ceil(toEdge / step);
                n = static_cast<int>(std::min<double>(n, std::max(1.0, samples)));
            }
        }

        const NAM_SAMPLE *chunkIn = src + done;
        NAM_SAMPLE *chunkOut = dst + done;

        // --- run BOTH bound branches ---------------------------------------------------------
        // Never `if (weight > 0) process(...)`. That looks like free optimisation and silently
        // deletes the priming that makes the crossfade click-free, turning it straight back into
        // a hard switch. A bound branch is processed every chunk, always.
        NAM_SAMPLE *inPtr = const_cast<NAM_SAMPLE *>(chunkIn);
        // Input calibration, if any, scaled per branch into a scratch buffer. The common case is
        // calibration off, where every inputGain is exactly 1.0 and this costs one comparison per
        // branch per sub-chunk and touches no memory — which is what keeps the default path, and
        // every measurement taken on it, exactly what it was.
        auto branchInput = [&](const Branch &branch) -> NAM_SAMPLE * {
            if (branch.inputGain == 1.0)
                return inPtr;
            NAM_SAMPLE *scaled = mScratchIn.data();
            for (int i = 0; i < n; ++i)
                scaled[i] = chunkIn[i] * branch.inputGain;
            return scaled;
        };
        if (mA.bound()) {
            NAM_SAMPLE *inA = branchInput(mA);
            NAM_SAMPLE *outA = mScratchA.data();
            mA.model->process(&inA, &outA, n);
            mA.samplesLive += n;
        }
        if (mB.bound()) {
            // Recomputed rather than reused: A and B are different captures and may state
            // different input levels, and the buffer above holds A's scaling by now.
            NAM_SAMPLE *inB = branchInput(mB);
            NAM_SAMPLE *outB = mScratchB.data();
            mB.model->process(&inB, &outB, n);
            mB.samplesLive += n;
        }

        // --- mix, with the weight ramped per sample ------------------------------------------
        // Amplitude-complementary (a + b = 1), not equal-power. Adjacent captures of the same amp
        // fed the same input are strongly correlated, so a^2 + b^2 = 1 would produce a mid-fade
        // level bump of up to +3 dB.
        double pos = mPos;
        double fCur = f;
        const double posEnd = mPos + std::min(std::fabs(delta), step * n) * (delta < 0 ? -1 : 1);
        const double fEnd = clamp01(posEnd - static_cast<double>(k));
        const double fStep = mB.bound() && n > 0 ? (fEnd - fCur) / static_cast<double>(n) : 0.0;

        if (mA.bound() && mB.bound()) {
            const double gA = mA.gain;
            const double gB = mB.gain;
            for (int i = 0; i < n; ++i) {
                // The weight at the END of this sample's own interval, which is where the
                // position itself has advanced to once the sample is written. Taking the weight
                // at the START instead leaves the last sample of a sub-chunk one step short of
                // the boundary, so a retiring branch's final applied weight is fStep rather than
                // exactly zero — and exact zero at the swap is the invariant the whole no-click
                // argument rests on.
                fCur += fStep;
                const double w = fCur;
                chunkOut[i] = (1.0 - w) * gA * mScratchA[static_cast<size_t>(i)] +
                              w * gB * mScratchB[static_cast<size_t>(i)];
            }
        } else if (mA.bound()) {
            const double gA = mA.gain;
            for (int i = 0; i < n; ++i)
                chunkOut[i] = gA * mScratchA[static_cast<size_t>(i)];
        } else {
            std::memset(chunkOut, 0, static_cast<size_t>(n) * sizeof(NAM_SAMPLE));
        }

        // Open the ready gate.
        for (int i = 0; i < n; ++i) {
            mReadyMix = std::min(1.0, mReadyMix + mReadyStep);
            chunkOut[i] *= mReadyMix;
        }

        // --- advance the position -------------------------------------------------------------
        pos = mPos;
        if (std::fabs(delta) <= step * n) {
            // Snap EXACTLY onto the target. Leaving floating-point residue here would permanently
            // defeat branch collapse — a silent, permanent doubling of CPU cost — and would weaken
            // the zero-weight swap that makes a crossing inaudible.
            pos = target;
        } else {
            pos += (delta > 0.0 ? 1.0 : -1.0) * step * n;
            // n was chosen to travel no further than the current cell's edge, but it was chosen
            // with ceil(), which can carry the advance a fraction of a sample past it. Snap onto
            // the edge for the same reason the target is snapped: a branch that retires at a
            // crossing must retire at exactly zero weight, not at one step of residue. Guarded on
            // the edge being ahead of the position, because descending from an exact integer the
            // "edge" IS the position and snapping to it would freeze the sweep.
            const double edge = delta > 0.0 ? std::floor(mPos) + 1.0 : std::floor(mPos);
            if (delta > 0.0 ? (edge > mPos && pos > edge) : (edge < mPos && pos < edge))
                pos = edge;
        }
        mPos = pos;

        if (mDetenting && mPos == mDetentTarget) {
            mDetenting = false;
            mDetented = true;
        }
        if (mPos == target && !mDetenting)
            mIdleSamples += n;

        // The weight of a branch about to be dropped must be exactly zero at the crossing. This is
        // the invariant the whole no-click argument rests on, so it is checked, not assumed.
        assert((!mB.bound() || mPos != std::floor(mPos) || std::fabs(mPos - k) < 1e-12 ||
                std::fabs(mPos - k - 1.0) < 1e-12) &&
               "position left an integer cell without landing on a cell boundary");

        done += n;
    }

    // Tell the worker which entry to build next, so a bank that is still loading finishes the one
    // under the knob first.
    if (mLoader)
        mLoader->setPriority(static_cast<int>(std::round(mPos)));
}

} // namespace Rations
