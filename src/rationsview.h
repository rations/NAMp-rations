// Rations native editor (IPlugView).
//
// RationsEditorView is the IPlugView the controller returns from createView(). It derives from
// NativePlugView — X11PlugView or Win32PlugView, chosen by platform/plugview.h — which owns the
// native child window, the event and timer plumbing, the double buffer and the resize plumbing;
// everything below is the panel itself, painted through Canvas. Nothing in this file or
// rationsview.cpp knows which window system is underneath.
//
// SCALE AND PAGES. The editor is host-resizable and the whole layout lives in logical units in
// geometry.h. compose() applies exactly one cairo_scale(s, s), so every draw call, font size and
// blit scales for free, and every incoming mouse coordinate is divided by s before it is
// hit-tested. No scale factor is ever baked into a geometry constant.
//
// What is new here relative to the parent plug-in is that s is applied to a page's OWN canvas
// size rather than to one global one. Four pages, four shapes: the head is 2.81:1 and nothing
// else here is. On a page change the editor keeps its scale, applies it to the incoming page's
// base size and asks the host to resize the window (NativePlugView::requestResize, which is
// IPlugFrame::resizeView). The window changes shape; it never changes apparent size.
//
// A host that supplies no IPlugFrame cannot be asked, and there the page is letterboxed inside
// whatever window there already is. Both cases go through ONE piece of arithmetic — fit the page
// inside the device rect and centre it — so the fallback is not a second code path that only runs
// in hosts nobody tests against; it is the same path with a different remainder.
//
// SCROLLING. One page — settings, at 928 units — can be taller than its window, and that is the
// case the letterbox arithmetic above could not serve: fitting it would have meant a scale below
// the floor, and clamping to the floor cut the ends off the page. So that page is width-locked
// and free in height, and the band that does not fit is reached by scrolling (geo::pageScrolls).
//
// The scroll is ONE translate inside the page transform, not a second coordinate system. Content
// is drawn with it and clipped to the viewport; chrome — the back button, the scrollbar and the
// browser overlay — is drawn after it is undone; and hit-testing adds it back exactly where the
// drawing subtracted it, in contentY(). Every page that does not scroll runs the same code with
// mScrollY at 0.
//
// The page is editor-local state and is deliberately neither a parameter nor persisted: a host
// recalling a preset must not also recall which panel the user was looking at.
//
// The controller pushes value changes in through ParamChanged() and bank state through
// ModelCapsChanged(); user edits go out through EditController::beginEdit/performEdit/endEdit,
// which reach the host's IComponentHandler. All of it runs on the host's run-loop thread, the
// same thread the controller is called on, so none of it needs locking.

#pragma once

#include "filebrowser.h"
#include "geometry.h"
#include "gfx/canvas.h"
#include "gfx/fontstack.h"
#include "gfx/image.h"
#include "gfx/svg.h"
#include "platform/plugview.h"

#include <string>
#include <vector>

namespace Rations
{

class RationsController;

//------------------------------------------------------------------------
class RationsEditorView : public Steinberg::NativePlugView
{
public:
    explicit RationsEditorView(RationsController *controller);
    ~RationsEditorView() override;

    // Called by the controller whenever a parameter value changes (automation, generic UI, state
    // load, metering, bank progress).
    void ParamChanged(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);

    // Called by the controller when the processor reports what the four banks hold. `names[c]`
    // are channel c's captures in gain order — the order that channel's dial sweeps — and may be
    // empty while the workers are still building.
    void ModelCapsChanged(const int entryCounts[kChannelCount],
                          const std::vector<std::string> names[kChannelCount]);

    // Called by the controller when an IR path changes. The paths are not parameters, so nothing
    // else would tell the loader rows to repaint.
    void FilesChanged()
    {
        invalidate();
    }

protected:
    //---from NativePlugView----------
    void onAttached() SMTG_OVERRIDE;
    void onRemoved() SMTG_OVERRIDE;
    void onDraw(cairo_t *cr) SMTG_OVERRIDE;
    void onMouseDown(int x, int y, int button) SMTG_OVERRIDE;
    void onMouseUp(int x, int y, int button) SMTG_OVERRIDE;
    void onMouseMove(int x, int y) SMTG_OVERRIDE;
    void onMouseWheel(int x, int y, int delta) SMTG_OVERRIDE;
    void onTick() SMTG_OVERRIDE;

