// HostApp — the IHostApplication a hosted VST3 plug-in is given, plus an RT-allocation counter.
//
// Every VST3 plug-in gets a host context; without one, allocateMessage() returns null and every
// controller-to-processor message silently vanishes. The SDK ships a ready implementation
// (public.sdk/source/vst/hosting/hostclasses.h) and this subclasses it for exactly one reason: to
// notice when a plug-in asks the host to build an object while the audio thread is inside
// process().
//
// IHostApplication::createInstance() news a HostMessage or a HostAttributeList. That is the classic
// route by which a well-known plug-in allocates on the real-time thread — typically a metering or
// notification message sent once per block. We cannot stop it: the plug-in is entitled to call us,
// and refusing would break it. What we can do is COUNT it and name the offender, so an xrun is
// attributable instead of mysterious. The author's Haiku host does the same and makes a zero count
// an explicit acceptance gate.
//
// The RT flag is thread-local and is raised only around the chain's process() call, so the counter
// measures calls genuinely made from the audio thread and ignores the many legitimate ones made
// during load.
//
// Ownership: exactly one HostApp exists per process. It is published through the SDK's
// PluginContextFactory before any module is loaded and retracted after the last one is released.

#pragma once

#include "public.sdk/source/vst/hosting/hostclasses.h"

#include <atomic>
#include <cstdint>

namespace NAMp::host
{

//------------------------------------------------------------------------
// Raise for the duration of a call that must not allocate. Nested raises are counted, so a chain
// that processes a node inside a node still reports correctly.
class RtScope
{
public:
    RtScope();
    ~RtScope();

    RtScope(const RtScope &) = delete;
    RtScope &operator=(const RtScope &) = delete;
};

// True when the calling thread is inside an RtScope.
bool rtScopeActive();

//------------------------------------------------------------------------
class HostApp : public Steinberg::Vst::HostApplication
{
public:
    HostApp() = default;

    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID cid, Steinberg::TUID iid,
                                                 void **obj) SMTG_OVERRIDE;

    // Total createInstance calls made from inside an RtScope, across every hosted plug-in. Per-node
    // attribution is done by reading this before and after each node's process() when diagnostics
    // are armed.
    uint64_t rtAllocCount() const
    {
        return mRtAllocCount.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> mRtAllocCount{0};
};

//------------------------------------------------------------------------
// The process-wide instance. Null until installHostApp() is called.
HostApp *hostApp();

// Creates the HostApp and publishes it as the SDK plug-in context. Must be called before any
// module is loaded, and only from the main thread.
//
// Returns false if a plug-in context is ALREADY set. That is not a failure to route around: the
// SDK's PluginContextFactory is a process-wide singleton, so inside a DAW that has already set its
// own context we must not overwrite it. In the standalone the slot is ours and this always
// succeeds; the in-plug-in phase has to solve it properly.
bool installHostApp();

// Retracts the plug-in context and drops our reference to the HostApp. The object itself lives on
// until the last plug-in holding it releases it, so this is safe to call before the modules are
// unloaded — and must be, because a plug-in's terminate() releases the context it was given.
void uninstallHostApp();

} // namespace NAMp::host
