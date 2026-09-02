// JackClient — mono-in / stereo-out audio, plus a MIDI in port, for the Rations standalone.
//
// The process callback runs on JACK's real-time thread and obeys the same contract the plug-in's
// own process() does: no allocation, no locks, no logging, no file I/O. Every VST3 process
// structure is allocated once in open(); the sample buffers are JACK's own, which the bus pointers
// are aimed at each block rather than copied through.
//
// Three things have to cross the RT boundary, and none of them may touch a lock or the edit
// controller from the audio thread:
//
//   UI -> RT   parameter edits made in the editor. They arrive on the UI thread through the host's
//              IComponentHandler and are pushed into a single-producer/single-consumer ring, which
//              the audio thread drains into the VST3 input parameter queues at the top of each
//              block. Sharing a ParameterChanges between the two threads instead would be a plain
//              data race.
//
//   MIDI -> RT the footswitch. JACK hands its messages to this same callback, and where each one
//              has to go was worked out on the main thread (see midiroute.h) precisely so that
//              nothing here has to ask the controller anything.
//
//   RT -> UI   the meters, the bank build progress and which channel is actually SOUNDING, all of
//              which the plug-in publishes through outputParameterChanges. The audio thread only
//              stores them in atomics; the UI thread reads those and calls the controller, because
//              IEditController must never be called from RT.

#pragma once

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
class JackClient
{
public:
    ~JackClient();

    // Connects to a running JACK server and starts processing. `processor` must already be set up
    // and activated, and `route` must already have been resolved: both are read from the audio
    // thread the moment the client is activated.
    bool open(const char *clientName, Steinberg::Vst::IAudioProcessor *processor,
              Steinberg::Vst::IComponent *component, const MidiRoute *route);
    void close();

    bool isOpen() const
    {
        return mClient != nullptr;
    }
    double sampleRate() const
    {
        return mSampleRate;
    }
    int blockSize() const
    {
        return mBlockSize.load(std::memory_order_relaxed);
    }

    // --- runtime buffer-size changes -------------------------------------
    // JACK can resize its buffers under a running client. The chunk loop in process() keeps that
    // safe on its own, but the processor is then still set up for the old size, so it is worth
    // telling it. The reconfiguration is VST3 main-thread work, and it is not done in JACK's
    // callback: these three calls hand it to the run loop instead.

    // UI thread: the size JACK has moved to, or 0 if it has not moved.
    int takeBufferSizeChange();

    // UI thread: stop the audio callback from entering the processor, and wait until any call
    // already in flight has returned. It outputs silence until resumed. False means the audio
    // thread did not respond in time, in which case the caller must NOT touch the processor.
    bool suspendProcessing();

    // UI thread: adopt the new block size and let the audio callback back in.
    void resumeProcessing(int blockSize);

    // UI thread: queue a normalized parameter change for the next block. Returns false if the ring
    // is full (the change is then dropped, which is preferable to blocking the UI or the audio
    // thread).
    bool pushParameter(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);

    // --- what the plug-in publishes back ---------------------------------
    // EVERY parameter the processor writes into outputParameterChanges, whatever it is. Forwarding
    // a hand-picked few was the first version of this and it was wrong, in a way that only showed
    // up with a footswitch in hand: the queue carries two different kinds of traffic and only one
    // of them was being read.
    //
    //   * The hidden read-only ones the plug-in publishes every block - two meters, the bank build
    //     progress, which capture is sounding, which CHANNEL is sounding.
    //   * An ECHO of any parameter the plug-in changed BY ITSELF, which is what the MIDI learn
    //     table does on the audio thread. src/rationsprocessor.cpp says why it reports them: a
    //     parameter the plug-in moved and did not report leaves the editor's copy disagreeing with
    //     the audio. A host closes that loop by calling IEditController::setParamNormalized, and
    //     the standalone is the host here.
    //
    // Dropping the second kind is a footswitch that changes the sound while the panel says
    // otherwise - the bat switch stays where it was, the pedal's lamp stays dark.
    //
    // Values are COALESCED per parameter id rather than queued, so the meters - which arrive every
    // block - cannot crowd out an echo that arrives once. Slots are claimed on the audio thread
    // and never released, which is what keeps this allocation-free; a plug-in that published more
    // distinct parameters than there are slots would lose the excess rather than grow.
    static constexpr int kFeedbackSlots = 64;

    // UI thread: read slot `index`. False means the slot has never been used. `seq` changes every
    // time the audio thread writes, so a caller can tell a repeat from a new value.
    bool readFeedback(int index, Steinberg::Vst::ParamID &id, double &value, uint32_t &seq) const;

private:
    static int processTrampoline(jack_nframes_t nframes, void *arg);
    static int bufferSizeTrampoline(jack_nframes_t nframes, void *arg);
    int process(jack_nframes_t nframes);
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

    jack_client_t *mClient = nullptr;
    jack_port_t *mInPort = nullptr;
    jack_port_t *mOutPorts[2] = {nullptr, nullptr};
    jack_port_t *mMidiPort = nullptr;

    Steinberg::Vst::IAudioProcessor *mProcessor = nullptr;
    Steinberg::Vst::IComponent *mComponent = nullptr;
    const MidiRoute *mRoute = nullptr;

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
