// RackWindow implementation. See rackwindow.h for the painting and threading discipline.

#include "rackwindow.h"

#include "host/diagnostics.h"

#include "rack/rackgeometry.h"

#include <cstdio>

namespace Rations
{

//------------------------------------------------------------------------
RackWindow::RackWindow(EventLoop &loop, NAMp::host::ChainBuilder &builder)
    : mBuilder(builder), mWindow(loop)
{
    mModel.setBuilder(&builder);
    mView.setModel(&mModel);
    // The one place $NAMP_DIAG is consulted for the strip. See RackModel::setDiagArmed.
    mModel.setDiagArmed(NAMp::host::diagArmed());
}

//------------------------------------------------------------------------
RackWindow::~RackWindow()
{
    destroy();
}

//------------------------------------------------------------------------
void RackWindow::loadFonts(const std::string &resourceDir)
{
    // An empty directory is the normal case in a single-file build and is NOT a failure: FontStack
    // falls back to the faces linked into this binary (src/gfx/resourcestore.h). Only a face that
    // ends up as a generic system font is worth a word.
    if (!mFonts.load(resourceDir))
        fprintf(stderr, "namp-standalone: the rack fell back to generic fonts\n");
}

//------------------------------------------------------------------------
bool RackWindow::create(NativeHandle parent, int x, int y, int w, int h)
{
    if (!parent || w <= 0 || h <= 0)
        return false;
    if (mWindow.isOpen())
        return true;

    // Input::Full: unlike the top-level, this window wants input — the rack is the one part of the
    // interface the standalone draws and handles itself. Keys among them, for the preset name field
    // and nothing else; see syncKeyboardFocus for why asking for them claims nothing.
    if (!mWindow.createChild(parent, x, y, w, h, NativeWindow::Input::Full))
        return false;

    mWindow.setEventCallback([this](const WindowEvent &event) { onEvent(event); });

    mWidth = w;
    mHeight = h;
    if (!mWindow.createSurfaces(w, h)) {
        fprintf(stderr, "namp-standalone: cannot create the rack's drawing surface\n");
        destroy();
        return false;
    }

    mWindow.show();
    mDirty = true;
    return true;
}

//------------------------------------------------------------------------
void RackWindow::destroy()
{
    // The window releases the keyboard focus and stops dispatching before it goes; both are inside
    // destroy() and in that order.
    mWindow.destroy();
}

//------------------------------------------------------------------------
void RackWindow::setGeometry(int x, int y, int w, int h, double scale)
{
    if (!mWindow.isOpen() || w <= 0 || h <= 0)
        return;

    mScale = scale > 0.0 ? scale : 1.0;
    mWindow.moveResize(x, y, w, h);

    if (w != mWidth || h != mHeight) {
        mWidth = w;
        mHeight = h;
        mWindow.resizeSurfaces(w, h);
    }
    mDirty = true;
}

//------------------------------------------------------------------------
void RackWindow::setEditorOpen(uint64_t nodeId, bool open)
{
    mModel.setEditorOpen(nodeId, open);
    mDirty = true;
}

//------------------------------------------------------------------------
uint64_t RackWindow::nodeIdAt(NAMp::host::ChainSection section, int index) const
{
    NAMp::host::ChainNodeInfo info;
    return mBuilder.nodeInfo(section, index, info) ? info.id : 0;
}

//------------------------------------------------------------------------
void RackWindow::onTimer()
{
    // A requested scan runs here, on the tick, and not where the click was handled — see
    // requestScan(). It paints its own progress while it runs, which is legal from here.
    if (mScanPending) {
        mScanPending = false;
        if (mScanPlugins)
            mScanPlugins();
    }
    if (mDirty)
        redraw();
}

//------------------------------------------------------------------------
void RackWindow::redraw()
{
    cairo_surface_t *surface = mWindow.drawingSurface();
    if (!surface)
        return;
    mDirty = false;

    // Compose offscreen...
    cairo_t *cr = cairo_create(surface);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
        // The single scale the whole design rests on. Everything below is in logical units.
        cairo_scale(cr, mScale, mScale);
        Canvas canvas(cr, &mFonts, NAMp::rackgeo::kRackW, NAMp::rackgeo::kRackH);
        mView.draw(canvas);
    }
    cairo_destroy(cr);

    // ...then present in one operation, so no partially drawn frame is ever visible.
    mWindow.present();
}

