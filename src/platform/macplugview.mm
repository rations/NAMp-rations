// MacPlugView implementation. See macplugview.h for the embedding contract, the
// points-versus-pixels rule, and how both differ from the other two platforms.
//
// Every AppKit and CoreGraphics call below was verified against a working
// implementation in a reference tree on this machine rather than written from
// memory, because none of these headers exist here to check against. The three
// that were read are named at their sites:
//
//   vst3sdk/vstgui4/vstgui/lib/platform/mac/  VSTGUI's own NSView for a VST3
//                                             plug-in editor — the closest
//                                             possible reference for this file.
//   third_party/win-deps/cairo-1.18.4/src/    cairo's own ARGB32 -> CoreGraphics
//                                             code, which is the authority on
//                                             how OUR pixels must be described.
//   NeuralAmpModelerPlugin/iPlug2/WDL/swell/  SWELL's BitBlt, which is the same
//                                             problem this file solves: a
//                                             top-down 32-bit buffer onto a
//                                             flipped Cocoa view.

#include "macplugview.h"

#include "pluginterfaces/base/keycodes.h"

#import <Cocoa/Cocoa.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
// ~30 Hz, matching the other two platforms and the original plug-in's meter refresh.
constexpr double kTimerSeconds = 1.0 / 30.0;

// The hook contract uses X button numbers, so that RationsEditorView's hit-testing
// is identical on all three platforms.
constexpr int kButtonLeft = 1;
constexpr int kButtonMiddle = 2;
constexpr int kButtonRight = 3;
} // namespace

//------------------------------------------------------------------------
// The character-to-virtual-key table is VSTGUI's (cocoa/cocoahelpers.mm,
// CreateKeyboardEventFromNSEvent), which classifies from the CHARACTER rather
// than from the hardware key code. Two entries in it are not guessable: the key
// labelled Delete on a Mac keyboard sends 0x7f and means Backspace, and the
// arrows and navigation keys arrive as the NS*FunctionKey constants from
// NSText.h rather than as anything ASCII.
static Steinberg::int16 virtualKeyFromUnichar(unichar c)
{
    switch (c) {
        case 8:
        case 0x7f:
            return Steinberg::KEY_BACK;
        case 9:
        case 0x19:
            return Steinberg::KEY_TAB;
        case 0xd:
            return Steinberg::KEY_RETURN;
        case 3: // NSEnterCharacter, the numeric keypad's own Enter
            return Steinberg::KEY_ENTER;
        case 0x1b:
            return Steinberg::KEY_ESCAPE;
        case NSDeleteFunctionKey:
            return Steinberg::KEY_DELETE;
        case NSLeftArrowFunctionKey:
            return Steinberg::KEY_LEFT;
        case NSRightArrowFunctionKey:
            return Steinberg::KEY_RIGHT;
        case NSUpArrowFunctionKey:
            return Steinberg::KEY_UP;
        case NSDownArrowFunctionKey:
            return Steinberg::KEY_DOWN;
        case NSHomeFunctionKey:
            return Steinberg::KEY_HOME;
        case NSEndFunctionKey:
            return Steinberg::KEY_END;
        case NSPageUpFunctionKey:
            return Steinberg::KEY_PAGEUP;
        case NSPageDownFunctionKey:
            return Steinberg::KEY_PAGEDOWN;
        default:
            return 0;
    }
}

//------------------------------------------------------------------------
// The mapping is the SDK's own, from the comments on KeyModifier
// (pluginterfaces/base/keycodes.h:147-150): kShiftKey and kAlternateKey are
// "same on Windows and macOS", kCommandKey is "macOS: cmd key" and kControlKey
// is "macOS: ctrl key". Reading the last two the wrong way round — which is easy,
// because their NAMES suggest the opposite — would make Cmd-S look like a plain
// S to anything that inspects the mask.
static Steinberg::int16 keyModifiersFromFlags(NSEventModifierFlags flags)
{
    Steinberg::int16 mods = 0;
    if (flags & NSEventModifierFlagShift)
        mods |= Steinberg::kShiftKey;
    if (flags & NSEventModifierFlagOption)
        mods |= Steinberg::kAlternateKey;
    if (flags & NSEventModifierFlagCommand)
        mods |= Steinberg::kCommandKey;
    if (flags & NSEventModifierFlagControl)
        mods |= Steinberg::kControlKey;
    return mods;
}

