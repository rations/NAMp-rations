// JackClient implementation. See jackclient.h.

#include "jackclient.h"

#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstevents.h"

#include <jack/midiport.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace Steinberg;

namespace Rations
{

namespace
{

// Reserved queue counts, and the reason they are not left at the default. ParameterChanges grows
// its vector when addParameterData runs out of reserved queues
// (public.sdk/source/vst/hosting/parameterchanges.cpp), which on this thread is a malloc. One
// block can carry every knob the editor touched plus a burst of CC, so the input side is sized for
// far more than a person can produce and the output side for the five the plug-in publishes.
constexpr int32 kMaxInputParameters = 64;
constexpr int32 kMaxOutputParameters = 8;

// One block of MIDI. A footswitch sends one message; this is sized for a controller sweeping a
// bank of pedals, and anything past it is dropped rather than grown into.
constexpr int32 kMaxEvents = 256;

constexpr int kMidiStatusMask = 0xf0;
constexpr int kMidiChannelMask = 0x0f;
constexpr int kMidiNoteOff = 0x80;
constexpr int kMidiNoteOn = 0x90;
constexpr int kMidiControlChange = 0xb0;
constexpr int kMidiProgramChange = 0xc0;

} // namespace

//------------------------------------------------------------------------
// UI thread.
bool JackClient::readFeedback(int index, Vst::ParamID &id, double &value, uint32_t &seq) const
{
    if (index < 0 || index >= mFeedbackCount.load(std::memory_order_acquire))
        return false;
    const FeedbackSlot &slot = mFeedback[index];
    // seq first, then the value it belongs to: the audio thread writes them the other way round,
    // so a value read after its sequence number is at least as new as that number says.
    seq = slot.seq.load(std::memory_order_acquire);
    id = slot.id.load(std::memory_order_relaxed);
    value = slot.value.load(std::memory_order_relaxed);
    return true;
}

//------------------------------------------------------------------------
JackClient::~JackClient()
{
    close();
}

//------------------------------------------------------------------------
bool JackClient::open(const char *clientName, Vst::IAudioProcessor *processor,
                      Vst::IComponent *component, const MidiRoute *route)
{
    if (!processor || !component)
        return false;
    mProcessor = processor;
    mComponent = component;
    mRoute = route;

    jack_status_t status = static_cast<jack_status_t>(0);
    mClient = jack_client_open(clientName, JackNoStartServer, &status);
    if (!mClient) {
        fprintf(stderr, "rations-standalone: cannot connect to JACK (is jackd running?)\n");
        return false;
    }

    mSampleRate = static_cast<double>(jack_get_sample_rate(mClient));
    mBlockSize.store(static_cast<int>(jack_get_buffer_size(mClient)), std::memory_order_relaxed);

    mInPort = jack_port_register(mClient, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    mOutPorts[0] =
        jack_port_register(mClient, "out_l", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    mOutPorts[1] =
        jack_port_register(mClient, "out_r", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!mInPort || !mOutPorts[0] || !mOutPorts[1]) {
        fprintf(stderr, "rations-standalone: cannot register JACK ports\n");
        close();
        return false;
    }

    // The footswitch port. Not fatal if it cannot be registered: the amp still plays, and the bat
    // switches still change channel.
    mMidiPort = jack_port_register(mClient, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    if (!mMidiPort)
        fprintf(stderr, "rations-standalone: cannot register the MIDI port - no footswitch\n");

    // All RT-side allocation happens here, before the process callback can run: the bus and
    // channel-pointer arrays, the parameter queues and the event list.
    //
    // bufferSamples is deliberately 0. That is what tells HostProcessData it does NOT own the
    // sample buffers, because process() below points the buses straight at JACK's own memory each
    // block. Passing mBlockSize here instead makes it allocate a buffer per channel and set
    // channelBufferOwner - and then unprepare() runs delete[] on whatever the pointers hold at
    // close, which by then is JACK's memory, while the buffers it really allocated leak.
    if (!mProcessData.prepare(*component, 0, Vst::kSample32)) {
        fprintf(stderr, "rations-standalone: cannot prepare the process buffers\n");
        close();
        return false;
    }

    mInputChanges.setMaxParameters(kMaxInputParameters);
    mOutputChanges.setMaxParameters(kMaxOutputParameters);
    mEvents.setMaxSize(kMaxEvents);

    mProcessData.numSamples = mBlockSize.load(std::memory_order_relaxed);
    mProcessData.symbolicSampleSize = Vst::kSample32;
    mProcessData.inputParameterChanges = &mInputChanges;
    mProcessData.outputParameterChanges = &mOutputChanges;
    mProcessData.inputEvents = &mEvents;

    if (jack_set_process_callback(mClient, processTrampoline, this) != 0) {
        fprintf(stderr, "rations-standalone: cannot install the JACK process callback\n");
        close();
        return false;
    }
    // Must be registered before jack_activate() (jack.h says so explicitly).
    if (jack_set_buffer_size_callback(mClient, bufferSizeTrampoline, this) != 0)
        fprintf(stderr, "rations-standalone: cannot install the JACK buffer-size callback - the "
                        "processor will stay set up for the size it started with\n");

    if (jack_activate(mClient) != 0) {
        fprintf(stderr, "rations-standalone: cannot activate the JACK client\n");
        close();
        return false;
    }

    // Auto-connect the AUDIO to the system's physical ports, so the amp makes sound out of the
    // box. Failures here are not fatal: the user can wire it up in a patchbay instead.
    //
    // The MIDI port is deliberately left unconnected. Which of a machine's MIDI sources is the
    // footswitch is not something to guess at, and connecting the wrong one would have a
    // keyboard changing amp channels.
    if (const char **ins = jack_get_ports(mClient, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                          JackPortIsPhysical | JackPortIsOutput)) {
        if (ins[0])
            jack_connect(mClient, ins[0], jack_port_name(mInPort));
        jack_free(ins);
    }
    if (const char **outs = jack_get_ports(mClient, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsPhysical | JackPortIsInput)) {
        for (int i = 0; i < 2 && outs[i]; ++i)
            jack_connect(mClient, jack_port_name(mOutPorts[i]), outs[i]);
        jack_free(outs);
    }

    printf("rations-standalone: JACK connected at %.0f Hz, %d frames\n", mSampleRate,
           mBlockSize.load(std::memory_order_relaxed));
    return true;
}

//------------------------------------------------------------------------
void JackClient::close()
{
    if (mClient) {
        jack_deactivate(mClient);
        jack_client_close(mClient);
        mClient = nullptr;
    }
    mInPort = nullptr;
    mOutPorts[0] = mOutPorts[1] = nullptr;
    mMidiPort = nullptr;
    mProcessData.inputEvents = nullptr;
    mProcessData.unprepare();
    mProcessor = nullptr;
    mComponent = nullptr;
    mRoute = nullptr;
}

//------------------------------------------------------------------------
// UI thread (producer).
bool JackClient::pushParameter(Vst::ParamID id, Vst::ParamValue value)
{
    const uint32_t write = mRingWrite.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) % kRingSize;
    if (next == mRingRead.load(std::memory_order_acquire))
        return false; // full; drop rather than block either thread
    mRing[write] = {id, value};
    mRingWrite.store(next, std::memory_order_release);
    return true;
}

//------------------------------------------------------------------------
// RT thread (consumer). addParameterData/addPoint only reuse the queues and points reserved
// during open(), so this does not allocate.
void JackClient::drainParameterRing()
{
    uint32_t read = mRingRead.load(std::memory_order_relaxed);
    const uint32_t write = mRingWrite.load(std::memory_order_acquire);
    while (read != write) {
        const Change &change = mRing[read];
        int32 index = 0;
        if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(change.id, index)) {
            int32 pointIndex = 0;
            queue->addPoint(0, change.value, pointIndex);
        }
        read = (read + 1) % kRingSize;
    }
    mRingRead.store(read, std::memory_order_release);
}

//------------------------------------------------------------------------
// RT thread. Translates one block of JACK MIDI into the two shapes a VST3 plug-in actually
// receives, using the table midiroute.h resolved on the main thread. Nothing here asks the
// controller anything, which is the whole reason that table exists.
//
// EVERYTHING LANDS AT SAMPLE 0 of the block rather than at its true offset. The chunk loop below
// hands the queues and the event list to the first chunk only, so an offset past that chunk would
// be silently clamped by the plug-in anyway; and what this port carries is a footswitch, where a
// block of jitter is under three milliseconds and no one can play tighter than that.
void JackClient::readMidi(jack_nframes_t frames)
{
    if (!mMidiPort || !mRoute)
        return;
    void *buffer = jack_port_get_buffer(mMidiPort, frames);
    if (!buffer)
        return;

    const uint32_t count = jack_midi_get_event_count(buffer);
    for (uint32_t i = 0; i < count; ++i) {
        jack_midi_event_t event;
        if (jack_midi_event_get(&event, buffer, i) != 0 || event.size < 2)
            continue;

        const int status = event.buffer[0] & kMidiStatusMask;
        const int channel = event.buffer[0] & kMidiChannelMask;
        const int data1 = event.buffer[1] & 0x7f;
        const int data2 = event.size > 2 ? (event.buffer[2] & 0x7f) : 0;

        switch (status) {
            case kMidiControlChange: {
                const Vst::ParamID id = mRoute->ccParam(channel, data1);
                if (id == Vst::kNoParamId)
                    break;
                int32 index = 0;
                if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(id, index)) {
                    int32 point = 0;
                    queue->addPoint(0, data2 / 127.0, point);
                }
                break;
            }
            case kMidiProgramChange: {
                const Vst::ParamID id = mRoute->programParam(channel);
                if (id == Vst::kNoParamId)
                    break;
                int32 index = 0;
                if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(id, index)) {
                    int32 point = 0;
                    queue->addPoint(0, mRoute->programValue(channel, data1), point);
                }
                break;
            }
            case kMidiNoteOn:
            case kMidiNoteOff: {
                // A note on at velocity 0 is a note off, which is how most controllers send one.
                const bool on = (status == kMidiNoteOn) && data2 > 0;
                Vst::Event e = {};
                e.busIndex = 0;
                e.sampleOffset = 0;
                e.type = on ? static_cast<uint16>(Vst::Event::kNoteOnEvent)
                            : static_cast<uint16>(Vst::Event::kNoteOffEvent);
                if (on) {
                    e.noteOn.channel = static_cast<int16>(channel);
                    e.noteOn.pitch = static_cast<int16>(data1);
                    e.noteOn.velocity = static_cast<float>(data2) / 127.0f;
                    e.noteOn.noteId = -1;
                } else {
                    e.noteOff.channel = static_cast<int16>(channel);
                    e.noteOff.pitch = static_cast<int16>(data1);
                    e.noteOff.velocity = static_cast<float>(data2) / 127.0f;
                    e.noteOff.noteId = -1;
                }
                mEvents.addEvent(e); // full is a drop, not a growth
                break;
            }
            default:
                break;
        }
    }
}

