// Does RationsEditorView's hook set still fit MacPlugView? Every override
// rationsview.h declares against NativePlugView, with its exact signature, plus
// every base member the editor calls. A signature that drifted apart from the
// other two platforms fails to compile here.
#include "platform/macplugview.h"

namespace Steinberg
{
class SeamProbe : public MacPlugView
{
public:
    SeamProbe(Vst::EditController *c) : MacPlugView(c, nullptr)
    {
    }

protected:
    void onAttached() SMTG_OVERRIDE
    {
    }
    void onRemoved() SMTG_OVERRIDE
    {
    }
    void onDraw(cairo_t *cr) SMTG_OVERRIDE
    {
        (void)cr;
    }
    void onMouseDown(int x, int y, int button) SMTG_OVERRIDE
    {
        (void)x, (void)y, (void)button;
    }
    void onMouseUp(int x, int y, int button) SMTG_OVERRIDE
    {
        (void)x, (void)y, (void)button;
    }
    void onMouseMove(int x, int y) SMTG_OVERRIDE
    {
        (void)x, (void)y;
    }
    void onMouseWheel(int x, int y, int delta) SMTG_OVERRIDE
    {
        (void)x, (void)y, (void)delta;
    }
    void onTick() SMTG_OVERRIDE
    {
    }
    bool isResizable() const SMTG_OVERRIDE
    {
        return true;
    }
    void constrainSize(int &w, int &h) const SMTG_OVERRIDE
    {
        (void)w, (void)h;
    }
    void onResized(int w, int h) SMTG_OVERRIDE
    {
        (void)w, (void)h;
    }
    bool onKeyDownNative(char16 key, int16 keyCode, int16 modifiers) SMTG_OVERRIDE
    {
        (void)key, (void)keyCode, (void)modifiers;
        return false;
    }

public:
    // The base members the editor calls, in the ways it calls them.
    void exercise()
    {
        invalidate();
        (void)isWindowOpen();
        (void)requestResize(640, 460);
        setKeyboardFocus(true);
        setKeyboardFocus(false);
    }
};
} // namespace Steinberg
