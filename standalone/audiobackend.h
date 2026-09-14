// AudioBackend — the seam between the standalone and whatever is actually moving samples.
//
// Linux is JACK. Windows will be WASAPI. The two have almost nothing in common at the API level —
// JACK calls a registered callback from a thread it owns and hands over its own buffers; WASAPI
// hands out a buffer to fill and expects the client to drive the loop, in a thread the client
// creates — but what the standalone needs from either is the same short list, and it is that list
// this interface is.
//
// THE INTERFACE IS DRAWN NOW RATHER THAN LATER because the alternative is a refactor. Everything
// above it — the component handler, the feedback pump, the buffer-size watcher — is written
// against these methods, so growing a second implementation later is a new file rather than a
// change to every caller. It is the same seam the platform plug-view header already provides for
// windowing: one indirection, drawn once, at the point where the platforms genuinely differ.
//
// WHAT IS *NOT* HERE IS AS MUCH OF THE DESIGN AS WHAT IS. There is no per-block entry point, no
// buffer pointer and no sample format: the backend owns the audio thread and the VST3 process
// plumbing behind it, and the standalone never sees a sample. That is deliberate — the RT contract
// (no allocation, no locks, no logging, no file I/O, and everything pre-allocated in open()) is
// enforceable inside one implementation and unenforceable across an interface that hands buffers
// out. So the three things that genuinely have to cross the boundary cross it as lock-free
// queues, and nothing else crosses at all:
//
//   UI -> RT   parameter edits, through pushParameter(), into an SPSC ring the audio thread drains
//              at the top of each block. Sharing a ParameterChanges between the threads instead
//              would be a plain data race.
//   RT -> UI   everything the plug-in publishes through outputParameterChanges — meters, bank
//              progress, which channel is sounding, and any parameter the plug-in moved by itself
//              — through readFeedback(), coalesced per parameter id. IEditController must never be
//              called from the audio thread, so the audio thread only stores; the UI thread reads
//              and calls the controller.
//   MIDI -> RT the footswitch, which the backend reads inside its own callback. Where each message
//              has to be delivered was resolved on the main thread before the backend was opened
//              (see midiroute.h), precisely so that nothing on the audio thread has to ask the
//              controller anything.
//
// THREADING. Every method here is called on the main thread EXCEPT the reads of sampleRate() and
// blockSize(), which are safe anywhere. An implementation's own audio callback is the only thing
// that touches the processor while the backend is open, and suspendProcessing() is how the main
// thread gets it out of there long enough to reconfigure.

#pragma once

#include "pluginterfaces/vst/vsttypes.h"

#include <cstdint>
#include <string>

namespace Steinberg
{
namespace Vst
{
class IAudioProcessor;
class IComponent;
} // namespace Vst
} // namespace Steinberg

namespace NAMp::host
{
class ChainEngine;
}

namespace Rations
{

class MidiRoute;

//------------------------------------------------------------------------
class AudioBackend
{
public:
    virtual ~AudioBackend() = default;

    // Connect to the audio system and start processing. `processor` must already be set up and
    // activated, and `route` must already have been resolved: both are read from the audio thread
    // the moment processing starts, so neither may be touched afterwards.
    //
    // `clientName` is what the audio system shows the user — a JACK client name, a WASAPI session
    // display name. False means no audio; the editor is still expected to run.
    virtual bool open(const char *clientName, Steinberg::Vst::IAudioProcessor *processor,
                      Steinberg::Vst::IComponent *component, const MidiRoute *route) = 0;

    // Stop the audio thread and release the device. Idempotent, and the destructor must call it:
    // an early return that skips it would otherwise leave a thread running through the teardown of
    // everything it reads.
    virtual void close() = 0;

    virtual bool isOpen() const = 0;

    // --- the rack ---------------------------------------------------------
    // The chain of other people's plug-ins that runs either side of the amp. The engine is the
    // audio-thread half of it and is owned above this interface, because the builder that publishes
    // into it lives on the run loop; a backend only DRIVES it, bracketing its own call to the amp
    // with the engine's beginBlock/beginChunk/endChunk.
    //
    // ON THE INTERFACE RATHER THAN ON ONE IMPLEMENTATION, because every backend has to do this and
    // main.cpp only ever holds the interface. It is also the reason there is still no buffer
    // pointer here: the samples reach the engine inside the callback, on the audio thread, and
    // never cross this boundary.
    //
    // OPTIONAL, AND THE NULL CASE IS THE ONE THAT MUST STAY FREE. With no engine set, or with no
    // chain published to one, the audio path must be exactly what it was before the rack existed —
    // one branch per chunk and no copies. Set before open(), or with processing suspended.
    virtual void setChainEngine(NAMp::host::ChainEngine *engine) = 0;

    // Main thread: the chain's latency has changed, so whatever figure the audio system is
    // reporting to the rest of the graph is stale. Cheap but not free, so it is called on a publish
    // rather than every block. A backend with no notion of graph latency may do nothing.
    virtual void notifyLatencyChanged() = 0;

    // The rate the device is actually running at, and the largest block it will deliver. Both are
    // the device's to decide, not ours — a backend reports what it was given rather than what it
    // was asked for.
    virtual double sampleRate() const = 0;
    virtual int blockSize() const = 0;