//------------------------------------------------------------------------
// The Objective-C view.
//
// FLAGGED: Objective-C class names live in one flat, process-wide namespace, so
// two DIFFERENT BUILDS of this plug-in loaded into one host would register this
// name twice; the second registration is ignored and both would run the first
// one's method implementations, over ivars that may not have the same layout.
// The name is specific enough that no other vendor can collide with it, and the
// only way to hit it is to have two copies of NAMp-rations in one process, which
// no install of it produces. The general fix, if it is ever wanted, is in the
// SDK: public.sdk/source/vst/utility/objcclassbuilder.h builds a class at run
// time under a name that can be made unique per module, and VSTGUI uses exactly
// that for exactly this reason. It is not used here because it would mean
// writing every method below as a bare C function with (id, SEL, ...) plumbing,
// which is a large amount of hand-verified code bought for a hazard nobody can
// reach.
//
// Reference counting here is MANUAL, not ARC. This file is deliberately not
// compiled with -fobjc-arc: the view's lifetime is owned by a C++ object in the
// same way the Win32 sibling owns its HWND, and the create/destroy shape is
// meant to read the same in both files. (The SDK's module_mac.mm IS built with
// -fobjc-arc, because it is written that way; see CMakeLists.txt.)
//------------------------------------------------------------------------
@interface NAMpRationsPlugView : NSView {
@public
    Steinberg::MacPlugView *mOwner;
    // Set while a left button press is being reported as a RIGHT button, which
    // is what a control-click is on this platform. Held so that the matching up
    // and any drag in between report the same button as the down did — reporting
    // 3 down and 1 up would leave the editor believing a button is still held.
    BOOL mLeftIsRight;
    // Wheel notches, accumulated. A mouse wheel reports +/-1 per detent so this
    // is a no-op for one; a trackpad reports small fractions, which is what has
    // to be summed. The direct analogue of the Win32 side's WHEEL_DELTA
    // remainder.
    double mWheelAccum;
}
@end

@implementation NAMpRationsPlugView

//------------------------------------------------------------------------
- (instancetype)initWithFrame:(NSRect)frameRect
{
    self = [super initWithFrame:frameRect];
    if (!self)
        return nil;
    mOwner = nullptr;
    mLeftIsRight = NO;
    mWheelAccum = 0.0;

    // NSTrackingInVisibleRect makes AppKit keep the area's rectangle in step
    // with the view for us, so a resize needs no updateTrackingAreas bookkeeping
    // at all. Options and the whole approach from VSTGUI's NSViewFrame::
    // initTrackingArea (cocoa/nsviewframe.mm), which uses the same four.
    // ActiveInActiveApp rather than ActiveAlways: an editor in a background
    // application has no business tracking the pointer.
    NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                                    NSTrackingActiveInActiveApp | NSTrackingInVisibleRect;
    NSTrackingArea *area = [[[NSTrackingArea alloc] initWithRect:[self bounds]
                                                         options:options
                                                           owner:self
                                                        userInfo:nil] autorelease];
    [self addTrackingArea:area];
    return self;
}

//------------------------------------------------------------------------
// YES so the origin is top-left and every hit test in rationsview.cpp reads the
// same on all three platforms. Getting this wrong produces a vertically mirrored
// panel rather than an error, which is why it is the first thing in the file.
// VSTGUI's NSViewFrame and Wine's own Cocoa window both return YES here.
- (BOOL)isFlipped
{
    return YES;
}

// Every pixel is painted by the blit, so AppKit need not clear behind us.
- (BOOL)isOpaque
{
    return YES;
}

//------------------------------------------------------------------------
// ONLY while a text field is open, which is RULES.md section 4's requirement
// spelled as one method. Returning YES unconditionally — which is what VSTGUI
// and iPlug2 both do — would let AppKit make this view the first responder on
// any click in it, and the host's own key commands (the space bar in a DAW
// above all) would stop working the moment a user touched a knob.
//
// setKeyboardFocus() sets the flag BEFORE it calls makeFirstResponder:, because
// that call consults this method and would otherwise be refused.
- (BOOL)acceptsFirstResponder
{
    return (mOwner && mOwner->hasKeyboardFocus()) ? YES : NO;
}

// A click into a host window that is not frontmost is delivered rather than
// being swallowed by the activation. Without it the first click on a knob after
// switching applications does nothing.
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    (void)event;
    return YES;
}

- (BOOL)resignFirstResponder
{
    // The focus can be taken away at any moment — the host opening a dialog, the
    // user clicking elsewhere. Let the flag follow reality, or a later release
    // would hand focus somewhere it no longer is. The Win32 sibling does this on
    // WM_KILLFOCUS.
    if (mOwner)
        mOwner->macFocusLost();
    return YES;
}

//------------------------------------------------------------------------
- (void)dealloc
{
    // The owner clears this before releasing us, so a stray event arriving
    // during teardown finds nothing to call.
    mOwner = nullptr;
    [super dealloc];
}

//------------------------------------------------------------------------
- (void)onTimer:(NSTimer *)timer
{
    (void)timer;
    if (mOwner)
        mOwner->macTick();
}