//------------------------------------------------------------------------
// Progress for a running scan.
//
// A scan is synchronous: it walks every bundle and spawns a helper process for each one that
// changed, and the run loop does not tick again until it returns. Painting on the next tick would
// therefore mean painting when it is over, which is exactly when nobody needs to be told it is
// running — so this composes and presents immediately, the same way onTimer does.
//
// That is allowed because of where it is called from: requestScan() defers the whole scan to the
// timer tick, so this never runs inside a window's event callback and nothing here can recurse into
// drawing at all. No window event is dispatched while it runs.
void RackWindow::showScanProgress(int index, int total, const std::string &current)
{
    NAMp::rack::ScanState &scan = mModel.scan();
    scan.running = true;
    scan.index = index;
    scan.total = total;
    scan.current = current;
    mDirty = true;
    redraw();
}

void RackWindow::endScanProgress()
{
    mModel.scan() = NAMp::rack::ScanState();
    mDirty = true;
}

//------------------------------------------------------------------------
void RackWindow::syncKeyboardFocus()
{
    mWindow.setKeyboardFocus(mView.wantsKeyboard());
}

//------------------------------------------------------------------------
void RackWindow::onEvent(const WindowEvent &event)
{
    // Nothing here paints. Every branch either updates state or asks for a repaint on the next
    // tick — see the discipline note in the header.
    //
    // COORDINATES ARE DIVIDED BY THE SCALE HERE AND NOWHERE ELSE. A window reports physical pixels;
    // the strip is laid out in the same logical units as the editor above it and drawn at the same
    // factor, so this is the one boundary where a pixel becomes a logical unit. That is why nothing
    // in the rack code below knows what a pixel is.
    const double scale = mScale > 0.0 ? mScale : 1.0;
    const float x = static_cast<float>(event.x / scale);
    const float y = static_cast<float>(event.y / scale);

    switch (event.kind) {
        case WindowEvent::Kind::Redraw:
            mDirty = true;
            return;

        case WindowEvent::Kind::Resize:
            if (event.width != mWidth || event.height != mHeight) {
                mWidth = event.width;
                mHeight = event.height;
                mWindow.resizeSurfaces(mWidth, mHeight);
            }
            mDirty = true;
            return;

        case WindowEvent::Kind::Close:
            // A child window is never asked to close on its own; the top-level is what carries the
            // close request, and the standalone handles it there.
            return;

        case WindowEvent::Kind::Key: {
            const NAMp::rack::RackAction action =
                mView.key(event.character, event.virtualKey, event.modifiers);
            if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                mDirty = true;
            // After, not before: the key that closed the field is what releases the keyboard.
            syncKeyboardFocus();
            return;
        }

        case WindowEvent::Kind::FocusLost:
            // The window has already dropped its own claim; there is nothing for the rack to do but
            // leave the field as it is. Whatever is typed next goes wherever the focus went.
            return;

        case WindowEvent::Kind::MouseLeave:
            // Clear the hover, or a control keeps its highlight after the pointer has gone.
            mModel.setHover(NAMp::rack::HitTarget());
            mDirty = true;
            return;

        case WindowEvent::Kind::MouseMove: {
            // Motion ACTS, it does not only repaint. A drag on the wet/dry slider reports its new
            // value from here and nowhere else, so a handler that merely set the dirty flag drew
            // the drag and threw the value away.
            const NAMp::rack::RackAction action = mView.mouseMove(x, y);
            if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                mDirty = true;
            return;
        }

        case WindowEvent::Kind::Wheel: {
            const NAMp::rack::RackAction action = mView.wheel(x, y, event.delta);
            if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                mDirty = true;
            return;
        }

        case WindowEvent::Kind::MouseDown: {
            if (event.button != kButtonLeft && event.button != kButtonRight)
                return;
            const NAMp::rack::RackAction action = mView.mouseDown(x, y, event.button);
            if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                mDirty = true;
            // A click is what opens the name field and what dismisses it, so this is the other end
            // of the keyboard contract: taken here when a field appeared, handed back here when one
            // went away.
            syncKeyboardFocus();
            return;
        }

        case WindowEvent::Kind::MouseUp: {
            if (event.button != kButtonLeft && event.button != kButtonRight)
                return;
            const NAMp::rack::RackAction action = mView.mouseUp(x, y, event.button);
            if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                mDirty = true;
            return;
        }
    }
}

