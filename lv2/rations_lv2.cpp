// SPDX-License-Identifier: MIT
//
// rations.so — the LV2 DSP half.
//
// It is a HOST, not a second plug-in: it instantiates the same RationsProcessor a DAW instantiates
// and drives it through IAudioProcessor, exactly as standalone/jackclient.cpp drives it from a JACK
// callback. Everything measured about the chain — the channel switch, the pedal placement, the
// output modes, the state format — is that code and not a copy of it. See lv2/rationslv2.h.
//
// WHAT RUNS WHERE, which is the whole of the real-time story:
//
//   run()            LV2's audio thread. Reads the control ports, turns MIDI into the parameter
//                    points and note events VST3 expects, calls process(), and copies the
//                    feedback out. Allocates nothing: every VST3 process structure is built in
//                    instantiate(), the parameter queues are pre-sized AND pre-touched, and the
//                    two message rings are fixed-capacity.
//
//   messageLoop()    one thread this file owns, and the reason it exists is not obvious. A
//                    controller -> processor message is handled by RationsProcessor::notify, which
//                    scans a directory, parses JSON and takes ModelBank's mutex. In VST3 that
//                    lands on the host's message thread. In LV2 the same message arrives as an
//                    atom IN run(), so calling notify() there would put file I/O and a mutex on
//                    the audio thread, which the real-time contract forbids outright. So run()
//                    copies the
//                    atom into a ring and this thread drains it.
//
//                    The LV2 way to say that is work:schedule, and it was the first plan. It is
//                    not used because it is an OPTIONAL host feature: a host that does not offer
//                    it would leave an amp that cannot load a capture, which is the whole product.
//                    A thread of our own works in every host, and it is not a new class of thing
//                    here — the plug-in already owns four bank workers and a prime worker, and
//                    this one is simply the message thread the VST3 build gets from its host.
//
//   save()/restore() the host's own non-RT context, which is where getState/setState belong.
//
// The two rings are the only traffic between run() and the message thread, and neither direction
// ever blocks the audio thread: run() writes a request and moves on, and reads whatever replies
// happen to be waiting.

#include "lv2message.h"
#include "rationslv2.h"

#include "rationsprocessor.h"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Rations;
using namespace Rations::lv2;

namespace
{

//------------------------------------------------------------------------
// A fixed-capacity ring of serialized atoms.
//
// Single producer, single consumer, and the two indices are the whole of the synchronisation: the
// producer publishes its write index with a release store after the bytes are in place, and the
// consumer's acquire load of it is what makes those bytes visible. No allocation and no lock on
// either side, which is what lets the audio thread be one of the two.
//
// A message too big for a slot is DROPPED and said so, rather than truncated: half a capture path
// is a load of the wrong folder, and half a state blob is a project that opens wrong.
class AtomRing
{
public:
    static constexpr std::uint32_t kSlots = 8;
    static constexpr std::uint32_t kSlotBytes = 32 * 1024;

    bool push(const void *data, std::uint32_t size)
    {
        if (size == 0 || size > kSlotBytes)
            return false;
        const std::uint32_t w = mWrite.load(std::memory_order_relaxed);
        const std::uint32_t r = mRead.load(std::memory_order_acquire);
        if (w - r >= kSlots)
            return false; // full
        Slot &slot = mSlots[w % kSlots];
        std::memcpy(slot.bytes, data, size);
        slot.size = size;
        mWrite.store(w + 1, std::memory_order_release);
        return true;
    }

    // The oldest slot, or nullptr when empty. Valid until drop().
    const std::uint8_t *peek(std::uint32_t &size) const
    {
        const std::uint32_t r = mRead.load(std::memory_order_relaxed);
        if (mWrite.load(std::memory_order_acquire) == r)
            return nullptr;
        const Slot &slot = mSlots[r % kSlots];
        size = slot.size;
        return slot.bytes;
    }

    void drop()
    {
        mRead.store(mRead.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

private:
    struct Slot {
        std::uint32_t size = 0;
        // 8-byte aligned because an LV2_Atom is read in place out of this buffer and its bodies
        // are 64-bit aligned by the atom spec's own padding rule.
        alignas(8) std::uint8_t bytes[kSlotBytes];
    };
    Slot mSlots[kSlots];
    std::atomic<std::uint32_t> mWrite{0};
    std::atomic<std::uint32_t> mRead{0};
};

//------------------------------------------------------------------------
class RationsLv2;

// The processor's peer. RationsProcessor::sendMessage lands here, on the message thread, and the
// atom is queued for run() to write into the notify port.
class UiPeer : public Vst::IConnectionPoint
{
public:
    explicit UiPeer(RationsLv2 &owner) : mOwner(owner)
    {
    }

    tresult PLUGIN_API connect(IConnectionPoint *) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint *) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API notify(Vst::IMessage *message) SMTG_OVERRIDE;

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Vst::IConnectionPoint::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IConnectionPoint *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    // Non-deleting, and it has to be: ComponentBase holds its peer in an IPtr, so connect()
    // addRefs this and disconnect() releases it — and the generated release() calls `delete this`,
    // which on a member of the wrapper is a free of an interior pointer. Found by running it:
    // lilv_instance_free aborted in munmap_chunk on the way out of the destructor.
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    RationsLv2 &mOwner;
};

//------------------------------------------------------------------------
class RationsLv2
{
public:
    RationsLv2() : mHostApp("NAMp Rations LV2"), mUiPeer(*this)
    {
    }

