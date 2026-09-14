// PlugFrame — the host object one VST3 editor talks to.
//
// A view is given a frame in IPlugView::setFrame, and everything it needs from the host afterwards
// it gets through that one pointer: the run loop by queryInterface, and a resize request by
// resizeView. Both halves have to be right or the editor either never ticks or draws into the wrong
// rectangle.
//
// ONE PER VIEW, not one per process. resizeView() must resize the window THIS view is embedded in,
// and the frame is the only context the callback gets — a shared frame would have to guess which of
// several open editors was asking. The run loop it hands back, by contrast, is deliberately the one
// shared EventLoop: there is a single X connection and a single select() loop for the whole
// process.
//
// The resize contract is the SDK's, from pluginterfaces/gui/iplugview.h ("Sizing of a view"): the
// host resizes the platform window and then calls IPlugView::onSize() in the SAME callstack. The
// callback owns both halves, because only the caller knows which window and whether the size is
// acceptable. Refusing when there is no callback is deliberate: reporting success without resizing
// would leave the window and the view disagreeing, which shows up as a panel drawn into the wrong
// rectangle rather than as an error.

#pragma once

#include "eventloop.h"

#include "pluginterfaces/gui/iplugview.h"

#include <functional>
#include <utility>

namespace Rations
{

//------------------------------------------------------------------------
class PlugFrame : public Steinberg::IPlugFrame
{
public:
    // Return true if the request was honoured; the callback owns both halves of the contract —
    // resize the window, then call view->onSize().
    using ResizeCallback = std::function<bool(Steinberg::IPlugView *, Steinberg::ViewRect *)>;

    explicit PlugFrame(EventLoop &loop) : mLoop(loop)
    {
    }

    void setResizeCallback(ResizeCallback callback)
    {
        mResize = std::move(callback);
    }

    //---from IPlugFrame--------------
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view,
                                             Steinberg::ViewRect *newSize) SMTG_OVERRIDE;

    //---from FUnknown----------------
    // Lifetime is the owning window's, not the reference count's; see the note in eventloop.h.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void **obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    EventLoop &mLoop;
    ResizeCallback mResize;
};

} // namespace Rations