//------------------------------------------------------------------------
bool RackWindow::applyAction(const NAMp::rack::RackAction &action)
{
    using Kind = NAMp::rack::RackAction::Kind;

    switch (action.kind) {
        case Kind::NoAction:
        case Kind::Redraw:
            return false;

        case Kind::SetViewMode:
            mModel.setViewMode(action.mode);
            mModel.setListScroll(0);
            return false;

        case Kind::ToggleEditor: {
            if (mEditorToggle)
                mEditorToggle(action.section, action.index);
            return false;
        }

        case Kind::Route: {
            mBuilder.setEnabled(action.section, action.index, action.flag);
            if (action.toIndex >= 0)
                mBuilder.move(action.section, action.index, action.toIndex);
            break;
        }

        case Kind::SetMix:
            mBuilder.setMix(action.section, action.index, action.value);
            break;

        case Kind::Remove: {
            // Any window showing this plug-in's own editor has to go FIRST: it holds a reference to
            // the backend that is about to be buried, and a later collect() would free it under the
            // window's feet.
            if (mEditorClose)
                mEditorClose(nodeIdAt(action.section, action.index));
            // The instance itself is not destroyed here. It goes to the builder's graveyard and is
            // freed by collect() once a snapshot newer than any that could name it is live on the
            // audio thread — which is the whole reason a chain can be edited while it is playing.
            mBuilder.remove(action.section, action.index);
            break;
        }

        case Kind::Add: {
            // Loading a plug-in opens a shared object and runs its static initialisers, which
            // blocks this thread — the run loop — for as long as that takes.
            //
            //   Measured in P7 and deliberately left here: 12-18 ms for a native plug-in, ~860 ms
            //   for a Windows one over Wine. See the note at the top of host/chainbuilder.h for why
            //   a worker is not the free fix it looks like. It does NOT interrupt audio either way,
            //   because the chain the audio thread is running is not touched until publish() below.
            std::string error;
            if (mBuilder.add(action.section, action.ref, error) < 0) {
                fprintf(stderr, "namp-standalone: cannot add that plug-in: %s\n", error.c_str());
                return false;
            }
            break;
        }

        case Kind::LoadPreset: {
            // Every open hosted editor first, for the same reason Remove does it: a preset load
            // removes every node, and a window holding a backend the builder is about to bury is a
            // use-after-free waiting for the next collect().
            if (mEditorClose) {
                for (const NAMp::rack::RackNode &node : mModel.nodes())
                    mEditorClose(node.id);
            }
            if (!mLoadPreset)
                return false;
            if (!mLoadPreset(action.text))
                return false;
            mPresetName = action.text;
            mModel.setPresetName(mPresetName);
            // applyRack has already published; falling through to publish() again would be
            // harmless but would spend a snapshot for nothing.
            mModel.refresh();
            mDirty = true;
            if (mChainChanged)
                mChainChanged();
            return true;
        }

        case Kind::SavePreset: {
            const std::string name = action.text.empty() ? mPresetName : action.text;
            if (name.empty())
                return false;
            if (mSavePreset)
                mSavePreset(name);
            // Saving under a new name makes that rack the one being worked on, which is what Save
            // As means everywhere else. Without it the list would show the new preset while the
            // save row still offered the OLD name, so the next save would quietly go somewhere the
            // user had just moved away from.
            mPresetName = name;
            mModel.setPresetName(mPresetName);
            mDirty = true;
            return false;
        }

        case Kind::DeletePreset: {
            if (action.text.empty())
                return false;
            if (mDeletePreset)
                mDeletePreset(action.text);
            // The name is deliberately NOT cleared when the rack that was deleted is the one
            // loaded. What is playing is still that rack; the file is what went. Leaving the name
            // means the save row still offers it, so a deletion made by mistake is undone by
            // pressing save — which is the only undo this has.
            mDirty = true;
            return false;
        }

        // Discovery. None of the three touches the chain, so none of them falls through to
        // publish(): a scan changes what COULD be added, not what is playing.
        case Kind::ScanPlugins: {
            requestScan();
            return false;
        }

        case Kind::AddSearchPath: {
            if (mAddSearchPath)
                mAddSearchPath(action.text);
            mDirty = true;
            return false;
        }

        case Kind::RemoveSearchPath: {
            if (mRemoveSearchPath)
                mRemoveSearchPath(action.text);
            mDirty = true;
            return false;
        }

        case Kind::SelectAudioDevice: {
            // The standalone closes and reopens the device, and pushes the list back in with the
            // current row moved — so the overlay the user is still looking at shows what actually
            // happened, which may not be what was asked for.
            if (mSelectAudioDevice)
                mSelectAudioDevice(action.text, action.index);
            mDirty = true;
            return false;
        }
    }

    mBuilder.publish();
    // Free whatever the audio thread has handed back by now. Cheap, and doing it on every edit
    // keeps the graveyard from growing across a long editing session.
    mBuilder.collect();
    // The snapshot the strip is drawn from is now stale by definition, so re-read it here rather
    // than relying on whoever called this to remember. Every caller wants the picture to follow the
    // edit, and a caller that is not the pointer — the stress driver, and later the preset
    // loader — has no X event to piggyback the refresh on.
    mModel.refresh();
    mDirty = true;
    if (mChainChanged)
        mChainChanged();
    return true;
}

} // namespace Rations