    bool instantiate(double rate, const char *bundlePath, const LV2_Feature *const *features);
    void connectPort(std::uint32_t port, void *data);
    void activate();
    void run(std::uint32_t nframes);
    void deactivate();
    ~RationsLv2();

    LV2_State_Status save(LV2_State_Store_Function store, LV2_State_Handle handle,
                          const LV2_Feature *const *features);
    LV2_State_Status restore(LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
                             const LV2_Feature *const *features);

    // Message thread -> run(). Called with mReplyMutex held by the only two producers.
    void queueReply(Vst::IMessage *message);

private:
    void messageLoop();
    void dispatch(Message *message);
    void sendStateToUi();
    void handleMidi(const std::uint8_t *bytes, std::uint32_t size);
    void readAtomInput();
    void writeAtomOutput();
    void pushPoint(Vst::ParamID id, double normalized);
    // Send one load message into the processor exactly as the controller would. Used by restore()
    // when state:mapPath says a file has moved.
    void sendCaptureLoad(int channel, const std::string &path, bool isDirectory);
    void sendIrLoad(int slot, const std::string &path);

    // --- host features ---------------------------------------------------
    LV2_URID_Map *mMap = nullptr;
    MessageUris mUris;
    LV2_URID mMidiEvent = 0;
    LV2_URID mTimePosition = 0;
    LV2_URID mTimeBpm = 0;
    LV2_URID mAtomFloat = 0;
    LV2_URID mAtomDouble = 0;
    LV2_URID mAtomInt = 0;
    LV2_URID mAtomLong = 0;
    LV2_URID mStateBlob = 0;
    LV2_URID mAtomChunk = 0;
    LV2_URID mAtomString = 0;
    LV2_URID mAtomPath = 0;
    LV2_URID mCaptureDirs = 0;
    LV2_URID mPathAbstract[kPathSlotCount] = {};
    LV2_URID mPathRaw[kPathSlotCount] = {};

    // --- the plug-in ------------------------------------------------------
    HostApp mHostApp;
    UiPeer mUiPeer;
    IPtr<RationsProcessor> mProcessor;

    // --- ports ------------------------------------------------------------
    const float *mAudioIn = nullptr;
    float *mAudioOut[2] = {nullptr, nullptr};
    const LV2_Atom_Sequence *mAtomIn = nullptr;
    LV2_Atom_Sequence *mAtomOut = nullptr;
    const float *mControl[kControlInCount] = {};
    float *mFeedback[kFeedbackCount] = {};
    float *mLatency = nullptr;

    // Last value seen on each control port, so a point is pushed only when the host actually moved
    // something. NaN so the first block always publishes, which is what gets the plug-in to the
    // host's restored values before the first sample is processed.
    double mLastControl[kControlInCount];

    // --- VST3 process plumbing, all built in instantiate() ----------------
    Vst::ProcessData mData;
    Vst::AudioBusBuffers mInBus;
    Vst::AudioBusBuffers mOutBus;
    float *mOutPtrs[2] = {nullptr, nullptr};
    const float *mInPtr = nullptr;
    Vst::ParameterChanges mInputChanges;
    Vst::ParameterChanges mOutputChanges;
    Vst::EventList mEvents;
    Vst::ProcessContext mContext = {};

    // --- the message thread -----------------------------------------------
    AtomRing mToMessage; // run() -> messageLoop()
    AtomRing mToRun;     // messageLoop() (and restore()) -> run()
    // The reply ring has two producers — the message thread and whichever thread the host calls
    // restore() on — so its push side is serialized here. run() is the single consumer and never
    // touches this.
    std::mutex mReplyMutex;
    std::thread mMessageThread;
    std::atomic<bool> mMessageRunning{false};

    // Scratch for forging one reply, owned by whichever producer holds mReplyMutex.
    std::vector<std::uint8_t> mReplyScratch;
    LV2_Atom_Forge mReplyForge = {};
    // run()'s own forge, aimed at the notify port.
    LV2_Atom_Forge mOutForge = {};

    double mSampleRate = 48000.0;
    std::int32_t mMaxBlockSize = 4096;
    // Warned once rather than every block, because a full ring under a stuck message thread would
    // otherwise print at the audio rate.
    std::atomic<bool> mWarnedDropped{false};

