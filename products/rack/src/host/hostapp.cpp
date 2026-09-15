// HostApp implementation. See hostapp.h for the threading and ownership contract.

#include "hostapp.h"

#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/base/funknownimpl.h"

#include <cstring>

using namespace Steinberg;

namespace NAMp::host
{

namespace
{

// Depth rather than a bool so a nested RtScope cannot clear the flag early.
thread_local int gRtDepth = 0;

HostApp *gHostApp = nullptr;

} // namespace

//------------------------------------------------------------------------
RtScope::RtScope()
{
    ++gRtDepth;
}

RtScope::~RtScope()
{
    --gRtDepth;
}

bool rtScopeActive()
{
    return gRtDepth > 0;
}

//------------------------------------------------------------------------
tresult PLUGIN_API HostApp::getName(Vst::String128 name)
{
    // UTF-16, as String128 requires. The SDK's own implementation writes "My VST3 HostApplication";
    // a plug-in occasionally special-cases on this string, so it is worth being honest about who we
    // are rather than impersonating a DAW.
    static const char16_t kName[] = u"NAMp-Rack";
    const size_t count = sizeof(kName) / sizeof(kName[0]); // includes the terminator
    for (size_t i = 0; i < count && i < 128; ++i)
        name[i] = static_cast<Vst::TChar>(kName[i]);
    name[127] = 0;
    return kResultTrue;
}

//------------------------------------------------------------------------
tresult PLUGIN_API HostApp::createInstance(TUID cid, TUID iid, void **obj)
{
    // Count first: the base class allocates, so counting afterwards would miss a throwing path.
    if (rtScopeActive())
        mRtAllocCount.fetch_add(1, std::memory_order_relaxed);

    return Vst::HostApplication::createInstance(cid, iid, obj);
}

//------------------------------------------------------------------------
HostApp *hostApp()
{
    return gHostApp;
}

//------------------------------------------------------------------------
bool installHostApp()
{
    if (gHostApp)
        return true; // already ours

    auto &factory = Vst::PluginContextFactory::instance();
    if (factory.getPluginContext())
        return false; // someone else's context — see the header

    // Constructed with a reference count of 1, which is ours; every plug-in that keeps the context
    // adds its own. See uninstallHostApp() for why that matters.
    gHostApp = new HostApp;
    // HostApplication derives from IHostApplication derives from FUnknown, so the implicit
    // conversion is the whole cast — the same way the standalone publishes its context today.
    factory.setPluginContext(static_cast<FUnknown *>(gHostApp));
    return true;
}

//------------------------------------------------------------------------
void uninstallHostApp()
{
    if (!gHostApp)
        return;

    auto &factory = Vst::PluginContextFactory::instance();
    if (factory.getPluginContext() == static_cast<FUnknown *>(gHostApp))
        factory.setPluginContext(nullptr);

    // release(), NOT delete. HostApplication uses DECLARE_FUNKNOWN_METHODS, so it is genuinely
    // reference counted, and every plug-in initialised with this context holds a reference until
    // its own terminate() runs — ComponentBase::terminate() is what drops it. Deleting here frees
    // the object out from under a controller that is torn down later, and the crash lands inside
    // the plug-in with nothing in the backtrace to say why. Releasing lets the last owner destroy
    // it, whichever of us that turns out to be.
    gHostApp->release();
    gHostApp = nullptr;
}

} // namespace NAMp::host