//------------------------------------------------------------------------
// RT thread. Only stores into atomics - the controller is a UI-thread object and must never be
// called from here, which is the whole reason this is a table the run loop reads later rather than
// a call.
//
// The last point of each queue wins: these are levels and states, not gestures, so what the UI
// wants is where the parameter ENDED the block.
void JackClient::publishFeedback()
{
    const int32 count = mOutputChanges.getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::IParamValueQueue *queue = mOutputChanges.getParameterData(i);
        if (!queue)
            continue;
        const int32 points = queue->getPointCount();
        if (points < 1)
            continue;
        int32 offset = 0;
        Vst::ParamValue value = 0.0;
        if (queue->getPoint(points - 1, offset, value) != kResultTrue)
            continue;
        const Vst::ParamID id = queue->getParameterId();

        int used = mFeedbackCount.load(std::memory_order_relaxed);
        int slot = -1;
        for (int s = 0; s < used; ++s) {
            if (mFeedback[s].id.load(std::memory_order_relaxed) == id) {
                slot = s;
                break;
            }
        }
        if (slot < 0) {
            if (used >= kFeedbackSlots)
                continue; // no room, and growing here would be an allocation on this thread
            mFeedback[used].id.store(id, std::memory_order_relaxed);
            slot = used;
            // Released AFTER the id is in place, so a reader that sees this count sees the id too.
            mFeedbackCount.store(used + 1, std::memory_order_release);
        }
        mFeedback[slot].value.store(value, std::memory_order_relaxed);
        // Released LAST, so a reader that sees this sequence number sees the value with it.
        mFeedback[slot].seq.fetch_add(1, std::memory_order_release);
    }
}

