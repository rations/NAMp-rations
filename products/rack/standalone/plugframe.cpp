// PlugFrame implementation. See plugframe.h.

#include "plugframe.h"

using namespace Steinberg;

namespace Rations
{

//------------------------------------------------------------------------
tresult PLUGIN_API PlugFrame::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;

#if !SMTG_OS_WINDOWS
    // The run loop is the shared one, and it is handed back through this frame because that is the
    // only route a plug-in has to it: IPlugView::setFrame is the single host pointer a view holds.
    //
    // OFFERED ON THIS PLATFORM ONLY, because it is the only one where a plug-in has no event loop
    // of its own and the host must lend it one. On Windows the system runs that loop, our own is a
    // plain message pump with nothing to register into, and a Windows plug-in uses its own timers —
    // so there is deliberately nothing to hand back. Answering with a loop whose
    // registerEventHandler half cannot work (it takes a file descriptor to select() on, and there
    // are none) would be worse than answering with nothing: a cross-platform plug-in that queried
    // defensively would have its timer accepted and its event handler silently dropped. See
    // eventloop.h.
    if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid))
        return mLoop.queryInterface(iid, obj);
#endif

    if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IPlugFrame *>(this);
        addRef();
        return kResultOk;
    }

    *obj = nullptr;
    return kNoInterface;
}

//------------------------------------------------------------------------
tresult PLUGIN_API PlugFrame::resizeView(IPlugView *view, ViewRect *newSize)
{
    if (!view || !newSize)
        return kInvalidArgument;
    if (!mResize)
        return kResultFalse;
    return mResize(view, newSize) ? kResultTrue : kResultFalse;
}

} // namespace Rations
