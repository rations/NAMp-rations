// CrossfadeEngine — the model stage, at the native rate.
//
// The Gain knob is a CONTINUOUS POSITION p in [0, N-1] across the bank, not an index. With
// k = floor(p) and f = p - k, branch A plays entry k at weight 1-f and branch B plays entry k+1 at
// weight f. That formulation is the whole design, and it buys three things:
//
//   * Every integer crossing happens when one branch's weight is EXACTLY ZERO. Swapping a branch
//     at zero weight cannot produce a discontinuity, so the crossfade is click-free structurally
//     rather than because some fade curve was tuned until the click stopped being audible.
//   * There is no pending-index bookkeeping, no reverse-direction special case, and no chained
//     fade to lag behind a knob that turns back on itself. Position is just a number that moves.
//   * Cost is two models while the knob moves and one at rest, because at an integer position the
//     second branch is unbound.
//
// The reason priming is free: these captures run on the A2 fast-path WaveNet, which is strictly
// feed-forward — per-layer convolution history rings and no recurrent state anywhere. Its output
// at sample t is a pure function of the input window [t - receptiveField, t]. So a model that has
// been fed live input for one receptive field is bit-identical to one that had been running since
// the session began. Feeding the incoming branch during the fade IS the priming; there is nothing
// else to do and nothing to catch up.
//
// Real-time contract: no allocation, no locks, no file I/O, no logging, no destructors. Retired
// banks are handed back to the worker through a lock-free queue.

#pragma once

#include "bank.h"
#include "nativeresampler.h"

#include <vector>

namespace Rations
{

class ModelBank;

//------------------------------------------------------------------------
class CrossfadeEngine : public NativeBlockProcessor
{
public:
    // Non-RT. Sizes the scratch buffers for the largest native-rate block that can arrive.
    void prepare(int maxNativeFrames, double nativeSampleRate);

    // Non-RT. The engine hands retired banks to this loader and asks it to build entries.
    void setLoader(ModelBank *loader)
    {
        mLoader = loader;
    }

    // RT. Collects any newly published bank and releases the old one. Call once per host block,
    // before processing. Returns true when a bank was actually taken — a republish replaces every
    // model pointer in this engine, which the rack has to know about because a channel switch in
    // flight toward it has been priming objects that are no longer bound.
    bool pollBank();

    // RT. Knob position as a normalized 0 .. 1 value; mapped onto [0, N-1] internally.
    void setPositionNorm(double norm);

    // RT. Output mode drives the per-branch level compensation, which must be applied to each
    // branch BEFORE the mix: adjacent captures of the same amp differ in measured loudness by up
    // to 1.75 dB and not monotonically, so compensating after the mix would step audibly at every
    // crossing. Mode 0 = Raw (no compensation, gain rises across the bank the way the amp's own
    // control does), 1 = Normalized, 2 = Calibrated.
    void setOutputMode(int mode, double calLevelDbu);

    // NativeBlockProcessor.
    void processNative(NAM_SAMPLE **in, NAM_SAMPLE **out, int numFrames) override;

    // RT, for the processor's input-calibration stage and its editor feedback.
    bool hasInputLevel() const;
    double inputLevelDbu() const;
    int entryCount() const
    {
        return mBank ? mBank->count : 0;
    }
    // Normalized index of the capture that is actually sounding — the nearer branch, not the knob.
    double activeIndexNorm() const;
    // True once at least one entry is playable. Until then the engine outputs ramped silence, not
    // dry signal: passing the input through and then dropping a model on top of it would jump in
    // level the instant the first entry landed.
    bool playable() const
    {
        return mReadyMix > 0.0 || mHaveReady;
    }
    // True once this engine has actually put a sample into the output. Distinct from playable():
    // the rack uses it to tell "nothing is being heard yet" from "something is", because adopting
    // a channel outright is only free while nothing is audible.
    bool sounding() const
    {
        return mReadyMix > 0.0;
    }
    // True when the ready gate is fully open, so this engine's output is its models' output and
    // not a ramp of it. The channel fade must not start before this: a fade into a signal that is
    // still being ramped in would be exact arithmetic on an inexact signal.
    bool fullyOpen() const
    {
        return mReadyMix >= 1.0;
    }

    // RT. How many models this engine will run on the next block: one at rest, two while its dial
    // is moving. The rack subtracts this from its total model budget before deciding how fast the
    // incoming channel may be caught up, so a stomp can never stack on top of a knob turn.
    int boundBranches() const
    {
        return (mA.bound() ? 1 : 0) + (mB.bound() ? 1 : 0);
    }

    // --- the channel switch ---------------------------------------------------------------
    // RT. The bank index this engine would sound if it were made active right now: the capture
    // its own dial names. Returns -1 when that capture is not built yet, which the rack answers
    // by HOLDING the switch rather than by sounding a neighbour. Substituting here would be the
    // one place a footswitch could put the wrong amp under the player's hands.
    int restIndex() const;
    // RT. How many samples of input that capture must be fed before its output is exact. Read
    // from the entry the worker read it off the model; 0 when there is no such capture yet.
    int restPrewarmSamples() const;
    // RT. Park the engine on that capture, which is where an idle channel always is. Costs one
    // bound branch rather than two, so exactly one model needs priming for a switch.
    void snapToRest();
    // RT. Tell this channel's worker which entry to build next even though the channel is silent,
    // so the capture a switch would land on is the one that gets built first. An idle engine is
    // not processed, and setPriority() otherwise only happens inside processNative().
    void hintPriority();

    // Non-RT teardown: hand the live bank back once the worker has stopped.
    Bank *releaseBank();

private:
    struct Branch {
        int index = -1;
        nam::DSP *model = nullptr;
        long long samplesLive = 0;
        int prewarmSamples = 0;
        double gain = 1.0;

        bool bound() const
        {
            return model != nullptr;
        }
    };

    void bindBranches(int k, bool wantUpper);
    void bindOne(Branch &branch, int index);
    double entryGain(const BankEntry &entry) const;
    void refreshGains();
    // How much of `branch`'s receptive field has been fed with live input, 0 .. 1. A branch may
    // not reach weight 1.0 before this reaches 1.0, or its contribution at full weight would not
    // yet be the model's true response.
    static double primedFraction(const Branch &branch);

    ModelBank *mLoader = nullptr;
    Bank *mBank = nullptr;
    // Held when the retirement queue was momentarily full. Retried on the next block; the audio
    // thread never deletes it itself.
    Bank *mHolding = nullptr;

    Branch mA, mB;

    double mPos = 0.0;      // current continuous position
    double mKnobPos = 0.0;  // where the knob says to go
    double mLastKnob = 0.0; // to detect that the knob has stopped moving
    long long mIdleSamples = 0;
    // Set when a bank is bound, so the first block lands the position on the knob rather than
    // travelling to it.
    bool mSnapPosition = true;
    bool mDetenting = false; // gliding onto a capture
    bool mDetented = false;  // settled on one, and staying until the knob moves
    double mDetentTarget = 0.0;
    double mDetentStep = 0.0;

    double mSlewStep = 0.0; // position units per sample while following the knob
    double mReadyMix = 0.0; // ramped 0 .. 1 gate over "a playable entry exists"
    double mReadyStep = 1.0;
    bool mHaveReady = false;

    int mOutputMode = 0;
    double mCalLevelDbu = 12.0;

    double mNativeSampleRate = kNativeSampleRate;
    long long mPrewarmForSlew = 0;

    std::vector<NAM_SAMPLE> mScratchA;
    std::vector<NAM_SAMPLE> mScratchB;
};

} // namespace Rations