//------------------------------------------------------------------------
- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    if (!mOwner)
        return;
    // The modern accessor, not the deprecated -graphicsPort. VSTGUI's
    // NSViewFrame::drawRect uses exactly this.
    NSGraphicsContext *ctx = [NSGraphicsContext currentContext];
    if (!ctx)
        return;
    mOwner->macDraw([ctx CGContext]);
}

//------------------------------------------------------------------------
// AppKit delivers every mouse location in WINDOW coordinates; this is the
// conversion into our own. With isFlipped returning YES the y is already
// measured from the top. From VSTGUI and from iPlug2's -getMouseXY, which agree.
- (NSPoint)localPoint:(NSEvent *)event
{
    return [self convertPoint:[event locationInWindow] fromView:nil];
}

//------------------------------------------------------------------------
// A control-click is a right-click on this platform, and the editor's
// right-button behaviour (reset a control to its default) is worth having under
// the gesture Mac users actually make. VSTGUI's buttonStateFromNSEvent does the
// same substitution.
- (void)mouseDown:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    mLeftIsRight = ([event modifierFlags] & NSEventModifierFlagControl) ? YES : NO;
    if (mOwner)
        mOwner->macMouseDown(p.x, p.y, mLeftIsRight ? kButtonRight : kButtonLeft);
}

- (void)mouseDragged:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    if (mOwner)
        mOwner->macMouseMove(p.x, p.y);
}

- (void)mouseUp:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    const int button = mLeftIsRight ? kButtonRight : kButtonLeft;
    mLeftIsRight = NO;
    if (mOwner)
        mOwner->macMouseUp(p.x, p.y, button);
}

- (void)rightMouseDown:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    if (mOwner)
        mOwner->macMouseDown(p.x, p.y, kButtonRight);
}

- (void)rightMouseDragged:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    if (mOwner)
        mOwner->macMouseMove(p.x, p.y);
}

- (void)rightMouseUp:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    if (mOwner)
        mOwner->macMouseUp(p.x, p.y, kButtonRight);
}

- (void)otherMouseDown:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    if (mOwner)
        mOwner->macMouseDown(p.x, p.y, kButtonMiddle);
}

- (void)otherMouseDragged:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    if (mOwner)
        mOwner->macMouseMove(p.x, p.y);
}

- (void)otherMouseUp:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    if (mOwner)
        mOwner->macMouseUp(p.x, p.y, kButtonMiddle);
}

//------------------------------------------------------------------------
// AppKit sends mouseDragged: rather than mouseMoved: while a button is held, and
// it sends it to the view that received the mouseDown: WHEREVER THE POINTER GOES.
// That is the implicit grab X11 gives for free and Win32 does not, and it is why
// this file has no counterpart to the Win32 sibling's SetCapture bookkeeping.
- (void)mouseMoved:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    if (mOwner)
        mOwner->macMouseMove(p.x, p.y);
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (mOwner)
        mOwner->macMouseLeave();
}

//------------------------------------------------------------------------
// deltaY is passed through rather than corrected: the sign already reflects the
// user's "natural scrolling" setting, and second-guessing a system preference
// would make the editor scroll the opposite way from every other window on the
// machine.
- (void)scrollWheel:(NSEvent *)event
{
    const NSPoint p = [self localPoint:event];
    if (mOwner)
        mOwner->macMouseWheel(p.x, p.y, [event deltaY]);
}

//------------------------------------------------------------------------
- (void)keyDown:(NSEvent *)event
{
    BOOL handled = NO;
    if (mOwner) {
        // TWO DIFFERENT STRINGS, and the difference is the whole of the shift
        // handling. -charactersIgnoringModifiers gives the key's UNSHIFTED
        // character, which is what the virtual-key table above has to classify;
        // -characters gives the character the keyboard layout and the shift
        // state actually produce, which is what gets typed. Using the first for
        // both would make every letter lower case and reintroduce exactly the
        // defect the channel-name field was fixed for.
        NSString *bare = [event charactersIgnoringModifiers];
        NSString *typed = [event characters];

        Steinberg::int16 virt = 0;
        if ([bare length] > 0)
            virt = virtualKeyFromUnichar([bare characterAtIndex:0]);

        Steinberg::char16 ch = 0;
        if (virt == 0 && [typed length] > 0) {
            const unichar c = [typed characterAtIndex:0];
            // Control characters reach here too; they are already handled as
            // virtual keys above, so anything below 0x20 is dropped rather than
            // inserted as text. The same rule as the Win32 side's WM_CHAR arm.
            if (c >= 0x20 && c < 0x7F)
                ch = static_cast<Steinberg::char16>(c);
        }

        if (virt != 0 || ch != 0)
            handled = mOwner->macKeyDown(ch, virt, keyModifiersFromFlags([event modifierFlags]))
                          ? YES
                          : NO;
    }
    // Anything we did not consume goes on down the responder chain, so the
    // host's own key commands keep working. VSTGUI's keyDown: ends the same way.
    if (!handled)
        [[self nextResponder] keyDown:event];
}

