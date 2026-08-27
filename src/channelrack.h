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

#include <string>
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
    void loadChannel(Channel ch, const std::string &dir, double slim, int maxBufferSize);
    void setOutputMode(int mode, double calLevelDbu);
    std::vector<std::string> captureNames(Channel ch) const;
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

private:
    void writeRing(const NAM_SAMPLE *src, int numFrames);
    // Acts on mRequested. Never queues: a stomp inside the switch window is resolved in place.
    void resolveRequest(int numFrames);
    void beginCatchup(int ch, int numFrames);
    void runCatchup(int numFrames);
    void startFadeIfReady();
    void mixFade(NAM_SAMPLE *dst, int numFrames);
    // Exchange the two ends of a running fade and complement its position, so the output is
    // continuous across the instant the direction changes.
    void reverseFade();
    // Feed one engine `count` samples of input history starting at absolute position `from`, in
    // fixed chunks, with the output thrown away. This IS the priming.
    void feedFromRing(CrossfadeEngine &target, long long from, long long count);

    ModelBank mLoader[kChannelCount];
    CrossfadeEngine mEngine[kChannelCount];

    // The fade pair. mFrom carries weight (1 - mFadeMix) and mTo carries mFadeMix; with no fade
    // running they are the same channel and mFrom is simply what is sounding.
    int mFrom = kChannelClean;
    int mTo = kChannelClean;
    bool mFading = false;
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
    // an absolute position and the wrap is only ever an indexing detail.
    std::vector<NAM_SAMPLE> mRing;
    long long mWritePos = 0;

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
