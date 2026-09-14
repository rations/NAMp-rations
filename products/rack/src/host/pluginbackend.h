// PluginBackend — the one interface every hosted plug-in format implements.
//
// Provenance: the method list is re-derived from the MIT-licensed function set in the author's own
// Haiku plug-in host (its pluginhost_internal.h declares the same operations as a flat list of
// extern "C" functions per format, dispatched by an if/else chain on a format tag). It is NOT
// derived from that project's Linux sibling, which has no licence file, states GPLv2 intent, and
// whose VST2 backend includes a GPLv2 ABI header. Nothing from that tree may enter this include
// graph — see NOTICE, which records the licence of every dependency in it.
//
// THREADING. Three distinct contracts, and mixing them up is what makes plug-in hosts crash:
//
//   * process() runs on the audio thread. No allocation, no locks, no logging, no file I/O, no
//     destruction, no VST3 IEditController call.
//   * paramSetFromUi() ENQUEUES. It never touches the plug-in's live processing state. The parent
//     project's VST2 backend calls setParameter() straight from the UI thread with no ring at all,
//     so dragging a slider can block the audio thread inside a plug-in's own mutex. Making the
//     enqueue the only entry point is what makes that class of bug unrepresentable here.
//   * everything else runs on the owning thread — the chain builder worker for lifecycle and state,
//     the run loop for the editor. stateLoad() in particular requires that the instance is NOT in a
//     published chain, and ALSO that it has already been prepared: an LV2 plug-in has no instance
//     to restore into until then. ChainBuilder::loadNodeState is the one place that gets both ends
//     of that right, and is what a preset load goes through.
//
// The interface carries MIDI but no transport and no instrument support. MIDI is here because the
// pedals that ship beside this host are STOMPBOXES: each has its own MIDI-learn row and its whole
// reason to exist is being switched by a foot, and a rack that delivers a footswitch to the amp and
// not to the pedals in front of it is a pedalboard with half its cabling missing. It is delivered
// as AudioBlock::midi — undecoded, because the door differs per format (see RtMidiEvent) — and
// every hosted node is offered every message, exactly as the amp is. Filtering is the plug-in's
// own business: a learned binding is what says which message is for it, and that lives inside the
// plug-in where the user set it, not in a routing matrix here.
//
// Transport and instrument support stay out. This is still a guitar rack: nothing in it needs a bar
// line, and a node that wants to be played rather than driven is a different product.
//
// Wet/dry mix and per-node enable are not backend state either — they live in the chain snapshot,
// which is what lets a node at mix == 1.0 cost exactly nothing extra.

#pragma once