//------------------------------------------------------------------------
int JackClient::processTrampoline(jack_nframes_t nframes, void *arg)
{
    return static_cast<JackClient *>(arg)->process(nframes);
}

//------------------------------------------------------------------------
// JACK's notification thread, with the process cycle suspended. It would be legal to do the whole
// reconfiguration here, but setupProcessing and setActive are VST3 main-thread calls and the
// plug-in's message thread may be in the middle of a load, so all this does is record the new size
// for the run loop to act on.
int JackClient::bufferSizeTrampoline(jack_nframes_t nframes, void *arg)
{
    static_cast<JackClient *>(arg)->mNewBlockSize.store(static_cast<int>(nframes),
                                                        std::memory_order_release);
    return 0;
}

//------------------------------------------------------------------------
int JackClient::takeBufferSizeChange()
{
    const int size = mNewBlockSize.exchange(0, std::memory_order_acquire);
    // JACK announces the size once at activation too; only a real move counts.
    if (size <= 0 || size == mBlockSize.load(std::memory_order_relaxed))
        return 0;
    return size;
}

//------------------------------------------------------------------------
bool JackClient::suspendProcessing()
{
    if (!mClient)
        return true; // no audio thread to race with

    mSuspended.store(true, std::memory_order_release);

    // Two cycles, not one: the first may already have been inside process() when the flag went up,
    // so only the second is guaranteed to have seen it.
    const uint32_t start = mCycle.load(std::memory_order_acquire);
    for (int attempt = 0; attempt < 200; ++attempt) { // ~2 s
        if (mCycle.load(std::memory_order_acquire) - start >= 2)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // The audio thread is not running (a stopped server, or freewheeling). Say so and leave the
    // processor alone rather than reconfiguring it underneath a callback that might still come
    // back.
    mSuspended.store(false, std::memory_order_release);
    return false;
}

//------------------------------------------------------------------------
void JackClient::resumeProcessing(int blockSize)
{
    if (blockSize > 0)
        mBlockSize.store(blockSize, std::memory_order_relaxed);
    mSuspended.store(false, std::memory_order_release);
}

//------------------------------------------------------------------------
// JACK real-time thread. Nothing here allocates, locks, or logs.
int JackClient::process(jack_nframes_t nframes)
{
    float *outL = static_cast<float *>(jack_port_get_buffer(mOutPorts[0], nframes));
    float *outR = static_cast<float *>(jack_port_get_buffer(mOutPorts[1], nframes));
    if (!outL || !outR)
        return 0;

    // Suspended: the UI thread is reconfiguring the processor and must not be raced. Silence is
    // the honest output - a buffer-size change already puts a gap in the audio flow, and stale
    // samples would be worse.
    if (!mProcessor || mSuspended.load(std::memory_order_acquire)) {
        memset(outL, 0, nframes * sizeof(float));
        memset(outR, 0, nframes * sizeof(float));
        mCycle.fetch_add(1, std::memory_order_release);
        return 0;
    }

    float *in = static_cast<float *>(jack_port_get_buffer(mInPort, nframes));
    if (!in) {
        mCycle.fetch_add(1, std::memory_order_release);
        return 0;
    }

    mInputChanges.clearQueue();
    mOutputChanges.clearQueue();
    mEvents.clear();
    drainParameterRing();
    readMidi(nframes);
    mProcessData.inputEvents = &mEvents;

    // Loop, never clamp. JACK's buffer size can change under a running client, and the processor
    // was set up for mBlockSize: handing it more would break that contract, and truncating to
    // mBlockSize would leave the rest of the block holding whatever JACK's buffer had in it from
    // the previous cycle, which is stale audio rather than a dropout.
    const jack_nframes_t chunk =
        static_cast<jack_nframes_t>(mBlockSize.load(std::memory_order_relaxed));
    jack_nframes_t done = 0;
    while (done < nframes) {
        const int32 n = static_cast<int32>(std::min<jack_nframes_t>(chunk, nframes - done));

        // Point the VST3 bus buffers straight at JACK's, so no copy is needed.
        if (mProcessData.inputs && mProcessData.inputs[0].numChannels > 0)
            mProcessData.inputs[0].channelBuffers32[0] = in + done;
        if (mProcessData.outputs && mProcessData.outputs[0].numChannels > 1) {
            mProcessData.outputs[0].channelBuffers32[0] = outL + done;
            mProcessData.outputs[0].channelBuffers32[1] = outR + done;
        }
        mProcessData.numSamples = n;

        mProcessor->process(mProcessData);

        publishFeedback();
        // The queued edits and the MIDI belong to the top of the JACK block, not to every chunk of
        // it. Replaying the parameter points would re-apply the same edit n times; replaying the
        // events would stomp the footswitch once per chunk, which on a toggling pedal row is the
        // difference between on and off.
        mInputChanges.clearQueue();
        mOutputChanges.clearQueue();
        mEvents.clear();
        mProcessData.inputEvents = nullptr;
        done += static_cast<jack_nframes_t>(n);
    }

    // Every exit from this callback bumps the cycle counter, including the early ones:
    // suspendProcessing() waits on it, and a path that skipped it would look like an audio thread
    // that had stopped responding.
    mCycle.fetch_add(1, std::memory_order_release);
    return 0;
}

} // namespace Rations