//------------------------------------------------------------------------
// The editor has no key-up behaviour, so this exists only to pass them on: a
// responder that swallows keyUp: while forwarding keyDown: leaves the host with
// half of every keystroke.
- (void)keyUp:(NSEvent *)event
{
    [[self nextResponder] keyUp:event];
}

//------------------------------------------------------------------------
- (void)setFrameSize:(NSSize)newSize
{
    [super setFrameSize:newSize];
    // Reached both from our own onSize() and from a host that resized this view
    // directly without announcing it — the counterpart of the X11 side following
    // ConfigureNotify and the Win32 side following WM_SIZE.
    if (mOwner)
        mOwner->macFrameChanged(newSize.width, newSize.height);
}

//------------------------------------------------------------------------
// The backing scale is a fact about the display, not a negotiation with the
// host, and it CHANGES mid-session when a window is dragged between a Retina
// screen and an external one. Both hooks exist because the first tells us the
// initial value (there is no window, and so no scale, at initWithFrame: time)
// and the second tells us when it moves. From iPlug2's IGraphicsMac_view.mm,
// which uses the same pair.
- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    NSWindow *window = [self window];
    if (window && mOwner)
        mOwner->macBackingScaleChanged([window backingScaleFactor]);
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    NSWindow *window = [self window];
    if (window && mOwner)
        mOwner->macBackingScaleChanged([window backingScaleFactor]);
}

@end

