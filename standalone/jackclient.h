// JackClient — the JACK implementation of AudioBackend: mono in, stereo out, plus a MIDI in port.
//
// What this class is FOR, and the three things that cross the RT boundary, are described once in
// audiobackend.h and not repeated here. What follows is only what is specific to JACK.
//
// The process callback runs on JACK's real-time thread and obeys the same contract the plug-in's
// own process() does: no allocation, no locks, no logging, no file I/O. Every VST3 process
// structure is allocated once in open(); the sample buffers are JACK's own, which the bus pointers
// are aimed at each block rather than copied through.
//
// JACK is unusual among audio APIs in two ways that shape this file. It calls a callback on a
// thread it owns rather than handing out a buffer to fill, so there is no audio thread here to
// create or join — jack_activate() starts it and jack_deactivate() stops it. And it can change its
// buffer size under a running client at any other client's request, which is why the base
// interface has a buffer-size handshake at all: on a system where the size is fixed at open time
// that machinery simply never fires.

#pragma once

#include "audiobackend.h"

#include "host/chainengine.h"
#include "midiroute.h"

#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <jack/jack.h>

#include <atomic>
#include <cstdint>

namespace Rations
{

//------------------------------------------------------------------------
class JackClient final : public AudioBackend
{
public:
    ~JackClient() override;

    // What the device is already running at, learned WITHOUT opening it, so that setupProcessing
    // can be told the truth before the component is activated. False means there is no device to be
    // had — no JACK server here — and the standalone continues with the editor only.
    //
    // This lives beside the backend rather than in the standalone because it is JACK knowledge: it
    // opens a throwaway client purely to read two numbers off the server. Its Windows counterpart
    // answers the same question from an entirely different place, which is exactly why the caller
    // should not be the one asking. See nativeaudio.h.
    bool probeDefaults(double &sampleRate, int &blockSize);

    // Connects to a running JACK server and starts processing. `processor` must already be set up
    // and activated, and `route` must already have been resolved: both are read from the audio
    // thread the moment the client is activated.
    bool open(const char *clientName, Steinberg::Vst::IAudioProcessor *processor,
              Steinberg::Vst::IComponent *component, const MidiRoute *route) override;
    void close() override;

    // --- the rack --------------------------------------------------------
    void setChainEngine(NAMp::host::ChainEngine *engine) override
    {
        mChain = engine;
    }
    void notifyLatencyChanged() override;

    bool isOpen() const override
    {
        return mClient != nullptr;
    }
    double sampleRate() const override
    {
        return mSampleRate;
    }
    int blockSize() const override
    {
        return mBlockSize.load(std::memory_order_relaxed);
    }

    int takeBufferSizeChange() override;
    bool suspendProcessing() override;
    void resumeProcessing(int blockSize) override;

    // JACK has no reopen event. A server that goes away takes its clients with it — the process is
    // told through jack_on_shutdown, not asked to open the device again — so there is nothing for
    // this to report and a constant false is the honest answer rather than a stub.
    bool takeDeviceReset() override
    {
        return false;
    }

    bool pushParameter(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) override;

    bool readFeedback(int index, Steinberg::Vst::ParamID &id, double &value,
                      uint32_t &seq) const override;

    uint32_t dropouts() const override
    {
        return mXruns.load(std::memory_order_relaxed);
    }

    std::string deviceSummary() const override;

private:
    static int processTrampoline(jack_nframes_t nframes, void *arg);
    static int bufferSizeTrampoline(jack_nframes_t nframes, void *arg);
    static int xrunTrampoline(void *arg);
    static void latencyTrampoline(jack_latency_callback_mode_t mode, void *arg);
    int process(jack_nframes_t nframes);
    void reportLatency(jack_latency_callback_mode_t mode);
    void drainParameterRing();            // RT thread
    void readMidi(jack_nframes_t frames); // RT thread
    void publishFeedback();               // RT thread

    // SPSC ring: written only by the UI thread, read only by the audio thread.
    static constexpr uint32_t kRingSize = 512; // power of two
    struct Change {
        Steinberg::Vst::ParamID id;
        Steinberg::Vst::ParamValue value;
    };
    Change mRing[kRingSize] = {};
    std::atomic<uint32_t> mRingWrite{0};
    std::atomic<uint32_t> mRingRead{0};

    // One slot per distinct parameter id the plug-in has published. mFeedbackCount only ever
    // grows, and is published with release/acquire so a reader that sees the count also sees the
    // id that was written into the slot before it.
    struct FeedbackSlot {
        std::atomic<Steinberg::Vst::ParamID> id{0};
        std::atomic<double> value{0.0};
        std::atomic<uint32_t> seq{0};
    };
    FeedbackSlot mFeedback[kFeedbackSlots];
    std::atomic<int> mFeedbackCount{0};

    // Buffer-size handshake. mCycle is bumped by the audio thread on every callback, suspended or
    // not, which is how the UI thread knows a call it might have raced with has finished.
    std::atomic<int> mNewBlockSize{0};
    std::atomic<bool> mSuspended{false};
    std::atomic<uint32_t> mCycle{0};

    // Bumped from JACK's own notification thread, read from anywhere. See dropouts().
    std::atomic<uint32_t> mXruns{0};

    // The rack. Null until setChainEngine(), and null is the transparent path: one branch per
    // chunk, no copies, and exactly the audio path this standalone had before the rack existed.
    NAMp::host::ChainEngine *mChain = nullptr;

    // The amp's own reported latency, read once at open() while the processor is quiescent. The
    // chain's is added to it in reportLatency() and comes from the engine, so it is the latency of
    // the chain actually running rather than of one that has been queued.
    uint32_t mPluginLatency = 0;

    jack_client_t *mClient = nullptr;
    jack_port_t *mInPort = nullptr;
    jack_port_t *mOutPorts[2] = {nullptr, nullptr};
    jack_port_t *mMidiPort = nullptr;

    Steinberg::Vst::IAudioProcessor *mProcessor = nullptr;
    Steinberg::Vst::IComponent *mComponent = nullptr;
    const MidiRoute *mRoute = nullptr;

    // This cycle's MIDI kept UNDECODED for the rack, alongside the decode readMidi() does for the
    // amp. The two cannot share a form: the amp's route is resolved against the amp's controller
    // and answers only for the amp, while every hosted plug-in has a route of its own. So the amp
    // gets its parameter writes as it always did and the rack gets the messages, and each opens the
    // door that is actually its own.
    //
    // Fixed capacity, filled on the audio thread, read within the same callback. A cycle carrying
    // more than kMaxChunkMidi drops the rest rather than growing.
    NAMp::host::RtMidiEvent mRackMidi[NAMp::host::kMaxChunkMidi];
    int32_t mRackMidiCount = 0;

    double mSampleRate = 48000.0;
    // Read by the audio thread, written by the UI thread on a size change.
    std::atomic<int> mBlockSize{1024};

    // Pre-allocated VST3 process plumbing, owned by the audio thread once open() returns.
    Steinberg::Vst::HostProcessData mProcessData;
    Steinberg::Vst::ParameterChanges mInputChanges;
    Steinberg::Vst::ParameterChanges mOutputChanges;
    Steinberg::Vst::EventList mEvents;
};

} // namespace Rations