    bool isResizable() const SMTG_OVERRIDE
    {
        return true;
    }
    // Reads its base size from the CURRENT page, which is why mPage must already be the incoming
    // page before requestResize() is called — the host calls back into onSize(), and so into
    // here, inside that call.
    void constrainSize(int &w, int &h) const SMTG_OVERRIDE;
    void onResized(int w, int h) SMTG_OVERRIDE;

private:
    //--- pages -----------------------------------------------------------
    void setPage(geo::Page page);
    // Fit the current page inside the device rect and centre it: sets mScale, mOffX, mOffY. One
    // rule for both the resized case (remainder zero) and the un-resizable-host case.
    void recomputeLayout();

    //--- drawing ---------------------------------------------------------
    void rebuildBackground();
    void releaseBackground();
    void drawStaticLayer(Canvas &c);
    void compose(Canvas &c);
    void composeHead(Canvas &c);
    void composeCabinet(Canvas &c);
    void composePedalboard(Canvas &c);
    void drawPedalboardStatic(Canvas &c);
    void composeSettings(Canvas &c);
    // Everything that does NOT scroll, drawn after the scroll translate is undone: the back
    // button, the scrollbar, and the browser overlay. The back button is chrome rather than page
    // content because it is the way out, and a scroll that can hide the exit is a trap.
    void composeChrome(Canvas &c);
    void drawScrollBar(Canvas &c);

public:
    // Keyboard, from the host. The SDK is explicit that a view must NOT take keys from platform
    // callbacks and must let the host pass them in here (pluginterfaces/gui/iplugview.h), which is
    // why the X11 window does not select KeyPressMask.
    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 keyCode,
                                            Steinberg::int16 modifiers) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 keyCode,
                                          Steinberg::int16 modifiers) SMTG_OVERRIDE;