//------------------------------------------------------------------------
namespace Steinberg
{

namespace
{
inline NAMpRationsPlugView *nativeView(void *v)
{
    return static_cast<NAMpRationsPlugView *>(v);
}
} // namespace

//------------------------------------------------------------------------
MacPlugView::MacPlugView(Vst::EditController *controller, ViewRect *size)
    : Vst::EditorView(controller, size)
{
}

MacPlugView::~MacPlugView()
{
    // removedFromParent() is the normal teardown path; this only covers a view
    // destroyed while still attached by a non-conforming host.
    closeView();
}

//------------------------------------------------------------------------
tresult PLUGIN_API MacPlugView::isPlatformTypeSupported(FIDString type)
{
    if (type && std::strcmp(type, kPlatformTypeNSView) == 0)
        return kResultTrue;
    return kResultFalse;
}

//------------------------------------------------------------------------
// CPluginView::attached() always reports success, which would leave a host
// believing in an editor that does not exist. Report what actually happened
// instead: a host that is told the attach failed can fall back to its generic
// parameter panel rather than showing an empty rectangle.
tresult PLUGIN_API MacPlugView::attached(void *parent, FIDString type)
{
    const tresult result = CPluginView::attached(parent, type);
    if (result != kResultOk)
        return result;
    return isWindowOpen() ? kResultOk : kResultFalse;
}

//------------------------------------------------------------------------
void MacPlugView::trace(const char *fmt, ...) const
{
    if (!mTrace)
        return;
    fputs("Rations/mac: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    // The host's stderr is usually a pipe or nothing at all, so without this the
    // interesting lines are still sitting in the CRT's buffer when the user hits
    // the bug.
    fflush(stderr);
}

//------------------------------------------------------------------------
// The two conversions, in one place each. A backing scale of 1.0 makes both the
// identity, which is every non-Retina Mac and is also what keeps this file
// behaving exactly like its Win32 sibling there.
int MacPlugView::toPixels(double points) const
{
    const double px = points * mBackingScale;
    return px > 0.0 ? static_cast<int>(px + 0.5) : 0;
}

double MacPlugView::toPoints(int pixels) const
{
    return mBackingScale > 0.0 ? pixels / mBackingScale : pixels;
}

//------------------------------------------------------------------------
bool MacPlugView::createSurface(int w, int h)
{
    if (w <= 0 || h <= 0)
        return false;

    mSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(mSurface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Rations: cannot create the editor's %dx%d drawing surface\n", w, h);
        destroySurface();
        return false;
    }
    mPixelW = w;
    mPixelH = h;
    return true;
}

void MacPlugView::destroySurface()
{
    if (mSurface) {
        cairo_surface_destroy(mSurface);
        mSurface = nullptr;
    }
    mPixelW = 0;
    mPixelH = 0;
}

//------------------------------------------------------------------------
bool MacPlugView::resizeSurface(int w, int h)
{
    if (w <= 0 || h <= 0)
        return false;
    if (mSurface && w == mPixelW && h == mPixelH)
        return true;

    // Keep the old buffer if the new one cannot be made: drawing at the previous
    // size is wrong but survivable, whereas no buffer would stop the editor
    // painting at all for the rest of the session. Same rule as the Win32 side.
    cairo_surface_t *oldSurface = mSurface;
    const int oldW = mPixelW;
    const int oldH = mPixelH;

    mSurface = nullptr;
    if (!createSurface(w, h)) {
        fprintf(stderr, "Rations: cannot resize the editor buffer to %dx%d\n", w, h);
        mSurface = oldSurface;
        mPixelW = oldW;
        mPixelH = oldH;
        return false;
    }
    if (oldSurface)
        cairo_surface_destroy(oldSurface);
    return true;
}

//------------------------------------------------------------------------
bool MacPlugView::openView(void *parent)
{
    @autoreleasepool {
        mTrace = std::getenv("RATIONS_MAC_TRACE") != nullptr;

        NSView *parentView = static_cast<NSView *>(parent);
        if (!parentView)
            return false;

        // rect is in POINTS here, which is what an NSView frame wants, so this
        // is the one place in the file where no conversion happens.
        const int pointW = rect.getWidth();
        const int pointH = rect.getHeight();

        NAMpRationsPlugView *view =
            [[NAMpRationsPlugView alloc] initWithFrame:NSMakeRect(0, 0, pointW, pointH)];
        if (!view) {
            fprintf(stderr, "Rations: cannot create the editor view\n");
            return false;
        }
        view->mOwner = this;
        mView = view;

        // The backing scale before the first surface is allocated, so a Retina
        // Mac never builds a 1x buffer and immediately throws it away. addSubview
        // is what gives the view a window, and so a scale to read.
        [parentView addSubview:view];
        if (NSWindow *window = [view window])
            mBackingScale = [window backingScaleFactor];

        if (!createSurface(toPixels(pointW), toPixels(pointH))) {
            closeView();
            return false;
        }

        trace("openView: %dx%d points, backing %.1f, buffer %dx%d px", pointW, pointH,
              mBackingScale, mPixelW, mPixelH);
        return true;
    }
}

//------------------------------------------------------------------------
void MacPlugView::closeView()
{
    @autoreleasepool {
        // Never leave the focus pointed at a view that is about to stop existing.
        setKeyboardFocus(false);

        if (mTimer) {
            NSTimer *timer = static_cast<NSTimer *>(mTimer);
            // Invalidate BEFORE the view is released: a scheduled timer retains
            // its target, so an uninvalidated one would both keep the view alive
            // and go on calling into an owner that is being torn down.
            [timer invalidate];
            [timer release];
            mTimer = nullptr;
        }

        destroySurface();

        if (mView) {
            NAMpRationsPlugView *view = nativeView(mView);
            trace("closeView: releasing %p", static_cast<void *>(view));
            // Clear the back pointer first: nothing in this object should be
            // reached from an event once teardown has begun.
            view->mOwner = nullptr;
            mView = nullptr;
            [view removeFromSuperview];
            [view release];
        }
        mPrevFocus = nullptr;
    }
}

//------------------------------------------------------------------------
void MacPlugView::attachedToParent()
{
    if (!systemWindow)
        return;

    mTrace = std::getenv("RATIONS_MAC_TRACE") != nullptr;
    trace("attachedToParent: parent=%p existing view=%p", systemWindow, mView);

    if (!mView && !openView(systemWindow))
        return;

    @autoreleasepool {
        // The repaint tick. kCFRunLoopCommonModes rather than the default mode,
        // and that is not a detail: a timer in the default mode alone STOPS
        // FIRING for the whole of a live window resize and while any menu is
        // tracking, so the editor would freeze mid-drag. Both VSTGUI's MacTimer
        // and SWELL's timer use the common modes for the same reason.
        NSTimer *timer = [NSTimer timerWithTimeInterval:kTimerSeconds
                                                 target:nativeView(mView)
                                               selector:@selector(onTimer:)
                                               userInfo:nil
                                                repeats:YES];
        if (timer) {
            [[NSRunLoop currentRunLoop] addTimer:timer forMode:(NSString *)kCFRunLoopCommonModes];
            mTimer = [timer retain];
        } else {
            fprintf(stderr, "Rations: cannot start the editor's repaint timer; "
                            "the editor will not animate\n");
        }
    }

    onAttached();
    // The subclass caches art at device resolution, so it needs the current size
    // before the first paint — attaching at a restored, non-default size is
    // otherwise drawn once at the wrong scale.
    onResized(mPixelW, mPixelH);
    mDirty = true;

    // Notify the controller (EditorView::attachedToParent -> editorAttached) only
    // once the view exists, so it may immediately push values in.
    Vst::EditorView::attachedToParent();

    redraw();
}

//------------------------------------------------------------------------
void MacPlugView::removedFromParent()
{
    trace("removedFromParent: view=%p", mView);

    // Controller first, so it stops touching this view before anything is torn
    // down.
    Vst::EditorView::removedFromParent();

    onRemoved();
    closeView();
}

//------------------------------------------------------------------------
tresult PLUGIN_API MacPlugView::canResize()
{
    return isResizable() ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
// The host asking whether a size is acceptable, typically once per mouse move
// during a window drag. The rect is in POINTS; the subclass's rule is in device
// pixels, so it is converted in and back out again.
//
// kResultTrue WHETHER OR NOT the rect was changed, which is what VSTGUI does
// (plug-in-bindings/vst3editor.cpp, VST3Editor::checkSizeConstraint). The SDK
// header only says "check if the view can be resized to the given rect, if not
// adjust the rect to the allowed size", which reads either way, so the reference
// implementation is the tie-breaker: every host is tested against VSTGUI
// plug-ins, so behaving exactly like one is what keeps a host from surprising us.
tresult PLUGIN_API MacPlugView::checkSizeConstraint(ViewRect *proposed)
{
    if (!proposed)
        return kInvalidArgument;
    if (!isResizable())
        return kResultFalse;

    int w = toPixels(proposed->getWidth());
    int h = toPixels(proposed->getHeight());
    constrainSize(w, h);
    proposed->right = proposed->left + static_cast<int32>(toPoints(w) + 0.5);
    proposed->bottom = proposed->top + static_cast<int32>(toPoints(h) + 0.5);
    return kResultTrue;
}

//------------------------------------------------------------------------
// The host telling us it has resized the view. This is the ONLY place the native
// view is resized, per the SDK's "please only resize the platform representation
// of the view when onSize() is called".
//
// The buffer and the subclass are NOT updated here: setFrameSize: reaches
// -setFrameSize: on the view, which calls macFrameChanged, which is the single
// place that follows a size. A host that resizes the view without calling
// onSize() lands in exactly the same function.
tresult PLUGIN_API MacPlugView::onSize(ViewRect *newSize)
{
    if (!newSize)
        return kInvalidArgument;

    int w = toPixels(newSize->getWidth());
    int h = toPixels(newSize->getHeight());
    if (isResizable())
        constrainSize(w, h);

    const int pointW = static_cast<int>(toPoints(w) + 0.5);
    const int pointH = static_cast<int>(toPoints(h) + 0.5);

    rect = *newSize;
    rect.right = rect.left + pointW;
    rect.bottom = rect.top + pointH;

    if (mView) {
        @autoreleasepool {
            [nativeView(mView) setFrameSize:NSMakeSize(pointW, pointH)];
        }
    }
    return kResultTrue;
}

//------------------------------------------------------------------------
bool MacPlugView::requestResize(int w, int h)
{
    if (!plugFrame)
        return false;
    const int pointW = static_cast<int>(toPoints(w) + 0.5);
    const int pointH = static_cast<int>(toPoints(h) + 0.5);
    ViewRect proposed(rect.left, rect.top, rect.left + pointW, rect.top + pointH);
    return plugFrame->resizeView(this, &proposed) == kResultTrue;
}

//------------------------------------------------------------------------
// Take the keyboard focus for this view, or give it straight back.
//
// WHY THIS EXISTS, and it is a deliberate departure from the SDK. iplugview.h
// states that a view "must not handle keyboard events by the means of platform
// callbacks, but let the host pass them to the view", and the editor does
// implement IPlugView::onKeyDown for exactly that. Measured, no host available
// here ever calls it, so the SDK route has never once carried a key and the
// editor's one text field could not be typed into. A field that cannot be typed
// into is not a field.
//
// What the SDK rule protects is the host's key commands, and that is preserved
// rather than traded away: focus is taken only while a text field is actually
// open, and is handed back to whoever held it the moment the field closes.
// Outside that window -acceptsFirstResponder returns NO, so AppKit will not even
// offer us the focus on a click, and every key goes where it went before.
// onKeyDown is still implemented and still preferred, so a host that does route
// keys keeps working unchanged.
//
// NEVER GRABS. A makeFirstResponder: the window declines simply leaves the field
// untyped, which is the documented failure and is survivable.
void MacPlugView::setKeyboardFocus(bool wanted)
{
    if (!mView || wanted == mKeyFocus)
        return;

    @autoreleasepool {
        NAMpRationsPlugView *view = nativeView(mView);
        NSWindow *window = [view window];
        if (!window) {
            mKeyFocus = false;
            mPrevFocus = nullptr;
            return;
        }

        if (wanted) {
            // Set BEFORE the request, because -acceptsFirstResponder consults it
            // and would otherwise refuse.
            mKeyFocus = true;
            mPrevFocus = [window firstResponder];
            if (![window makeFirstResponder:view]) {
                mKeyFocus = false;
                mPrevFocus = nullptr;
                trace("keyboard focus refused by the window");
                return;
            }
            trace("keyboard focus taken, previous=%p", mPrevFocus);
            return;
        }

        mKeyFocus = false;
        NSResponder *prev = static_cast<NSResponder *>(mPrevFocus);
        mPrevFocus = nullptr;
        // Only hand it back if we still hold it. Something else may have taken
        // the focus in the meantime, in which case putting it back where WE found
        // it would be taking it away from whatever has it now.
        if (prev && prev != static_cast<NSResponder *>(view) &&
            [window firstResponder] == static_cast<NSResponder *>(view))
            [window makeFirstResponder:prev];
        trace("keyboard focus released to %p", static_cast<void *>(prev));
    }
}

//------------------------------------------------------------------------
void MacPlugView::redraw()
{
    if (!mSurface || !mView)
        return;
    ++mRedrawCount;
    mDirty = false;

    // Compose into the offscreen buffer...
    cairo_t *cr = cairo_create(mSurface);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS)
        onDraw(cr);
    cairo_destroy(cr);

    // ...and ask AppKit for a paint, which reaches macDraw() and blits it. The
    // deferred-paint discipline: nothing is drawn to the screen from inside an
    // input handler, and drawRect: never calls onDraw().
    @autoreleasepool {
        [nativeView(mView) setNeedsDisplay:YES];
    }
}

//------------------------------------------------------------------------
// The blit. The exact analogue of the Win32 side's BitBlt out of a DIB and the
// X11 side's blit out of an image surface, and it is worth naming the three
// things in it that are not guessable:
//
//   THE PIXEL DESCRIPTION IS CAIRO'S OWN. cairo 1.18.4 describes a
//   CAIRO_FORMAT_ARGB32 image surface to CoreGraphics as
//   `kCGBitmapByteOrder32Host | kCGImageAlphaPremultipliedFirst`, 8 bits per
//   component, with the surface's own stride (src/cairo-quartz-image-surface.c,
//   cairo_quartz_image_surface_create). Taken from there rather than reasoned
//   about, because a wrong byte order is a colour-swapped panel rather than an
//   error, and 32Big — which VSTGUI uses for its OWN bitmaps — would be exactly
//   that here.
//
//   THE COLOUR SPACE IS THE DISPLAY'S, so CoreGraphics performs no conversion on
//   the way to the screen and the panel is the bytes cairo composed. Both cairo
//   (above) and SWELL's screen blit (swell-gdi.mm, __GetDisplayColorSpace) use
//   CGDisplayCopyColorSpace(CGMainDisplayID()) for this.
//
//   THE FLIP IS REQUIRED. CGContextDrawImage places an image bottom-up in the
//   current user space, and this view is flipped, so the image arrives upside
//   down unless the y axis is inverted around the destination rectangle first.
//   SWELL's BitBlt does the identical pair — CGContextScaleCTM(ctx, 1, -1) with a
//   destination rect whose origin y is negated (swell-gdi.mm, `CGRectMake(x,
//   -desth-y, destw, desth)`).
//
// The destination rectangle is in POINTS while the image is in device pixels,
// which is what makes a Retina Mac land 1:1: a 2266x806 image drawn into a
// 1133x403 point rect on a 2x backing store is one image pixel per screen pixel.
void MacPlugView::macDraw(void *cgContext)
{
    CGContextRef ctx = static_cast<CGContextRef>(cgContext);
    if (!ctx || !mSurface || mPixelW <= 0 || mPixelH <= 0)
        return;

    // Everything Cairo has drawn must have reached the buffer before CoreGraphics
    // reads it.
    cairo_surface_flush(mSurface);
    unsigned char *data = cairo_image_surface_get_data(mSurface);
    if (!data)
        return;
    const int stride = cairo_image_surface_get_stride(mSurface);

    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, data, static_cast<size_t>(stride) * static_cast<size_t>(mPixelH), nullptr);
    if (!provider)
        return;

    CGColorSpaceRef colorSpace = CGDisplayCopyColorSpace(CGMainDisplayID());
    const CGBitmapInfo bitmapInfo =
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Host;
    CGImageRef image = CGImageCreate(static_cast<size_t>(mPixelW), static_cast<size_t>(mPixelH), 8,
                                     32, static_cast<size_t>(stride), colorSpace, bitmapInfo,
                                     provider, nullptr, false, kCGRenderingIntentDefault);

    if (image) {
        const CGFloat w = static_cast<CGFloat>(toPoints(mPixelW));
        const CGFloat h = static_cast<CGFloat>(toPoints(mPixelH));
        CGContextSaveGState(ctx);
        // Nearest-neighbour: the image and the backing store are the same size,
        // so there is nothing to interpolate, and asking for smoothing would only
        // cost time. SWELL's blit does the same.
        CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
        CGContextScaleCTM(ctx, 1.0, -1.0);
        CGContextDrawImage(ctx, CGRectMake(0, -h, w, h), image);
        CGContextRestoreGState(ctx);
        CGImageRelease(image);
    }

    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
}

//------------------------------------------------------------------------
void MacPlugView::macTick()
{
    ++mTickCount;
    onTick();
    if (mDirty)
        redraw();
}

//------------------------------------------------------------------------
// AppKit hands over points; the hook set is in device pixels. This is the third
// of the four conversion sites named at the top of macplugview.h.
void MacPlugView::macMouseDown(double x, double y, int button)
{
    onMouseDown(toPixels(x), toPixels(y), button);
}

void MacPlugView::macMouseUp(double x, double y, int button)
{
    onMouseUp(toPixels(x), toPixels(y), button);
}

void MacPlugView::macMouseMove(double x, double y)
{
    onMouseMove(toPixels(x), toPixels(y));
}

void MacPlugView::macMouseLeave()
{
    onMouseLeave();
}

//------------------------------------------------------------------------
void MacPlugView::macMouseWheel(double x, double y, double deltaY)
{
    NAMpRationsPlugView *view = nativeView(mView);
    if (!view)
        return;
    view->mWheelAccum += deltaY;
    const int px = toPixels(x);
    const int py = toPixels(y);
    while (view->mWheelAccum >= 1.0) {
        view->mWheelAccum -= 1.0;
        onMouseWheel(px, py, 1);
    }
    while (view->mWheelAccum <= -1.0) {
        view->mWheelAccum += 1.0;
        onMouseWheel(px, py, -1);
    }
}

//------------------------------------------------------------------------
bool MacPlugView::macKeyDown(char16 key, int16 keyCode, int16 modifiers)
{
    return onKeyDownNative(key, keyCode, modifiers);
}

void MacPlugView::macFocusLost()
{
    mKeyFocus = false;
    mPrevFocus = nullptr;
}

//------------------------------------------------------------------------
// The single place a size is followed, whether it came from our own onSize() or
// straight from a host that resized the view without saying so.
void MacPlugView::macFrameChanged(double pointW, double pointH)
{
    const int w = toPixels(pointW);
    const int h = toPixels(pointH);
    if (w <= 0 || h <= 0)
        return;

    // rect is what getSize() reports, so it follows even when the buffer does not
    // have to be rebuilt.
    rect.right = rect.left + static_cast<int32>(pointW + 0.5);
    rect.bottom = rect.top + static_cast<int32>(pointH + 0.5);

    // Before openView() has built the buffer there is nothing to follow, and
    // building one here would leak the one openView is about to create. AppKit
    // reaches -setFrameSize: from inside -initWithFrame: and again from
    // -addSubview:, both of which happen while openView is still running.
    if (!mSurface || (w == mPixelW && h == mPixelH))
        return;

    resizeSurface(w, h);
    onResized(mPixelW, mPixelH);
    mDirty = true;
    // Repaint now rather than on the next tick: during a drag the host shows the
    // view at its new size immediately, and waiting up to 33 ms for the timer
    // shows a stale or torn frame at every step.
    redraw();
}

//------------------------------------------------------------------------
// A window dragged from a Retina display to an external one, or the reverse.
// The point size has not changed, so the host is told nothing and rect is
// untouched; what changes is how many pixels that many points are.
void MacPlugView::macBackingScaleChanged(double scale)
{
    if (!(scale > 0.0) || scale == mBackingScale)
        return;
    trace("backing scale %.1f -> %.1f", mBackingScale, scale);
    mBackingScale = scale;

    // -viewDidMoveToWindow fires from inside -addSubview:, which openView calls
    // BEFORE it creates the buffer -- deliberately, so that a Retina Mac never
    // builds a 1x buffer and immediately throws it away. Recording the scale and
    // returning is the whole of what is wanted there.
    if (!mSurface)
        return;

    const int w = toPixels(rect.getWidth());
    const int h = toPixels(rect.getHeight());
    if (w <= 0 || h <= 0 || (w == mPixelW && h == mPixelH))
        return;

    resizeSurface(w, h);
    onResized(mPixelW, mPixelH);
    mDirty = true;
    redraw();
}

//------------------------------------------------------------------------
} // namespace Steinberg
