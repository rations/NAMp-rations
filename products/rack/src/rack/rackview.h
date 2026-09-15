// RackView — drawing and hit-testing for the rack strip.
//
// Stateless with respect to the chain: everything it draws comes from RackModel, and everything a
// click means comes back as a RackAction for the owner to apply. It knows nothing about X11, JACK,
// the VST3 SDK or plug-in instances, which is what lets tools/rackrender.cpp drive the whole
// interface offline with a chain that has no plug-ins in it at all.
//
// LAYOUT IS COMPUTED ONCE AND SHARED. Every row rect, card rect and cable midpoint comes out of
// layoutList()/layoutNodes(), and both draw() and hitTest() call them. A UI whose painting and
// whose hit-testing each work the geometry out for themselves is a UI where a button eventually
// stops being where it looks like it is; deriving both from one pass makes that unrepresentable.
//
// NO ART. Every glyph here — the in-circuit dot, the reorder arrows, the gear, the cross, the
// cables, the ports — is drawn from Canvas primitives rather than loaded from a file. The rack is
// host chrome that has to work when a resource directory is missing, and the alternative is another
// asset load with another degradation path to get right for no visual gain at this size.
//
// Coordinates arriving here are ALREADY in logical units and already relative to the rack's own
// origin. The caller divides by the window scale and subtracts the editor's height; nothing in this
// file knows what a pixel is.

#pragma once

#include "rackmodel.h"

#include "filebrowser.h"
#include "gfx/canvas.h"

// For the three values a key arrives as. The rack takes keys the same way the editor does and in
// the same currency, so a handler cannot tell which window it is serving.
#include "pluginterfaces/base/keycodes.h"

namespace NAMp::rack
{

// The drawing primitives and the folder chooser belong to the amp this host is built around, and
// they are named unqualified all through this file and its implementation. Pulled into the rack's
// own namespace rather than qualified at every use: it keeps this file readable as the file it was
// ported from, and the three names it imports are this project's own, into a namespace that is also
// this project's.
using Rations::Canvas;
using Rations::FileBrowser;
using Rations::Rect;

//------------------------------------------------------------------------
class RackView
{
public:
    // Not owned; must outlive this.
    void setModel(RackModel *model)
    {
        mModel = model;
    }

    // Repaints from the model as it stands. Calls RackModel::refresh() itself, so a caller that has
    // just applied an action does not have to remember to.
    void draw(Canvas &c);

    //--- input ----------------------------------------------------------
    // button: 1 = left, 3 = right, matching X11. Every one of these returns the action the user
    // asked for, or Kind::NoAction when nothing happened and Kind::Redraw when only the picture
    // changed.
    RackAction mouseMove(float x, float y);
    RackAction mouseDown(float x, float y, int button);
    RackAction mouseUp(float x, float y, int button);
    RackAction wheel(float x, float y, int delta);

    // One key, in the same three values IPlugView::onKeyDown is given: an ASCII character, a
    // VirtualKeyCodes value and a KeyModifier mask. The window decodes the platform event; nothing
    // below this line knows what X11 is, which is the same seam the mouse already goes through.
    //
    // Returns an action, so Return can be the save. RackAction::Kind::NoAction means the key was
    // NOT consumed and the window must let it through — a wrong claim here would swallow keys that
    // belong to whatever else is listening.
    RackAction key(Steinberg::char16 ch, Steinberg::int16 keyCode, Steinberg::int16 modifiers);

    // Whether a text field is open, which is the whole of what the window needs to decide about the
    // keyboard: it holds the focus while this is true and at no other time.
    bool wantsKeyboard() const;

    HitTarget hitTest(float x, float y) const;

    // The folder chooser, open only while the user is adding a search path. It is the plug-in's own
    // FileBrowser rather than anything native (no GTK, no Qt, no portal — the same reason the amp
    // picks its capture folder that way), drawn inside the picker's box.
    bool browserOpen() const
    {
        return mBrowser.isOpen();
    }
    void openFolderBrowser(const std::string &startPath);

private:
    void drawHeader(Canvas &c);
    void drawList(Canvas &c);
    void drawNodes(Canvas &c);
    void drawPicker(Canvas &c);
    void drawFooter(Canvas &c);
    void drawScanProgress(Canvas &c);

    RackAction pickerMouseDown(float x, float y, int button);

    // Shut the overlay and cancel any open name field. See the definition for why cancel and not
    // commit.
    void closePicker();

    // Close the preset name field, KEEPING what was typed, and return the action that saves under
    // it — or a redraw when the trimmed name is empty and there is nothing to write. Reached only
    // from Return.
    RackAction commitPresetName();
    // Rows the overlay is listing right now, whichever of its three lists is up.
    int pickerRowCount() const;
    // Where this node's wet/dry track is in whichever view is up, so a drag can keep following it
    // after the pointer has left the row or the card. False when the node is not on screen — the
    // shelf draws no slider, and a node can be removed mid-drag.
    bool mixTrackFor(uint64_t nodeId, Rect &track) const;

    RackModel *mModel = nullptr;
    FileBrowser mBrowser;
    // Set on mouse-down, consumed on mouse-up, so a press that slides off its control does not fire
    // it — the behaviour every other button in this project already has.
    HitTarget mPressed;
};

} // namespace NAMp::rack