    // --- runtime buffer-size changes -------------------------------------
    // An audio system may resize its buffers under a running client — JACK does it on request from
    // any client, and a WASAPI device change does it implicitly. The chunk loop inside an
    // implementation keeps that SAFE on its own (no block larger than the size the processor was
    // set up with ever reaches it), but the processor is then still configured for the old size,
    // sizing its internal buffers and reporting a latency for a block nobody is sending.
    //
    // Reconfiguring is VST3 main-thread work and must not happen in an audio callback, so these
    // three calls hand it to the run loop instead.

    // Main thread: the size the device has moved to, or 0 if it has not moved. Taking it clears it.
    virtual int takeBufferSizeChange() = 0;

    // Main thread: stop the audio callback from entering the processor, and wait until any call
    // already in flight has returned. The backend outputs silence until resumed. FALSE MEANS THE
    // AUDIO THREAD DID NOT RESPOND IN TIME, in which case the caller must NOT touch the processor
    // — it may still be inside it.
    virtual bool suspendProcessing() = 0;

    // Main thread: adopt the new block size and let the audio callback back in.
    virtual void resumeProcessing(int blockSize) = 0;

    // --- the device going away underneath us -------------------------------
    // Main thread: the device asked to be reopened, or stopped existing. Taking it clears it.
    //
    // A BIGGER EVENT THAN A BUFFER-SIZE CHANGE, which is why it is not one. The three calls above
    // reconfigure a processor while the same device keeps running; this says the device itself is
    // no longer the one that was opened, and the only answer is close() and open() again — a new
    // rate, a new block size, new channel counts, possibly a different piece of hardware. The
    // caller does not need to know which of those changed.
    //
    // WHY IT IS ON THE INTERFACE AT ALL, given that JACK has no such event: two of the three
    // backends this project will have do. An ASIO driver asks for exactly this through
    // kAsioResetRequest, which is how it reports that its control panel changed something
    // fundamental, and the request arrives on the DRIVER's thread while it is inside its own
    // callback — there is nowhere to act on it but here. WASAPI's is AUDCLNT_E_DEVICE_INVALIDATED,
    // returned from an ordinary call after the user unplugged the interface or changed the default
    // device. JACK's nearest equivalent is the server going away, which it handles by shutting the
    // client down rather than by asking anyone to reopen anything, so its implementation is a
    // constant false.
    virtual bool takeDeviceReset() = 0;

    // Main thread: queue a normalized parameter change for the next block. False means the ring is
    // full and the change was DROPPED, which is preferable to blocking either thread.
    virtual bool pushParameter(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) = 0;

    // --- what the plug-in publishes back ---------------------------------
    // EVERY parameter the processor writes into outputParameterChanges, whatever it is. Forwarding
    // a hand-picked few was the first version of this in the parent project and it was wrong, in a
    // way that only showed up with a footswitch in hand: the queue carries two kinds of traffic and
    // only one of them was being read.
    //
    //   * The hidden read-only ones the plug-in publishes every block — two meters, the bank build
    //     progress, which capture is sounding, which CHANNEL is sounding.
    //   * An ECHO of any parameter the plug-in changed BY ITSELF, which is what the MIDI learn
    //     table does on the audio thread. A parameter the plug-in moved and did not report leaves
    //     the editor's copy disagreeing with the audio; a host closes that loop by calling
    //     IEditController::setParamNormalized, and the standalone is the host here.
    //
    // Dropping the second kind is a footswitch that changes the sound while the panel says
    // otherwise — the bat switch stays where it was.
    //
    // Values are COALESCED per parameter id rather than queued, so the meters, which arrive every
    // block, cannot crowd out an echo that arrives once. An implementation claims slots on the
    // audio thread and never releases them, which is what keeps this allocation-free; a plug-in
    // publishing more distinct parameters than there are slots loses the excess rather than grows.
    static constexpr int kFeedbackSlots = 64;

    // Main thread: read slot `index`. False means the slot has never been used, and slots are
    // filled in order, so the first empty one is the end. `seq` changes every time the audio
    // thread writes, which is how a caller tells a repeat from a new value.
    virtual bool readFeedback(int index, Steinberg::Vst::ParamID &id, double &value,
                              uint32_t &seq) const = 0;

    // --- the measurement ---------------------------------------------------
    // Dropouts the audio system has reported since open(): a JACK xrun, a WASAPI glitch. This is
    // THE number the live gate is written against — whether the amp's two crossfading models
    // really do fit inside the audio callback at a given buffer size on a given machine is a
    // measurement and not something anyone can read out of the source. Zero at the smallest
    // supported buffer size is the bar.
    //
    // Safe to read from any thread. It only ever grows, and a backend that cannot obtain the
    // count says so once on stderr and leaves this reading zero rather than guessing.
    virtual uint32_t dropouts() const = 0;

    // One line describing the device that is open, for the rack's footer. Empty when none is.
    //
    // COMPOSED BY THE BACKEND, because only the backend knows what is worth saying, and the three
    // have almost nothing in common to say: JACK has a server whose rate and size it was given, ASIO
    // has a driver the user chose by name, and WASAPI has two endpoints, a share mode it may have
    // been refused, and possibly two clocks that drift. A caller assembling this from the interface's
    // getters could only print the intersection, which is a rate and a block size — exactly the part
    // a user does not need help identifying.
    //
    // Main thread. Returns by value rather than a pointer into backend state: some of the numbers in
    // it are read from atomics the audio thread writes.
    virtual std::string deviceSummary() const = 0;
};

} // namespace Rations