#include "pluginref.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NAMp::host
{

//------------------------------------------------------------------------
// Asking a backend to open its editor. `parentWindow` is the platform's own window id — an X11
// Window on Linux, an HWND on Windows — and `plugFrame` a Steinberg::IPlugFrame*, both passed
// opaquely so this header needs neither a windowing system nor the SDK. uintptr_t rather than void*
// because an X id is an integer and not a pointer, and it is the wider of the two spellings.
struct EditorOpenRequest {
    uintptr_t parentWindow = 0;
    void *plugFrame = nullptr;
    const char *windowTitle = nullptr;
};

//------------------------------------------------------------------------
// What came back. `childWindow` is the window the plug-in created inside our parent, which only an
// LV2 X11 UI does — the format is Linux-only, so this is always an X11 Window when it is set at
// all; `plugView` is the Steinberg::IPlugView* (VST3). Exactly one is meaningful, per `kind`.
struct EditorSurface {
    EditorKind kind = EditorKind::NoEditor;
    uintptr_t childWindow = 0;
    void *plugView = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    bool resizable = false;
};

//------------------------------------------------------------------------
class PluginBackend
{
public:
    virtual ~PluginBackend() = default;

    PluginBackend(const PluginBackend &) = delete;
    PluginBackend &operator=(const PluginBackend &) = delete;

    //--- identity ------------------------------------------------------
    // Immutable after construction, so safe to read from any thread.
    virtual PluginFormat format() const = 0;
    virtual const char *key() const = 0;
    virtual const char *displayName() const = 0;
    virtual const char *category() const = 0;

    //--- lifecycle (owning thread) -------------------------------------
    // prepare() sizes every internal buffer; after it returns true no allocation may happen on the
    // audio path. activate()/deactivate() bracket processing. reset() clears tails and is NOT
    // RT-safe.
    virtual bool prepare(const ProcessConfig &config) = 0;
    virtual void activate() = 0;
    virtual void deactivate() = 0;
    virtual void reset() = 0;

    virtual uint32_t latencySamples() const = 0;
    // False forces the chain compiler to give this node distinct input and output slots. It still
    // costs no copy — the chain ping-pongs between pre-allocated buses — but it costs a slot.
    virtual bool prefersInPlace() const = 0;
    virtual int32_t audioInCount() const = 0;
    virtual int32_t audioOutCount() const = 0;

    //--- real time -----------------------------------------------------
    virtual void process(const AudioBlock &block) noexcept = 0;

    //--- parameters (owning thread) ------------------------------------
    virtual uint32_t paramCount() const = 0;
    virtual bool paramInfo(uint32_t index, ParamInfo &out) const = 0;
    // The UI-side shadow value, never a read of live processing state.
    virtual double paramGet(uint32_t index) const = 0;
    // Enqueues only. See the threading note at the top of this file.
    virtual void paramSetFromUi(uint32_t index, double normalized) = 0;
    virtual bool paramDisplay(uint32_t index, double normalized, char *buf,
                              int32_t bufLen) const = 0;
    // Drain one RT-published change (a meter, or an edit the plug-in made itself). Returns false
    // when the queue is empty.
    virtual bool paramPollFromRt(uint32_t &index, double &normalized) = 0;

    // Push every pending UI edit into the plug-in NOW, on this thread, instead of waiting for the
    // audio thread to drain the ring.
    //
    // Without this a saved preset can be a lie. paramSetFromUi() only enqueues, so the plug-in's
    // own state does not carry a knob the user just moved until a block has been processed — and
    // with no JACK server running, or with the transport stopped, that block never comes. A rack
    // saved in that window records the plug-in's DEFAULTS while the interface shows the user's
    // settings. It is not a hypothetical: it is what the round-trip gate caught, with every
    // parameter of all four pedals coming back at its default.
    //
    // Pure rather than defaulted, because "this backend has nothing queued" is a claim each format
    // has to make for itself; a silently inherited no-op is exactly how the bug above got written.
    // Owning thread only, and only while the node is NOT in a published chain — it writes the
    // plug-in's live processing state directly.
    virtual void paramFlushToPlugin() = 0;

    //--- opaque state (owning thread; NOT while published) --------------
    // Where this node's state may keep files it refers to — an impulse response, a sample — for the
    // next stateSave()/stateLoad() only. Null or empty means "nowhere", which is what an in-memory
    // round trip uses and what every caller before presets existed did.
    //
    // Not pure, and deliberately so: only LV2 has a mechanism for this (state:mapPath, which lilv
    // implements for us once it is given a directory). VST3 state is one opaque blob with no
    // in-band way to say "and this file beside it", so a VST3 plug-in storing a path stores an
    // absolute one no matter what a host does. Making every backend override a call that only one
    // of them can honour would state the opposite.
    virtual void stateSetDirectory(const char * /*dir*/)
    {
    }
    virtual bool stateSave(std::vector<uint8_t> &out) const = 0;
    virtual bool stateLoad(const uint8_t *data, size_t len) = 0;

    //--- editor (owning thread) ----------------------------------------
    virtual EditorKind editorKind() const = 0;
    virtual bool editorOpen(const EditorOpenRequest &request, EditorSurface &out) = 0;
    // Called on the UI timer. Drives LV2's idleInterface; a no-op for VST3, which registers its own
    // timers through the run loop we hand it.
    virtual void editorIdle() = 0;
    // Latched resize request, read-and-clear. An LV2 UI may call its resize callback from a thread
    // we do not control, so the callback only stores the size and the timer applies it — no Xlib
    // call is ever made from inside a plug-in callback.
    virtual bool editorTakeResizeRequest(int32_t &w, int32_t &h) = 0;
    // Ask the editor what it will accept nearest to `w` x `h`, adjusting both in place. False means
    // the backend has no opinion and the size stands. This is what lets a window manager learn a
    // resizable editor's real limits, and what keeps a user's drag from handing the editor a
    // rectangle it will only refuse — which presents as a panel drawn into the wrong place rather
    // than as an error.
    virtual bool editorCheckSize(int32_t &w, int32_t &h) const = 0;
    virtual void editorSetSize(int32_t w, int32_t h) = 0;
    virtual void editorShow() = 0; // showInterface kind only
    virtual void editorHide() = 0;
    virtual void editorClose() = 0;

    //--- host extensions ------------------------------------------------
    // INampFileLoader, discovered by queryInterface. A VST3 parameter is a normalized float, so a
    // plug-in whose real state is a FILE — a capture, an impulse response, a sample — has no way to
    // expose it as one, and a rack that only knows parameters cannot drive it at all. This is the
    // escape hatch for that class of plug-in, and it is deliberately not the amp's own: the amp is
    // linked in rather than hosted, and its four banks travel over the SDK's IConnectionPoint
    // channel instead. See inampfileloader.h for whose interface this is and why it is here.
    virtual bool hasFileLoader() const = 0;
    virtual bool fileGet(int32_t which, char *buf, int32_t bufLen) const = 0;
    virtual bool fileSet(int32_t which, const char *path) = 0;

    //--- diagnostics ----------------------------------------------------
    // Worst-case microseconds spent in process() since the last call, read-and-reset. Only measured
    // when diagnostics are armed; see diagnostics.h.
    virtual int64_t diagTakeMaxMicros() = 0;
    // Allocations this plug-in made on the audio thread, as far as we can observe them. We cannot
    // prevent a badly behaved plug-in from allocating; we can name it.
    virtual uint64_t diagRtAllocCount() const = 0;

protected:
    PluginBackend() = default;
};

} // namespace NAMp::host
