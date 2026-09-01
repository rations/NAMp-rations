// ChannelRack — four channels, one of which sounds, and the click-free switch between them.
//
// This is the only genuinely new piece of DSP in the plug-in; everything else is the single-bank
// engine run four times. It exists because a footswitch stomp has to do something a knob turn
// never does: bring a model that has been idle for minutes into the signal path immediately.
//
// Why that is hard, and why it is nonetheless exactly solvable:
//
//   * The captures run on the A2 fast-path WaveNet, which is strictly feed-forward. Output at
//     sample t is a pure function of the input window [t - R, t], where R is the model's own
//     GetPrewarmSamples(). There is no recurrent state, so there is nothing about a model's past
//     that cannot be reconstructed from the input's past.
//   * An idle channel therefore has no valid state, not merely stale state. Binding it and
//     letting it run produces a whole receptive field of wrong output — the click this plug-in
//     exists to prevent, arriving through the one control a player uses mid-song.
//   * But the whole of that missing state IS the last R input samples, and those we have. So the
//     rack keeps a ring of them and, on a switch, replays that history into the incoming channel
//     faster than real time with the output thrown away. When the ring is drained the incoming
//     model is bit-identical to one that had been running since the session began, and only then
//     does a short fade run — between two exact signals, so there is nothing to mask.
//
// How fast "faster than real time" is, is not a constant of the design: it is whatever is left of
// engine::kSwitchModelBudget once the sounding channel's own branches are paid for. Stating it as
// a total rather than as a rate is what stops a stomp stacking on top of a gain knob that is
// already costing two models, and when nothing is left the switch is HELD — the outgoing channel
// keeps sounding and the switch arrives late — rather than the deadline being missed. See
// engineconfig.h for the measurement that set the budget, and for what it costs in switch latency.
//
// What none of it costs is steady state: that is ONE model, the sounding channel at its detent.
// The other three banks are resident in memory and are not processed at all, which is what makes
// "instant switching" and "one channel's worth of CPU" compatible rather than opposed.
//
// The proof is not the code reading correctly. It is rations_switchcheck, which renders a
// continuously-running reference and asserts that the switched output converges on it, and shows
// the hard-switch control clicking where this does not.
//
// Real-time contract, unchanged and extended to the catch-up burst: no allocation, no locks, no
// file I/O, no logging, no destructors. Every model call goes through a fixed engine::kChunk
// slice, including the ones fed from the ring.

#pragma once