    // What the plug-in's file paths are, mirrored here for the same reason RationsController
    // mirrors them: this half writes the LV2 state, and the processor's copies are private to it.
    // Written on the message thread and by restore(); read by save(). Both are the host's own
    // non-RT calls, and the host does not overlap them.
    std::string mPath[kPathSlotCount];
    bool mCaptureIsDir[kChannelCount] = {false, false, false, false};
};

//------------------------------------------------------------------------
tresult PLUGIN_API UiPeer::notify(Vst::IMessage *message)
{
    mOwner.queueReply(message);
    return kResultOk;
}

//------------------------------------------------------------------------
bool RationsLv2::instantiate(double rate, const char *bundlePath,
                             const LV2_Feature *const *features)
{
    (void)bundlePath; // the DSP loads no art; the UI does, from its own copy of this path

    for (int i = 0; features && features[i]; ++i) {
        if (!features[i]->URI)
            continue;
        if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
            mMap = static_cast<LV2_URID_Map *>(features[i]->data);
    }
    if (!mMap || !mMap->map) {
        fprintf(stderr, "Rations: the host provides no urid:map; the plug-in cannot run\n");
        return false;
    }

    mUris.map(mMap);
    mMidiEvent = mMap->map(mMap->handle, LV2_MIDI__MidiEvent);
    mTimePosition = mMap->map(mMap->handle, LV2_TIME__Position);
    mTimeBpm = mMap->map(mMap->handle, LV2_TIME__beatsPerMinute);
    mAtomFloat = mMap->map(mMap->handle, LV2_ATOM__Float);
    mAtomDouble = mMap->map(mMap->handle, LV2_ATOM__Double);
    mAtomInt = mMap->map(mMap->handle, LV2_ATOM__Int);
    mAtomLong = mMap->map(mMap->handle, LV2_ATOM__Long);
    mAtomChunk = mMap->map(mMap->handle, LV2_ATOM__Chunk);
    mAtomString = mMap->map(mMap->handle, LV2_ATOM__String);
    mAtomPath = mMap->map(mMap->handle, LV2_ATOM__Path);
    mStateBlob = mMap->map(mMap->handle, kStateBlobUri);
    mCaptureDirs = mMap->map(mMap->handle, kStateCaptureDirsUri);
    for (int slot = 0; slot < kPathSlotCount; ++slot) {
        const std::string abstractUri = std::string(kStatePathAbstractPrefix) + kPathSlotName[slot];
        const std::string rawUri = std::string(kStatePathRawPrefix) + kPathSlotName[slot];
        mPathAbstract[slot] = mMap->map(mMap->handle, abstractUri.c_str());
        mPathRaw[slot] = mMap->map(mMap->handle, rawUri.c_str());
    }

    // The largest block the host says it will ever ask for. The processor loops in whole
    // sub-blocks of whatever it was set up with, so a host that exceeds this is handled rather
    // than clamped; what the figure really decides is how much buffer is allocated once.
    bool sawMaxBlock = false;
    for (int i = 0; features && features[i]; ++i) {
        if (!features[i]->URI || std::strcmp(features[i]->URI, LV2_OPTIONS__options) != 0)
            continue;
        const LV2_URID wanted = mMap->map(mMap->handle, LV2_BUF_SIZE__maxBlockLength);
        const auto *option = static_cast<const LV2_Options_Option *>(features[i]->data);
        for (; option && option->key; ++option) {
            if (option->key != wanted || !option->value)
                continue;
            if (option->type == mAtomInt) {
                mMaxBlockSize = *static_cast<const std::int32_t *>(option->value);
                sawMaxBlock = true;
            } else if (option->type == mAtomLong) {
                mMaxBlockSize =
                    static_cast<std::int32_t>(*static_cast<const std::int64_t *>(option->value));
                sawMaxBlock = true;
            }
        }
    }
    if (!sawMaxBlock)
        fprintf(stderr,
                "Rations: the host states no bufsz:maxBlockLength; sizing buffers for "
                "%d frames and chunking anything larger\n",
                4096);
    mMaxBlockSize = std::clamp<std::int32_t>(mMaxBlockSize, 16, 1 << 16);
    mSampleRate = rate;

    mProcessor = owned(new (std::nothrow) RationsProcessor());
    if (!mProcessor)
        return false;
    // Before anything else: allocateMessage() asks the host context for its IMessage instances, so
    // a processor initialised without one silently drops every reply to the editor.
    if (mProcessor->initialize(&mHostApp) != kResultOk) {
        fprintf(stderr, "Rations: the processor refused to initialise\n");
        return false;
    }
    mProcessor->connect(&mUiPeer);

    // The buses initialize() declared are mono in / stereo out, which is exactly the port layout
    // above, so there is no arrangement to negotiate. Activating them is what a host does and what
    // keeps the two halves' idea of the bus state the same.
    mProcessor->activateBus(Vst::kAudio, Vst::kInput, 0, true);
    mProcessor->activateBus(Vst::kAudio, Vst::kOutput, 0, true);
    mProcessor->activateBus(Vst::kEvent, Vst::kInput, 0, true);

    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = mMaxBlockSize;
    setup.sampleRate = mSampleRate;
    if (mProcessor->setupProcessing(setup) != kResultOk) {
        fprintf(stderr, "Rations: the processor rejected the process setup\n");
        return false;
    }

    // --- the process structures, built once ------------------------------
    mInBus.numChannels = 1;
    mInBus.channelBuffers32 = const_cast<float **>(&mInPtr);
    mInBus.silenceFlags = 0;
    mOutBus.numChannels = 2;
    mOutBus.channelBuffers32 = mOutPtrs;
    mOutBus.silenceFlags = 0;

    mData.numInputs = 1;
    mData.numOutputs = 1;
    mData.inputs = &mInBus;
    mData.outputs = &mOutBus;
    mData.symbolicSampleSize = Vst::kSample32;
    mData.processMode = Vst::kRealtime;
    mData.inputParameterChanges = &mInputChanges;
    mData.outputParameterChanges = &mOutputChanges;
    mData.inputEvents = &mEvents;
    mData.processContext = &mContext;
    mContext.sampleRate = mSampleRate;

    // One queue per parameter the wrapper can ever write: the 48 controls, the 128 CC lanes a
    // footswitch may land on, and Program Change. Sized here AND pre-touched below, because
    // ParameterChanges::addParameterData grows a vector when it runs out of reserved queues and
    // ParameterValueQueue::addPoint grows one when it runs out of reserved points — both of which
    // are a malloc on the audio thread.
    const int32 maxIn = kControlInCount + kMidiCcCount + 1;
    mInputChanges.setMaxParameters(maxIn);
    mOutputChanges.setMaxParameters(kFeedbackCount + kMidiLearnRowCount);
    mEvents.setMaxSize(256);

    for (int i = 0; i < kControlInCount; ++i)
        pushPoint(controlPortParam(i), 0.0);
    for (int cc = 0; cc < kMidiCcCount; ++cc)
        pushPoint(static_cast<Vst::ParamID>(kMidiCcBaseId + cc), 0.0);
    pushPoint(kMidiProgramChangeId, 0.0);
    mInputChanges.clearQueue();

    for (int i = 0; i < kControlInCount; ++i)
        mLastControl[i] = std::nan("");

    mReplyScratch.resize(AtomRing::kSlotBytes);
    lv2_atom_forge_init(&mReplyForge, mMap);
    lv2_atom_forge_init(&mOutForge, mMap);

    mMessageRunning.store(true, std::memory_order_release);
    mMessageThread = std::thread([this] { messageLoop(); });
    return true;
}

//------------------------------------------------------------------------
RationsLv2::~RationsLv2()
{
    mMessageRunning.store(false, std::memory_order_release);
    if (mMessageThread.joinable())
        mMessageThread.join();
    if (mProcessor) {
        mProcessor->disconnect(&mUiPeer);
        mProcessor->terminate();
    }
}

//------------------------------------------------------------------------
void RationsLv2::connectPort(std::uint32_t port, void *data)
{
    switch (port) {
        case kPortAudioIn:
            mAudioIn = static_cast<const float *>(data);
            return;
        case kPortAudioOutL:
            mAudioOut[0] = static_cast<float *>(data);
            return;
        case kPortAudioOutR:
            mAudioOut[1] = static_cast<float *>(data);
            return;
        case kPortAtomIn:
            mAtomIn = static_cast<const LV2_Atom_Sequence *>(data);
            return;
        case kPortAtomOut:
            mAtomOut = static_cast<LV2_Atom_Sequence *>(data);
            return;
        default:
            break;
    }
    if (port >= kPortControlFirst && port < kPortFeedbackFirst) {
        mControl[port - kPortControlFirst] = static_cast<const float *>(data);
    } else if (port >= kPortFeedbackFirst && port < kPortLatency) {
        mFeedback[port - kPortFeedbackFirst] = static_cast<float *>(data);
    } else if (port == kPortLatency) {
        mLatency = static_cast<float *>(data);
    }
}

//------------------------------------------------------------------------
void RationsLv2::activate()
{
    if (!mProcessor)
        return;
    mProcessor->setActive(true);
    mProcessor->setProcessing(true);
}

void RationsLv2::deactivate()
{
    if (!mProcessor)
        return;
    mProcessor->setProcessing(false);
    mProcessor->setActive(false);
}

//------------------------------------------------------------------------
// One parameter point, on the audio thread, allocating nothing.
//
// Two SDK behaviours make that true and neither is obvious. addParameterData reuses a queue that
// setMaxParameters already reserved, and grows its vector when it runs out — which is why every id
// this wrapper can ever write is counted there. And addPoint REPLACES a point at the same sample
// offset rather than appending one (public.sdk/source/vst/hosting/parameterchanges.cpp), so the
// offset of 0 used here means a parameter written twice in one block costs no second point: six CC
// messages on one controller number in one block are six stores into one slot.
//
// Sample-accurate automation is given up by that choice, and it costs nothing: every parameter here
// is a control rather than a signal, and the processor reads only the last point of each queue.
void RationsLv2::pushPoint(Vst::ParamID id, double normalized)
{
    int32 index = 0;
    if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(id, index))
        queue->addPoint(0, normalized, index);
}

//------------------------------------------------------------------------
// One MIDI message, converted into whatever route VST3 actually delivers it by. The three are NOT
// alike and only one of them is still MIDI when it lands — src/midilearn.h sets out all three
// against the SDK sites they were verified at, and standalone/midiroute.h does this same job for
// the JACK standalone.
//
// The standalone DISCOVERS the mapping through IMidiMapping and IUnitInfo because it dlopens a
// bundle it is a stranger to. This file is inside the same source tree as the controller that
// declares it, so the two lines below name what src/rationsids.h defines: a CC lands on
// kMidiCcBaseId + cc, and a Program Change on kMidiProgramChangeId, whose value is the program
// number over the program list's 127 steps. tools/rations_ttlgen.cpp checks both against the
// controller, so a change to either would fail the build rather than lose a footswitch.
void RationsLv2::handleMidi(const std::uint8_t *bytes, std::uint32_t size)
{
    if (size < 2)
        return;
    const std::uint8_t status = bytes[0] & 0xF0;
    const std::uint8_t channel = bytes[0] & 0x0F;

    if (status == LV2_MIDI_MSG_CONTROLLER && size >= 3) {
        const int cc = bytes[1] & 0x7F;
        const int value = bytes[2] & 0x7F;
        pushPoint(static_cast<Vst::ParamID>(kMidiCcBaseId + cc),
                  static_cast<double>(value) / 127.0);
        return;
    }
    if (status == LV2_MIDI_MSG_PGM_CHANGE) {
        const int program = bytes[1] & 0x7F;
        pushPoint(kMidiProgramChangeId, static_cast<double>(program) / 127.0);
        return;
    }
    if (status == LV2_MIDI_MSG_NOTE_ON && size >= 3) {
        // The one message that arrives as a real event and the only one that still knows its MIDI
        // channel, which is why a learned note is the only binding that can be pinned to one.
        // A zero velocity is a note OFF by the oldest convention in MIDI; the processor drops it,
        // and passing it through unchanged is what lets it make that decision rather than this
        // file making it twice.
        Vst::Event event = {};
        event.type = Vst::Event::kNoteOnEvent;
        event.sampleOffset = 0;
        event.noteOn.channel = channel;
        event.noteOn.pitch = static_cast<int16>(bytes[1] & 0x7F);
        event.noteOn.velocity = static_cast<float>(bytes[2] & 0x7F) / 127.0f;
        event.noteOn.noteId = -1;
        mEvents.addEvent(event);
    }
}

//------------------------------------------------------------------------
void RationsLv2::readAtomInput()
{
    if (!mAtomIn)
        return;
    LV2_ATOM_SEQUENCE_FOREACH(mAtomIn, ev)
    {
        const LV2_Atom *atom = &ev->body;
        if (atom->type == mMidiEvent) {
            handleMidi(static_cast<const std::uint8_t *>(LV2_ATOM_BODY_CONST(atom)), atom->size);
            continue;
        }
        if (!lv2_atom_forge_is_object_type(&mOutForge, atom->type))
            continue;
        const auto *object = reinterpret_cast<const LV2_Atom_Object *>(atom);

        if (object->body.otype == mTimePosition) {
            // The Delay's sync divisions want the host's tempo, which VST3 carries in a
            // ProcessContext and LV2 carries as a time:Position atom. A host is obliged to give
            // neither, so an absent or unusable tempo leaves the flag clear and the Delay falls
            // back to its own free-running time — the same fallback the VST3 path takes.
            const LV2_Atom *bpm = nullptr;
            lv2_atom_object_get(object, mTimeBpm, &bpm, 0);
            double tempo = 0.0;
            if (bpm && bpm->type == mAtomFloat)
                tempo = *static_cast<const float *>(LV2_ATOM_BODY_CONST(bpm));
            else if (bpm && bpm->type == mAtomDouble)
                tempo = *static_cast<const double *>(LV2_ATOM_BODY_CONST(bpm));
            if (tempo > 0.0) {
                mContext.tempo = tempo;
                mContext.state |= Vst::ProcessContext::kTempoValid;
            }
            continue;
        }

        if (object->body.otype == mUris.message) {
            // Straight into the ring, bytes and all: parsing it here would mean allocating on the
            // audio thread, and acting on it here would mean file I/O.
            if (!mToMessage.push(atom, lv2_atom_total_size(atom)) &&
                !mWarnedDropped.exchange(true, std::memory_order_relaxed)) {
                fprintf(stderr, "Rations: an editor message was dropped; the message queue is "
                                "full or the message is too large\n");
            }
        }
    }
}

//------------------------------------------------------------------------
void RationsLv2::writeAtomOutput()
{
    if (!mAtomOut)
        return;
    // The capacity the host gave us is in the atom's own size field before we overwrite it, which
    // is the LV2 convention for an output atom port.
    const std::uint32_t capacity = mAtomOut->atom.size;
    lv2_atom_forge_set_buffer(&mOutForge, reinterpret_cast<std::uint8_t *>(mAtomOut), capacity);

    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_sequence_head(&mOutForge, &frame, 0);

    for (;;) {
        std::uint32_t size = 0;
        const std::uint8_t *bytes = mToRun.peek(size);
        if (!bytes)
            break;
        // Stop at the first one that does not fit and leave it in the ring for the next block,
        // rather than dropping it: a reply the editor never receives is a panel that never
        // catches up, and one block later there is room.
        if (mOutForge.offset + sizeof(LV2_Atom_Event) + size > mOutForge.size)
            break;
        if (!lv2_atom_forge_frame_time(&mOutForge, 0))
            break;
        if (!lv2_atom_forge_write(&mOutForge, bytes, size))
            break;
        mToRun.drop();
    }

    lv2_atom_forge_pop(&mOutForge, &frame);
}

//------------------------------------------------------------------------
void RationsLv2::run(std::uint32_t nframes)
{
    if (!mProcessor || !mAudioIn || !mAudioOut[0])
        return;

    mInputChanges.clearQueue();
    mOutputChanges.clearQueue();
    mEvents.clear();

    // Every control port whose value has moved becomes one parameter point. The host owns these
    // values, so this is also how a project's restored settings arrive: the first block sees every
    // one of them differ from the NaN they start at.
    for (int i = 0; i < kControlInCount; ++i) {
        if (!mControl[i])
            continue;
        const double plain = static_cast<double>(*mControl[i]);
        if (plain == mLastControl[i])
            continue;
        mLastControl[i] = plain;
        pushPoint(controlPortParam(i), controlNorm(controlSpec(i), plain));
    }

    readAtomInput();

    mInPtr = mAudioIn;
    mOutPtrs[0] = mAudioOut[0];
    // A host is obliged to connect every port, but a disconnected right channel would be a null
    // dereference inside the processor rather than a missing channel, so it is checked here and
    // the bus narrowed instead.
    mOutPtrs[1] = mAudioOut[1];
    mOutBus.numChannels = mAudioOut[1] ? 2 : 1;
    mData.numSamples = static_cast<int32>(nframes);
    mProcessor->process(mData);

    // The feedback block: two meters, the bank build progress, which capture is sounding and which
    // channel is. The processor publishes them through the output parameter queue every block, and
    // the ui:portNotification declarations in the TTL are what carry them on to the editor.
    //
    // The SAME queue also carries the echo of anything the MIDI learn table did to the plug-in's
    // own parameters, and dropping that half is a footswitch that changes the sound while the
    // panel sits still — found on the standalone with a real footswitch, recorded in
    // standalone/jackclient.h. Here it needs no special case: an echo lands on a control INPUT
    // port, which the host notifies the UI about by default, so writing it back is all there is.
    const int32 changed = mOutputChanges.getParameterCount();
    for (int32 q = 0; q < changed; ++q) {
        Vst::IParamValueQueue *queue = mOutputChanges.getParameterData(q);
        if (!queue)
            continue;
        const int32 points = queue->getPointCount();
        if (points <= 0)
            continue;
        int32 offset = 0;
        Vst::ParamValue value = 0.0;
        if (queue->getPoint(points - 1, offset, value) != kResultTrue)
            continue;
        const Vst::ParamID id = queue->getParameterId();

        bool placed = false;
        for (int f = 0; f < kFeedbackCount; ++f) {
            if (kFeedbackIds[f] != id)
                continue;
            if (mFeedback[f])
                *mFeedback[f] = static_cast<float>(value);
            placed = true;
            break;
        }
        if (placed)
            continue;
        // An echo. The host owns a control input port's value, so the plug-in cannot write it;
        // what it can do is stop overriding it, which is what clearing the remembered value does —
        // the next block sees the host's own value differ and republishes it. The editor learns
        // about the change through its own copy of the parameter, which it set when the learn
        // table fired.
        for (int i = 0; i < kControlInCount; ++i) {
            if (controlPortParam(i) == id) {
                mLastControl[i] = std::nan("");
                break;
            }
        }
    }

    writeAtomOutput();

    if (mLatency)
        *mLatency = static_cast<float>(mProcessor->getLatencySamples());
}

//------------------------------------------------------------------------
void RationsLv2::queueReply(Vst::IMessage *message)
{
    auto *concrete = static_cast<Message *>(message);
    if (!concrete)
        return;
    std::lock_guard<std::mutex> lock(mReplyMutex);
    lv2_atom_forge_set_buffer(&mReplyForge, mReplyScratch.data(),
                              static_cast<std::uint32_t>(mReplyScratch.size()));
    if (!forgeMessage(mReplyForge, mUris, message, concrete->attributes())) {
        fprintf(stderr, "Rations: a reply to the editor did not fit and was dropped (%s)\n",
                concrete->id().c_str());
        return;
    }
    const auto *atom = reinterpret_cast<const LV2_Atom *>(mReplyScratch.data());
    if (!mToRun.push(atom, lv2_atom_total_size(atom)))
        fprintf(stderr, "Rations: the editor is not draining its messages; one was dropped\n");
}

//------------------------------------------------------------------------
void RationsLv2::sendStateToUi()
{
    if (!mProcessor)
        return;
    MemoryStream stream;
    if (mProcessor->getState(&stream) != kResultOk)
        return;

    Message reply;
    reply.setMessageID(kMsgLv2State);
    reply.attributes().setBinary(kLv2StateAttr, stream.getData(),
                                 static_cast<uint32>(stream.getSize()));
    queueReply(&reply);
}

//------------------------------------------------------------------------
void RationsLv2::dispatch(Message *message)
{
    if (!message)
        return;
    // The wrapper's own two messages never reach the processor: they exist because LV2 has no
    // setComponentState, and the answer is one the wrapper can give on its own.
    if (message->id() == kMsgLv2RequestState) {
        sendStateToUi();
        return;
    }

    // Everything else is ordinary VST3 traffic and goes straight in. Mirror the paths on the way
    // past, because this half is the one that writes the LV2 state and the processor's own copies
    // are private to it.
    const std::string &id = message->id();
    for (int c = 0; c < kChannelCount; ++c) {
        if (id != kMsgLoadCapture[c])
            continue;
        const void *data = nullptr;
        uint32 size = 0;
        if (message->getAttributes()->getBinary(kMsgPathAttr, data, size) == kResultOk && data)
            mPath[c].assign(static_cast<const char *>(data), size);
        else
            mPath[c].clear();
        int64 isDir = 0;
        message->getAttributes()->getInt(kMsgIsDirAttr, isDir);
        mCaptureIsDir[c] = !mPath[c].empty() && isDir != 0;
    }
    for (int slot = 0; slot < kIrSlotCount; ++slot) {
        if (id != kMsgLoadIr[slot])
            continue;
        const void *data = nullptr;
        uint32 size = 0;
        if (message->getAttributes()->getBinary(kMsgPathAttr, data, size) == kResultOk && data)
            mPath[kPathSlotIrFirst + slot].assign(static_cast<const char *>(data), size);
        else
            mPath[kPathSlotIrFirst + slot].clear();
    }

    mProcessor->notify(message);
}

//------------------------------------------------------------------------
void RationsLv2::messageLoop()
{
    // A poll rather than a condition variable, because the producer is the audio thread and
    // notifying a condition variable takes its mutex. Ten milliseconds is far below anything a
    // person notices for a file load and costs a hundred wakeups a second on a thread that does
    // nothing the rest of the time.
    using namespace std::chrono_literals;
    while (mMessageRunning.load(std::memory_order_acquire)) {
        for (;;) {
            std::uint32_t size = 0;
            const std::uint8_t *bytes = mToMessage.peek(size);
            if (!bytes)
                break;
            const auto *atom = reinterpret_cast<const LV2_Atom *>(bytes);
            if (lv2_atom_forge_is_object_type(&mOutForge, atom->type)) {
                if (Message *message =
                        parseMessage(reinterpret_cast<const LV2_Atom_Object *>(atom), mUris)) {
                    dispatch(message);
                    message->release();
                }
            }
            mToMessage.drop();
        }
        std::this_thread::sleep_for(10ms);
    }
}

//------------------------------------------------------------------------
void RationsLv2::sendCaptureLoad(int channel, const std::string &path, bool isDirectory)
{
    if (channel < 0 || channel >= kChannelCount)
        return;
    Message message;
    message.setMessageID(kMsgLoadCapture[channel]);
    message.attributes().setBinary(kMsgPathAttr, path.data(), static_cast<uint32>(path.size()));
    message.attributes().setInt(kMsgIsDirAttr, (!path.empty() && isDirectory) ? 1 : 0);
    mProcessor->notify(&message);
    mPath[channel] = path;
    mCaptureIsDir[channel] = !path.empty() && isDirectory;
}

void RationsLv2::sendIrLoad(int slot, const std::string &path)
{
    if (slot < 0 || slot >= kIrSlotCount)
        return;
    Message message;
    message.setMessageID(kMsgLoadIr[slot]);
    message.attributes().setBinary(kMsgPathAttr, path.data(), static_cast<uint32>(path.size()));
    mProcessor->notify(&message);
    mPath[kPathSlotIrFirst + slot] = path;
}

//------------------------------------------------------------------------
LV2_State_Status RationsLv2::save(LV2_State_Store_Function store, LV2_State_Handle handle,
                                  const LV2_Feature *const *features)
{
    if (!mProcessor || !store)
        return LV2_STATE_ERR_UNKNOWN;

    const LV2_State_Map_Path *mapPath = nullptr;
    const LV2_State_Free_Path *freePath = nullptr;
    for (int i = 0; features && features[i]; ++i) {
        if (!features[i]->URI)
            continue;
        if (std::strcmp(features[i]->URI, LV2_STATE__mapPath) == 0)
            mapPath = static_cast<const LV2_State_Map_Path *>(features[i]->data);
        else if (std::strcmp(features[i]->URI, LV2_STATE__freePath) == 0)
            freePath = static_cast<const LV2_State_Free_Path *>(features[i]->data);
    }

    MemoryStream stream;
    if (mProcessor->getState(&stream) != kResultOk)
        return LV2_STATE_ERR_UNKNOWN;
    store(handle, mStateBlob, stream.getData(), static_cast<size_t>(stream.getSize()), mAtomChunk,
          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

    std::uint32_t dirs = 0;
    for (int c = 0; c < kChannelCount; ++c)
        if (mCaptureIsDir[c])
            dirs |= 1u << c;
    const std::int32_t dirsValue = static_cast<std::int32_t>(dirs);
    store(handle, mCaptureDirs, &dirsValue, sizeof(dirsValue), mAtomInt,
          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

    for (int slot = 0; slot < kPathSlotCount; ++slot) {
        if (mPath[slot].empty())
            continue;
        // The raw path is what the blob above contains; the abstract one is what survives the
        // session being moved. Restore compares them, which is what lets it tell a move from an
        // ordinary reopen without parsing a blob it does not own.
        store(handle, mPathRaw[slot], mPath[slot].c_str(), mPath[slot].size() + 1, mAtomString,
              LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
        if (!mapPath || !mapPath->abstract_path)
            continue;
        char *abstract = mapPath->abstract_path(mapPath->handle, mPath[slot].c_str());
        if (!abstract)
            continue;
        store(handle, mPathAbstract[slot], abstract, std::strlen(abstract) + 1, mAtomPath,
              LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
        if (freePath && freePath->free_path)
            freePath->free_path(freePath->handle, abstract);
        else
            std::free(abstract);
    }
    return LV2_STATE_SUCCESS;
}

//------------------------------------------------------------------------
LV2_State_Status RationsLv2::restore(LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
                                     const LV2_Feature *const *features)
{
    if (!mProcessor || !retrieve)
        return LV2_STATE_ERR_UNKNOWN;

    const LV2_State_Map_Path *mapPath = nullptr;
    const LV2_State_Free_Path *freePath = nullptr;
    for (int i = 0; features && features[i]; ++i) {
        if (!features[i]->URI)
            continue;
        if (std::strcmp(features[i]->URI, LV2_STATE__mapPath) == 0)
            mapPath = static_cast<const LV2_State_Map_Path *>(features[i]->data);
        else if (std::strcmp(features[i]->URI, LV2_STATE__freePath) == 0)
            freePath = static_cast<const LV2_State_Free_Path *>(features[i]->data);
    }

    size_t size = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    const void *blob = retrieve(handle, mStateBlob, &size, &type, &flags);
    if (blob && size > 0) {
        // The blob is untrusted input from a project file, which is what setState is already
        // written to assume: a malformed one is a clean refusal there, never a
        // crash here.
        MemoryStream stream(const_cast<void *>(blob), static_cast<TSize>(size));
        mProcessor->setState(&stream);
    }

    const void *dirsValue = retrieve(handle, mCaptureDirs, &size, &type, &flags);
    std::uint32_t dirs = 0;
    if (dirsValue && size == sizeof(std::int32_t))
        dirs = static_cast<std::uint32_t>(*static_cast<const std::int32_t *>(dirsValue));

    for (int slot = 0; slot < kPathSlotCount; ++slot) {
        const void *abstract = retrieve(handle, mPathAbstract[slot], &size, &type, &flags);
        if (!abstract || size == 0)
            continue;
        const std::string abstractPath(static_cast<const char *>(abstract),
                                       strnlen(static_cast<const char *>(abstract), size));
        if (abstractPath.empty())
            continue;

        std::string absolute = abstractPath;
        if (mapPath && mapPath->absolute_path) {
            if (char *resolved = mapPath->absolute_path(mapPath->handle, abstractPath.c_str())) {
                absolute = resolved;
                if (freePath && freePath->free_path)
                    freePath->free_path(freePath->handle, resolved);
                else
                    std::free(resolved);
            }
        }

        std::string raw;
        if (const void *rawValue = retrieve(handle, mPathRaw[slot], &size, &type, &flags)) {
            if (size > 0)
                raw.assign(static_cast<const char *>(rawValue),
                           strnlen(static_cast<const char *>(rawValue), size));
        }
        mPath[slot] = raw.empty() ? absolute : raw;
        if (slot < kPathSlotIrFirst)
            mCaptureIsDir[slot] = (dirs & (1u << slot)) != 0;

        // Only when the file actually moved. The setState above has already started the bank the
        // blob named, and re-sending an identical path would rebuild it for nothing — thirty-odd
        // models, and that channel back at its ramped-silence gate while they build.
        if (absolute == raw)
            continue;
        if (slot < kPathSlotIrFirst)
            sendCaptureLoad(slot, absolute, (dirs & (1u << slot)) != 0);
        else
            sendIrLoad(slot - kPathSlotIrFirst, absolute);
    }

    // An editor that is already open has a stale mirror of the paths and names now. Push the
    // restored state at it; an editor that is not open simply finds the ring empty when it opens
    // and asks for itself.
    sendStateToUi();
    return LV2_STATE_SUCCESS;
}

//------------------------------------------------------------------------
// The LV2 entry points.
//------------------------------------------------------------------------
LV2_Handle lv2Instantiate(const LV2_Descriptor *, double rate, const char *bundlePath,
                          const LV2_Feature *const *features)
{
    auto *self = new (std::nothrow) RationsLv2();
    if (!self)
        return nullptr;
    if (!self->instantiate(rate, bundlePath, features)) {
        delete self;
        return nullptr;
    }
    return static_cast<LV2_Handle>(self);
}

void lv2ConnectPort(LV2_Handle instance, std::uint32_t port, void *data)
{
    if (auto *self = static_cast<RationsLv2 *>(instance))
        self->connectPort(port, data);
}

void lv2Activate(LV2_Handle instance)
{
    if (auto *self = static_cast<RationsLv2 *>(instance))
        self->activate();
}

void lv2Run(LV2_Handle instance, std::uint32_t nframes)
{
    if (auto *self = static_cast<RationsLv2 *>(instance))
        self->run(nframes);
}

void lv2Deactivate(LV2_Handle instance)
{
    if (auto *self = static_cast<RationsLv2 *>(instance))
        self->deactivate();
}

void lv2Cleanup(LV2_Handle instance)
{
    delete static_cast<RationsLv2 *>(instance);
}

LV2_State_Status lv2Save(LV2_Handle instance, LV2_State_Store_Function store,
                         LV2_State_Handle handle, std::uint32_t, const LV2_Feature *const *features)
{
    auto *self = static_cast<RationsLv2 *>(instance);
    return self ? self->save(store, handle, features) : LV2_STATE_ERR_UNKNOWN;
}

LV2_State_Status lv2Restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
                            LV2_State_Handle handle, std::uint32_t,
                            const LV2_Feature *const *features)
{
    auto *self = static_cast<RationsLv2 *>(instance);
    return self ? self->restore(retrieve, handle, features) : LV2_STATE_ERR_UNKNOWN;
}

const LV2_State_Interface kStateInterface = {lv2Save, lv2Restore};

const void *lv2ExtensionData(const char *uri)
{
    if (uri && std::strcmp(uri, LV2_STATE__interface) == 0)
        return &kStateInterface;
    return nullptr;
}

const LV2_Descriptor kDescriptor = {
    kPluginUri, lv2Instantiate, lv2ConnectPort, lv2Activate,
    lv2Run,     lv2Deactivate,  lv2Cleanup,     lv2ExtensionData,
};

} // namespace

extern "C" {

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &kDescriptor : nullptr;
}

} // extern "C"