private:
    void drawKnob(Canvas &c, const geo::KnobSpec &k, bool enabled);
    void drawKnobAt(Canvas &c, float cx, float cy, float r, double norm);
    void drawToggle(Canvas &c, const geo::ToggleSpec &t, bool on);
    static void drawToggleFallback(Canvas &c, const Rect &dest, bool batUp);
    // The LED's radius is a parameter because the pedalboard's are smaller than the head's: a
    // pedal's status LED is 10 units across against the amp's 18, which is the proportion the two
    // have on the real objects.
    void drawLed(Canvas &c, float cx, float cy, bool lit, float r = static_cast<float>(geo::kLedR));
    // One pedal's face: its knobs, its mini controls, its LED, its footswitch and its name. The
    // enclosure itself is static and is composited by drawPedalboardStatic.
    void drawPedal(Canvas &c, int pedal);
    void drawPedalSwitch(Canvas &c, float cx, float cy);
    void drawMeter(Canvas &c, const geo::MeterRect &m, float level, float peak);
    void drawButton(Canvas &c, const geo::ButtonSpec &b, bool enabled = true);
    void drawIrRow(Canvas &c, int slot);
    void drawGear(Canvas &c);

    //--- interaction -----------------------------------------------------
    void editParam(Steinberg::Vst::ParamID id, double norm);
    void nudgeParam(Steinberg::Vst::ParamID id, double delta);
    void startDrag(Steinberg::Vst::ParamID id, float x, float y, bool horizontal = false);
    // Step a list parameter by one, wrapping. The Delay's Sync division is the only one today;
    // it is a click rather than a drag because twelve values over a 62-unit box is not a drag.
    void cycleList(const PedalParamSpec &spec, int dir);
    bool handleHeadClick(float x, float y);
    bool handleCabinetClick(float x, float y);
    bool handlePedalboardClick(float x, float y);
    bool handleSettingsClick(float x, float y);
    bool handleIrRowClick(int slot, float x, float y);
    // Load the previous (-1) or next (+1) IR in slot `slot`'s own folder.
    void stepIr(int slot, int dir);
    void openIrBrowser(int slot);
    // The settings page's four capture rows. Opens the same browser the IR rows use, in the mode
    // that accepts either a folder or one file — which has been in FileBrowser since it was ported
    // and, until now, had nothing in this plug-in to call it.
    void openCaptureBrowser(int channel);
    // Size the browser card to the VIEWPORT rather than to the page, because the settings page
    // may be taller than the window showing it. Called at open and again on every resize.
    void boundCaptureBrowser();

    // --- renaming a channel ----------------------------------------------------------------
    // The one place this editor takes typed input, and the only one it is ever likely to: every
    // other control here is a knob, a switch or a list, which is deliberate — see the note on
    // onKeyDown in the .cpp for why a plug-in view cannot count on getting keys at all.
    void beginRename(int channel);
    void commitRename();
    void cancelRename();
    // Returns true when the key was consumed, which is exactly what onKeyDown must report to the
    // host: a wrong "yes" swallows the host's own key commands, and the transport is one of them.
    bool handleRenameKey(Steinberg::char16 key, Steinberg::int16 keyCode,
                         Steinberg::int16 modifiers);
    void pollCaps();
    // Ask the processor what the learn table now says, while a row is waiting to be taught.
    void pollMidi();

    //--- helpers ---------------------------------------------------------
    double paramValue(Steinberg::Vst::ParamID id) const;
    std::string paramText(Steinberg::Vst::ParamID id) const;
    bool irFile(int slot, std::string &out) const;
    // Which of the four channels kChannelId asks for, and which one is actually sounding. They
    // differ while a switch is held waiting for its capture to be built — see the definitions.
    int requestedChannel() const;
    int activeChannel() const;
    // Text shown under a dial while it is being dragged. A channel dial names the capture it is
    // sitting on rather than a number, because that is what the control actually selects.
    std::string knobReadout(const geo::KnobSpec &k) const;
    // One wheel click. On a channel dial that is one capture, so the wheel lands on real captures
    // rather than between them.
    double wheelStep(Steinberg::Vst::ParamID id) const;
    // Index of the capture sounding in channel `c`, falling back to that dial's position before
    // the processor has reported one.
    int captureIndex(int c) const;
    // The blend dial only does anything with both slots filled; with one it is drawn disabled and
    // ignores input, because a mix that can attenuate a one-IR user is a bug, not a blend.
    bool blendActive() const;

    //--- scrolling -------------------------------------------------------
    // True when the current page's content is taller than the window shows. mScrollMax is 0 on
    // every other page and on this one whenever the whole page fits, so the scrollbar is drawn
    // and hit-tested only when it can do something.
    bool scrolling() const
    {
        return mScrollMax > 0.0;
    }
    // The visible height of the page, in logical units. Never more than the page itself.
    double viewportH() const;
    // Clamp into [0, mScrollMax] and repaint if it moved. Returns whether it moved.
    bool setScroll(double y);
    // A page-content Y from a window Y. The one place the scroll is undone for input, mirroring
    // the one place it is applied for drawing.
    float contentY(float fy) const
    {
        return fy + static_cast<float>(mScrollY);
    }
    Rect scrollTrackRect() const;
    Rect scrollThumbRect() const;
    // Thumb grab, or a track click that pages towards the pointer. Returns whether it took the
    // click, so the caller can fall through to the page's own rows when it did not.
    bool handleScrollBarClick(float x, float y);

    static bool hitCircle(float x, float y, float cx, float cy, float r);
    static Rect buttonRect(const geo::ButtonSpec &b);
    static Rect irRowRect(int slot);
    // One settings row, and its Learn (clear = false) or Clear (clear = true) button. Shared by
    // the painter and the hit test so a button that is only drawn on a learned row cannot become
    // clickable on one that is not.
    static Rect midiRowRect(int row);
    geo::ButtonSpec midiButton(int row, bool clear) const;
    static Rect levelRowRect(int row);
    static Rect captureRowRect(int row);
    static Rect captureNameRect(int row);
    static Rect captureClearBox(int row);
    void drawCaptureRow(Canvas &c, int row);
    // The output section's three radio rows and its calibration pair.
    static Rect outputModeRow(int index);
    static Rect calToggleRect();
    static Rect calValueRect();
    void drawOutputSection(Canvas &c);
    // Whether an output control can do anything, which depends on what the SOUNDING channel's
    // captures state about their own levels. Not on the requested channel: a switch that is being
    // held has not happened yet, and the answer has to describe what the player is hearing.
    bool outputModeAvailable(int mode) const;
    bool inputCalibrationAvailable() const;
    static Rect levelSliderRect(int row);
    void drawLevelRow(Canvas &c, int row);
    static Rect rowClearBox(const geo::FileRow &row);
    static const geo::FileRow &irRow(int slot);

    RationsController *mController = nullptr;

    FontStack mFonts;
    ImageCache mImages;
    SvgCache mSvgs;
    bool mResourcesLoaded = false;

    // Which page is showing. Editor-local; see the file header.
    geo::Page mPage = geo::Page::Head;

    // Device-resolution composite of everything static on the current page.
    cairo_surface_t *mBackground = nullptr;
    int mDevW = geo::kWinW, mDevH = geo::kWinH;
    double mScale = 1.0;
    double mOffX = 0.0, mOffY = 0.0;

    // How far the current page is scrolled, in logical units, and how far it can be. Both are 0
    // on every page but settings, and on that one whenever the window is tall enough to show all
    // 928 units, which is the normal case at the default size.
    double mScrollY = 0.0;
    double mScrollMax = 0.0;
    // A scrollbar drag, which is deliberately not an mDragParam: it edits no parameter, touches
    // no host automation, and must survive a pointer that leaves the bar.
    bool mScrollDrag = false;
    float mScrollGrabY = 0.0f;
    double mScrollGrabScroll = 0.0;

    FileBrowser mBrowser;
    int mBrowserSlot = 0; // which IR row opened it
    // Which capture row opened the browser, or -1 when an IR row did. One field rather than a
    // bool beside mBrowserSlot, so "an IR row opened it" and "which one" cannot disagree.
    int mBrowserChannel = -1;

    // Which channel's name is being typed, or -1. The text is held here rather than pushed at the
    // controller on every keystroke so that Escape has something to go back to.
    int mRenaming = -1;
    std::string mRenameText;
    size_t mRenameCaret = 0;

    Steinberg::Vst::ParamID mDragParam = 0; // 0 = no active drag
    float mDragStartY = 0;
    double mDragStartNorm = 0;
    // A knob is dragged vertically and a level slider horizontally, so the drag carries which
    // axis it is on rather than the hit test having to leave a knob's coordinate in a Y field.
    bool mDragHorizontal = false;
    float mDragStartX = 0;
    float mMouseX = 0, mMouseY = 0;

    // Meter display state: fast attack from ParamChanged, exponential release in onTick, plus a
    // peak marker that holds and then falls.
    float mInDisp = 0, mOutDisp = 0;
    float mInPeak = 0, mOutPeak = 0;
    int mInPeakHold = 0, mOutPeakHold = 0;

    // What the four banks hold, indexed by Channel.
    int mEntryCount[kChannelCount] = {0, 0, 0, 0};
    std::vector<std::string> mCaptureNames[kChannelCount];
    int mActiveIndex = -1; // capture sounding in the ACTIVE channel; -1 = not reported yet
    float mBankProgress = 0.0f;

    // Capability polling. The capture names are produced by the processor's worker threads, so
    // they are not available when the plug-in is first created; the editor asks again until they
    // arrive and then stops, so this costs nothing in the steady state.
    bool mCapsSettled = false;
    bool mProgressComplete = false;
    int mCapsPollTicks = 0;
    int mCapsPolls = 0;
    int mMidiPollTicks = 0;
};

} // namespace Rations