#include "crossfadeengine.h"
#include "modelbank.h"
#include "nativeresampler.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class ChannelRack : public NativeBlockProcessor
{
public:
    // No channel. Used for the catch-up target rather than a bool-plus-index pair, so "there is a
    // switch in flight" and "this is where it is going" cannot disagree.
    static constexpr int kNoChannel = -1;

    ChannelRack();
    ~ChannelRack() override;

    ChannelRack(const ChannelRack &) = delete;
    ChannelRack &operator=(const ChannelRack &) = delete;

    // --- message thread ------------------------------------------------------------------
    // Sizes every buffer for the largest native-rate block that can arrive, and the input ring.
    void prepare(int maxNativeFrames, double nativeSampleRate);
    void start();
    // Joins all four workers. Must happen before anything they could still be building goes away.
    void stop();
    // Load one channel from a folder of captures or from a single one. An empty path clears the
    // channel; a bank of one is not a special case anywhere below this line, so isDirectory only
    // decides which of ModelBank's two loaders runs.
    void loadChannel(Channel ch, const std::string &path, bool isDirectory, double slim,
                     int maxBufferSize);
    // Rebuild every loaded channel at a new Slim size. MESSAGE THREAD ONLY: ModelBank::post takes
    // a mutex, so this must never be reached from process(). What it costs is in the .cpp.
    void setSlim(double slim, int maxBufferSize);
    std::vector<std::string> captureNames(Channel ch) const;
    // What that channel's captures state about their own levels, for the editor. Message thread.
    CaptureLevels captureLevels(Channel ch) const;
    // Hands every live bank back for deletion. Only valid once stop() has been called.
    void releaseBanks();

    // --- audio thread --------------------------------------------------------------------
    // Collects newly published banks and retires the old ones. Once per host block, before
    // processing.
    void pollBanks();
    // Where each channel's own dial is, as 0 .. 1 over that channel's bank. Pushed for all four
    // every block: an idle channel's dial decides which capture a switch would land on, and which
    // capture its worker should build next.
    void setPositionNorm(Channel ch, double norm);
    // The channel kChannelId asks for. Acted on immediately, never queued, and HELD rather than
    // faked while the capture it names is still being built.
    void requestChannel(Channel ch);
    // Per-channel output trim, as a linear gain. Applied to that channel's OUTPUT, inside this
    // class, for two reasons that are both about the switch:
    //
    //   * it has to be per-channel at the point the fade mixes, or a channel change would step
    //     the level at the instant the sounding channel changes - which is precisely the
    //     discontinuity the catch-up and the fade exist to remove. A single trim applied after
    //     the resampler cannot do that: there is only one signal left by then.
    //   * it must not be on the model's INPUT. Models are fed from the ring, the ring is what the
    //     priming replays, and the proof that a switched channel is exact is a proof about that
    //     input. A trim on the output is outside all of it.
    //
    // Pushed for all four every block, like the dial positions.
    void setLevel(Channel ch, double linearGain);

    // --- either thread -------------------------------------------------------------------
    // The output section, for all four channels at once. PUBLISHED, not applied, for the same
    // reason the dial positions are: three of the four engines belong to the prime worker at any
    // instant, and each one picks this up on its own thread.
    //
    // `calibrateInput` is passed down rather than resolved here, and that is deliberate. Working
    // out the offset would mean reading each bank's input_level_dbu from the calling thread, which
    // is precisely the shared access this indirection exists to avoid — and it would also make the
    // offset per CHANNEL, when what is correct is per BRANCH: a dial sweeping across two captures
    // that state different input levels has to crossfade between their two offsets, exactly as it
    // already crossfades between their two output compensations, or it steps at the crossing.
    void setOutputMode(int mode, double calLevelDbu, bool calibrateInput);

    void processNative(NAM_SAMPLE **in, NAM_SAMPLE **out, int numFrames) override;

    // The channel that is actually sounding — which is not necessarily the one the parameter
    // asks for, because a switch is held until the audio can follow it. The editor's LEDs read
    // this rather than the parameter, so a lamp never lights over a channel that is not there yet.
    Channel soundingChannel() const
    {
        return static_cast<Channel>(mFading ? mTo : mFrom);
    }
    // Normalized index of the capture sounding in that channel.
    double activeIndexNorm() const;
    bool playable() const;
    // Fraction of all four banks that is built and primed. One number rather than four, because
    // what the editor shows is one progress line.
    float progress() const;

    // --- the prime worker ----------------------------------------------------------------
    // Worst gap, in native samples, that the worker has ever had to close in one tick. The ring
    // must be larger than this plus one receptive field or the worker gets lapped and the channel
    // it was keeping warm goes cold. Read from the message thread for reporting; it is the
    // measurement that decides whether kInputRingSamples has to grow.
    long long worstPrimeLag() const
    {
        return mWorstPrimeLag.load(std::memory_order_relaxed);
    }
    // Whether channel c is exact right now and could be switched to without priming. Phase A
    // publishes this and deliberately does not act on it: the audio thread still primes from a
    // whole receptive field, so what ships in this phase sounds exactly as it did before and the
    // only thing under test is whether the concurrency is correct.
    bool warm(Channel ch) const;

private:
    // Which thread owns a channel's engine. Exactly one at any instant, with no object either may
    // "just read": the handover is the whole safety property, so it is one atomic per channel and
    // three states rather than a bool.
    //
    // Worker -> ClaimedByRT is written by the audio thread. ClaimedByRT -> RT is written by the
    // worker, and only ever BETWEEN channels, so at the moment RT observes RT the worker is
    // provably not inside that engine. RT -> Worker is written by the audio thread when it has
    // finished with a channel. RT never waits for any of these: a claim that has not been
    // acknowledged yet simply holds the switch for another block, which is what the rack already
    // does for a capture that is not built.
    enum Owner : int {
        kOwnerWorker = 0,
        kOwnerClaimedByRT = 1,
        kOwnerRT = 2,
    };

    bool rtOwns(int c) const
    {
        return mOwner[c].load(std::memory_order_acquire) == kOwnerRT;
    }
    // Ask for a channel. Returns true once RT owns it; false means "not yet, hold the switch".
    bool claimForRT(int c);
    // Hand a channel back once RT has no further use for it. Its engine is exact at this instant,
    // so the worker is told where it got to rather than having to prime from scratch.
    void releaseToWorker(int c);
    void primeLoop();
    void primeChannel(int c, long long head);

    void writeRing(const NAM_SAMPLE *src, int numFrames);
    // Acts on mRequested. Never queues: a stomp inside the switch window is resolved in place.
    void resolveRequest(int numFrames);
    void beginCatchup(int ch, int numFrames);
    void runCatchup(int numFrames);
    void startFadeIfReady();
    void applyLevel(int channel, NAM_SAMPLE *buf, int numFrames);
    void mixFade(NAM_SAMPLE *dst, int numFrames);
    // Exchange the two ends of a running fade and complement its position, so the output is
    // continuous across the instant the direction changes.
    void reverseFade();
    // Feed one engine `count` samples of input history starting at absolute position `from`, in
    // fixed chunks, with the output thrown away. This IS the priming, and both threads do it
    // through this one function; the scratch buffers are passed in because the caller's thread
    // owns them and two threads must never share a staging buffer.
    // Bring one channel's engine up to date with the published output section, if it is behind.
    // Returns true when it actually applied something, because the caller has to know: an input
    // calibration change moves the level of the input its models are fed, which invalidates any
    // priming done before it exactly as a bank republish does.
    //
    // Called only by the thread that owns that channel.
    bool applyOutputMode(int channel);
    void feedFromRing(CrossfadeEngine &target, long long from, long long count, NAM_SAMPLE *feed,
                      NAM_SAMPLE *sink);

    ModelBank mLoader[kChannelCount];
    CrossfadeEngine mEngine[kChannelCount];

    // The fade pair. mFrom carries weight (1 - mFadeMix) and mTo carries mFadeMix; with no fade
    // running they are the same channel and mFrom is simply what is sounding.
    int mFrom = kChannelClean;
    int mTo = kChannelClean;
    bool mFading = false;

    // Per-channel output trim, and its ramp. Ramped rather than applied as a per-block scalar,
    // because a level that steps once per block zips audibly while the slider is being dragged.
    // The ramp is per channel, but at most two channels are producing output at once, so at most
    // two of these advance in a block.
    double mLevelTarget[kChannelCount] = {1.0, 1.0, 1.0, 1.0};
    double mLevelCurrent[kChannelCount] = {1.0, 1.0, 1.0, 1.0};
    // Per-sample multipliers, one each way, so neither direction costs a divide on the audio path.
    double mLevelStepUp = 1.0;
    double mLevelStepDown = 1.0;
    double mFadeMix = 0.0;
    double mFadeStep = 1.0;

    // The catch-up in flight, if any, and how far its read cursor has got. mPrimed says the
    // cursor has reached the write head, so the target is exact and is only waiting for the fade
    // slot to come free.
    int mTarget = kNoChannel;
    long long mCursor = 0;
    bool mPrimed = false;
    // The incoming capture's receptive field, kept from when the catch-up began: a catch-up with
    // no CPU headroom parks the cursor exactly this far behind the write head every block rather
    // than letting the gap grow past the ring.
    long long mCatchupLag = 0;

    int mRequested = kChannelClean;

    // The input history. mWritePos counts every native-rate sample ever written, so a cursor is
    // an absolute position and the wrap is only ever an indexing detail. It stays a plain long
    // long because only the audio thread advances it; mWriteHead is the same number PUBLISHED,
    // stored release-ordered after the ring writes so a worker that acquires it is guaranteed to
    // see the samples it counts.
    std::vector<NAM_SAMPLE> mRing;
    long long mWritePos = 0;
    std::atomic<long long> mWriteHead{0};

    // --- cross-thread state --------------------------------------------------------------
    std::atomic<int> mOwner[kChannelCount];
    // How far the owner has fed each engine, as an absolute ring position. Published by whichever
    // thread owns the channel and read by the other at the handover, so RT can compute the
    // remaining gap exactly instead of assuming a whole receptive field.
    std::atomic<long long> mPrimedThrough[kChannelCount];
    // Where the CURRENT continuous priming run began. Warmth is not "the worker fed this engine
    // recently"; it is "this engine has consumed an unbroken receptive field ending at the write
    // head", and the only way to know that is to remember where the run started. Anything that
    // invalidates the history - a rebind, a republish, being lapped by the ring, a handover from
    // the audio thread - restarts the run here.
    std::atomic<long long> mPrimedFrom[kChannelCount];
    // True when mPrimedThrough[c] has reached the write head and the entry it was primed against
    // is still the one a switch would bind. Goes false on a bank republish, a dial move that
    // rebinds an idle channel, and a worker that has been lapped by the ring.
    std::atomic<bool> mWarm[kChannelCount];
    // Every dial, published rather than applied. The audio thread must not touch an engine it
    // does not own, and it is handed all four positions every block, so it writes them here and
    // whichever thread owns each engine applies it.
    std::atomic<double> mPosNorm[kChannelCount];

    // The published output section, plus a generation counter so an owning thread can tell whether
    // what it applied is still current. A counter rather than a per-channel flag because the three
    // values are one decision and are always published together: one bump means "re-read all of
    // it", which cannot describe a half-applied change the way three flags could.
    std::atomic<int> mOutputMode{1}; // Normalized, matching the parameter's default
    std::atomic<double> mCalLevelDbu{12.0};
    std::atomic<bool> mCalibrateInput{false};
    std::atomic<unsigned> mOutputGen{0};
    // What each channel has applied. Written only by that channel's owning thread; atomic because
    // ownership moves between threads and a plain int would be a race the ownership handshake does
    // not by itself document away.
    std::atomic<unsigned> mAppliedOutputGen[kChannelCount] = {};
    // What the worker knows about the capture each idle channel would land on, so the audio
    // thread can decide whether a switch is even possible before it owns the engine to ask.
    std::atomic<int> mRestPrewarmPub[kChannelCount];
    std::atomic<int> mRestIndexPub[kChannelCount];

    std::thread mPrimeThread;
    std::atomic<bool> mPrimeRunning{false};
    std::atomic<long long> mWorstPrimeLag{0};
    // The worker's own staging. Sized in prepare() and never resized afterwards, because the
    // worker allocates nothing once it is running.
    std::vector<NAM_SAMPLE> mPrimeFeed;
    std::vector<NAM_SAMPLE> mPrimeSink;

    // Pre-allocated staging. mFeed gathers a contiguous chunk out of the (wrapping) ring, mSink
    // takes the output nobody listens to, and mMix takes the incoming channel's live output while
    // a fade is running.
    std::vector<NAM_SAMPLE> mFeed;
    std::vector<NAM_SAMPLE> mSink;
    std::vector<NAM_SAMPLE> mMix;
    NAM_SAMPLE *mMixPtr = nullptr;

    // Which channels were asked to load, so progress() has an honest denominator. Written on the
    // message thread before the audio thread exists, read-only afterwards.
    bool mLoadRequested[kChannelCount] = {false, false, false, false};

    // Set by pollBanks() for any channel whose bank pointer was replaced this block. A republish
    // invalidates every model pointer in that channel, so a switch in flight toward it has been
    // priming an object that is no longer bound.
    bool mBankChanged[kChannelCount] = {false, false, false, false};

    double mNativeSampleRate = kNativeSampleRate;
};

} // namespace Rations
