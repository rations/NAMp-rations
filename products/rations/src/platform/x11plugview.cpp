// X11PlugView implementation. See x11plugview.h for the run-loop contract.

#include "x11plugview.h"

#include "pluginterfaces/base/keycodes.h"

#include <cairo/cairo-xlib.h>

#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace Steinberg
{

namespace
{
// ~30 Hz, matching the original plug-in's meter refresh.
constexpr Linux::TimerInterval kTimerMs = 33;

// X wheel events arrive as presses of buttons 4 (up) and 5 (down).
constexpr unsigned int kWheelUp = 4;
constexpr unsigned int kWheelDown = 5;

// XEmbed (https://standards.freedesktop.org/xembed-spec/): _XEMBED_INFO is two
// CARD32s, {version, flags}. XEMBED_MAPPED asks the embedder to map us.
constexpr unsigned long kXEmbedVersion = 0;

// How many timer ticks to wait for an embedder to map us before doing it
// ourselves (see ensureMapped).
constexpr int kMapFallbackTicks = 6; // ~200 ms at 33 ms

// One trace summary line per ~1 s, and the threshold above which a repaint is
// interesting enough to log on its own (i.e. the editor was quiet and then
// something woke it up).
constexpr unsigned long kSummaryTicks = 30;
constexpr unsigned long kQuietRedrawTicks = 15;

//------------------------------------------------------------------------
// One X keysym to one VirtualKeyCodes value (pluginterfaces/base/keycodes.h), or 0 for a key
// that has no virtual code and is therefore carried by its character instead. Only the keys a
// text field needs are mapped: the point of this table is editing, not a general key API, and a
// code invented for a key nobody handles would be a guess rather than a translation.
int16 virtualKeyFromKeySym(KeySym sym)
{
    switch (sym) {
        case XK_BackSpace:
            return KEY_BACK;
        case XK_Tab:
            return KEY_TAB;
        case XK_Return:
            return KEY_RETURN;
        case XK_KP_Enter:
            return KEY_ENTER;
        case XK_Escape:
            return KEY_ESCAPE;
        case XK_Delete:
        case XK_KP_Delete:
            return KEY_DELETE;
        case XK_Left:
        case XK_KP_Left:
            return KEY_LEFT;
        case XK_Right:
        case XK_KP_Right:
            return KEY_RIGHT;
        case XK_Up:
        case XK_KP_Up:
            return KEY_UP;
        case XK_Down:
        case XK_KP_Down:
            return KEY_DOWN;
        case XK_Home:
        case XK_KP_Home:
            return KEY_HOME;
        case XK_End:
        case XK_KP_End:
            return KEY_END;
        case XK_Page_Up:
        case XK_KP_Page_Up:
            return KEY_PAGEUP;
        case XK_Page_Down:
        case XK_KP_Page_Down:
            return KEY_PAGEDOWN;
        default:
            return 0;
    }
}

//------------------------------------------------------------------------
// Non-fatal X error handling.
//
// Xlib's default error handler calls exit(), so one BadWindow — a host that
// destroys its container before calling removed(), a stale resource id after a
// re-embed — takes the whole host process down with it. Replace it with a
// handler that logs and returns.
//
// Ownership/threading: gOurDisplays, gPreviousErrorHandler and gErrorHandlerOnce
// are process-wide and guarded by gErrorMutex. Displays are added in
// openWindow() and removed in closeWindow(), both of which run on the host's
// run-loop thread; the handler itself can be entered from any thread that makes
// an X call, which is why the lock is taken there too. Errors on a display that
// is not ours are forwarded to whatever handler the host had installed, so this
// never swallows the host's own diagnostics.
std::mutex gErrorMutex;
std::vector<::Display *> gOurDisplays;
XErrorHandler gPreviousErrorHandler = nullptr;
std::once_flag gErrorHandlerOnce;

// Counts errors on our own connections. X requests are asynchronous, so a
// rejected CreateWindow does not fail in place — the only way to find out is to
// sample this, round-trip, and sample it again.
std::atomic<unsigned long> gErrorCount{0};

int xErrorHandler(::Display *display, XErrorEvent *event)
{
    bool ours = false;
    XErrorHandler previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(gErrorMutex);
        for (::Display *d : gOurDisplays) {
            if (d == display) {
                ours = true;
                break;
            }
        }
        previous = gPreviousErrorHandler;
    }

    if (!ours && previous)
        return previous(display, event);

    gErrorCount.fetch_add(1);

    char text[128];
    text[0] = '\0';
    XGetErrorText(display, event->error_code, text, sizeof(text));
    fprintf(stderr, "Rations: X error %u (%s) on request %u.%u, resource 0x%lx — ignored\n",
            static_cast<unsigned>(event->error_code), text,
            static_cast<unsigned>(event->request_code), static_cast<unsigned>(event->minor_code),
            static_cast<unsigned long>(event->resourceid));
    return 0;
}

void registerDisplay(::Display *display)
{
    std::call_once(gErrorHandlerOnce,
                   [] { gPreviousErrorHandler = XSetErrorHandler(xErrorHandler); });
    std::lock_guard<std::mutex> lock(gErrorMutex);
    gOurDisplays.push_back(display);
}

void unregisterDisplay(::Display *display)
{
    std::lock_guard<std::mutex> lock(gErrorMutex);
    for (size_t i = 0; i < gOurDisplays.size(); ++i) {
        if (gOurDisplays[i] == display) {
            gOurDisplays.erase(gOurDisplays.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

//------------------------------------------------------------------------
// Trace helpers. Everything here is inert unless RATIONS_X11_TRACE is set.
const char *eventName(int type)
{
    // Indexed by the X protocol event codes in X11/X.h (KeyPress == 2 ...
    // GenericEvent == 35).
    // clang-format off
    static const char *const kNames[] = {
        "0",              "1",              "KeyPress",         "KeyRelease",
        "ButtonPress",    "ButtonRelease",  "MotionNotify",     "EnterNotify",
        "LeaveNotify",    "FocusIn",        "FocusOut",         "KeymapNotify",
        "Expose",         "GraphicsExpose", "NoExpose",         "VisibilityNotify",
        "CreateNotify",   "DestroyNotify",  "UnmapNotify",      "MapNotify",
        "MapRequest",     "ReparentNotify", "ConfigureNotify",  "ConfigureRequest",
        "GravityNotify",  "ResizeRequest",  "CirculateNotify",  "CirculateRequest",
        "PropertyNotify", "SelectionClear", "SelectionRequest", "SelectionNotify",
        "ColormapNotify", "ClientMessage",  "MappingNotify",    "GenericEvent"};
    // clang-format on
    if (type < 0 || type >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0])))
        return "?";
    return kNames[type];
}

const char *mapStateName(int state)
{
    switch (state) {
        case IsUnmapped:
            return "IsUnmapped";
        case IsUnviewable:
            return "IsUnviewable";
        case IsViewable:
            return "IsViewable";
        default:
            return "?";
    }
}

const char *visibilityName(int state)
{
    switch (state) {
        case VisibilityUnobscured:
            return "Unobscured";
        case VisibilityPartiallyObscured:
            return "PartiallyObscured";
        case VisibilityFullyObscured:
            return "FullyObscured";
        default:
            return "?";
    }
}
} // namespace

//------------------------------------------------------------------------
X11PlugView::X11PlugView(Vst::EditController *controller, ViewRect *size)
    : Vst::EditorView(controller, size)
{
}

X11PlugView::~X11PlugView()
{
    // removedFromParent() is the normal teardown path; this only covers a
    // view destroyed while still attached by a non-conforming host.
    closeWindow();
}

//------------------------------------------------------------------------
tresult PLUGIN_API X11PlugView::isPlatformTypeSupported(FIDString type)
{
    if (type && std::strcmp(type, kPlatformTypeX11EmbedWindowID) == 0)
        return kResultTrue;
    return kResultFalse;
}

//------------------------------------------------------------------------
// CPluginView::attached() always reports success, which would leave a host
// believing in an editor that does not exist. Report what actually happened
// instead: a host that is told the attach failed can fall back to its generic
// parameter panel rather than showing an empty rectangle.
tresult PLUGIN_API X11PlugView::attached(void *parent, FIDString type)
{
    const tresult result = CPluginView::attached(parent, type);
    if (result != kResultOk)
        return result;
    return isWindowOpen() ? kResultOk : kResultFalse;
}

//------------------------------------------------------------------------
void X11PlugView::trace(const char *fmt, ...) const
{
    if (!mTrace)
        return;
    fputs("Rations/x11: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    // The host's stderr is usually a pipe, so without this the interesting
    // lines are still sitting in libc's buffer when the user hits the bug.
    fflush(stderr);
}

//------------------------------------------------------------------------
// One line per X event, logged BEFORE the "is this for our window" filter and
// before the dispatch switch, so events the editor currently ignores (XEmbed
// client messages above all) still show up.
void X11PlugView::traceEvent(const XEvent &event) const
{
    if (!mTrace)
        return;

    const char *mine = (event.xany.window == mWindow) ? "self" : "other";
    switch (event.type) {
        case Expose:
            trace("ev %s %s win=0x%lx %dx%d+%d+%d count=%d", eventName(event.type), mine,
                  event.xany.window, event.xexpose.width, event.xexpose.height, event.xexpose.x,
                  event.xexpose.y, event.xexpose.count);
            break;
        case VisibilityNotify:
            trace("ev %s %s win=0x%lx state=%s", eventName(event.type), mine, event.xany.window,
                  visibilityName(event.xvisibility.state));
            break;
        case ConfigureNotify:
            trace("ev %s %s win=0x%lx %dx%d+%d+%d above=0x%lx", eventName(event.type), mine,
                  event.xany.window, event.xconfigure.width, event.xconfigure.height,
                  event.xconfigure.x, event.xconfigure.y, event.xconfigure.above);
            break;
        case ReparentNotify:
            trace("ev %s %s win=0x%lx newparent=0x%lx at +%d+%d", eventName(event.type), mine,
                  event.xany.window, event.xreparent.parent, event.xreparent.x, event.xreparent.y);
            break;
        case PropertyNotify: {
            char *name = XGetAtomName(event.xany.display, event.xproperty.atom);
            trace("ev %s %s win=0x%lx atom=%s state=%s", eventName(event.type), mine,
                  event.xany.window, name ? name : "?",
                  event.xproperty.state == PropertyNewValue ? "NewValue" : "Delete");
            if (name)
                XFree(name);
            break;
        }
        case ClientMessage: {
            char *name = XGetAtomName(event.xany.display, event.xclient.message_type);
            trace("ev %s %s win=0x%lx type=%s fmt=%d data=%ld,%ld,%ld,%ld,%ld",
                  eventName(event.type), mine, event.xany.window, name ? name : "?",
                  event.xclient.format, event.xclient.data.l[0], event.xclient.data.l[1],
                  event.xclient.data.l[2], event.xclient.data.l[3], event.xclient.data.l[4]);
            if (name)
                XFree(name);
            break;
        }
        case MotionNotify:
            // Compressed below and far too frequent to log one by one.
            break;
        default:
            trace("ev %s %s win=0x%lx", eventName(event.type), mine, event.xany.window);
            break;
    }
}

//------------------------------------------------------------------------
// The ground truth the event stream cannot give us: whether the server thinks
// our window is viewable, where it is, and how it is stacked against the other
// plug-ins' windows in the host's container.
void X11PlugView::traceWindowState(const char *when) const
{
    if (!mTrace || !mDisplay || !mWindow)
        return;

    XWindowAttributes attrs;
    if (XGetWindowAttributes(mDisplay, mWindow, &attrs) == 0) {
        trace("%s: cannot read our own window attributes", when);
        return;
    }
    trace("%s: self 0x%lx %s %dx%d+%d+%d mapped=%d dirty=%d", when, mWindow,
          mapStateName(attrs.map_state), attrs.width, attrs.height, attrs.x, attrs.y,
          mMapped ? 1 : 0, mDirty ? 1 : 0);

    const ::Window parent = static_cast<::Window>(reinterpret_cast<uintptr_t>(systemWindow));
    if (!parent)
        return;

    XWindowAttributes parentAttrs;
    if (XGetWindowAttributes(mDisplay, parent, &parentAttrs) == 0) {
        trace("%s: cannot read parent 0x%lx attributes", when, parent);
        return;
    }
    trace("%s: parent 0x%lx %s %dx%d+%d+%d", when, parent, mapStateName(parentAttrs.map_state),
          parentAttrs.width, parentAttrs.height, parentAttrs.x, parentAttrs.y);

    // Siblings, bottom-to-top: this is what tells us whether another plug-in's
    // window is simply stacked over ours.
    ::Window root = 0, treeParent = 0, *children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(mDisplay, parent, &root, &treeParent, &children, &count) == 0)
        return;
    for (unsigned int i = 0; i < count && i < 8; ++i) {
        XWindowAttributes child;
        if (XGetWindowAttributes(mDisplay, children[i], &child) == 0)
            continue;
        trace("%s:   child[%u] 0x%lx %s %dx%d+%d+%d%s", when, i, children[i],
              mapStateName(child.map_state), child.width, child.height, child.x, child.y,
              children[i] == mWindow ? "  <-- us" : "");
    }
    if (count > 8)
        trace("%s:   ... %u more children", when, count - 8);
    if (children)
        XFree(children);
}

//------------------------------------------------------------------------
bool X11PlugView::openWindow(::Window parent)
{
    mTrace = std::getenv("RATIONS_X11_TRACE") != nullptr;

    // The host does not share its Display connection, so open our own. Its
    // file descriptor is what gets registered with the run loop below.
    mDisplay = XOpenDisplay(nullptr);
    if (!mDisplay) {
        fprintf(stderr, "Rations: cannot open an X display for the editor\n");
        return false;
    }
    // From here on an X error on this connection is ours to survive rather than
    // the host's to die of.
    registerDisplay(mDisplay);

    const int screen = DefaultScreen(mDisplay);
    const unsigned width = static_cast<unsigned>(rect.getWidth());
    const unsigned height = static_cast<unsigned>(rect.getHeight());

    // Inherit the parent's visual, depth and colormap rather than taking the
    // screen defaults. The X protocol says of CreateWindow's colormap
    // attribute: "If CopyFromParent is specified, the parent's colormap is
    // copied ... However, the window must have the same visual type as the
    // parent (or a Match error results)". Leaving the colormap at its default
    // of CopyFromParent while asking for the screen's default visual therefore
    // fails outright against a host whose container uses a different visual —
    // a 32-bit ARGB one, say, which is what compositing toolkits hand out — and
    // the editor window is then never created at all. The SDK's own reference
    // host passes CWColormap for the same reason.
    ::Window colormapRoot = RootWindow(mDisplay, screen);
    Visual *visual = DefaultVisual(mDisplay, screen);
    int depth = DefaultDepth(mDisplay, screen);
    Colormap colormap = DefaultColormap(mDisplay, screen);

    XWindowAttributes parentAttrs;
    if (XGetWindowAttributes(mDisplay, parent, &parentAttrs) != 0 && parentAttrs.visual) {
        visual = parentAttrs.visual;
        depth = parentAttrs.depth;
        colormap = parentAttrs.colormap;
        if (parentAttrs.root)
            colormapRoot = parentAttrs.root;
    } else {
        fprintf(stderr, "Rations: cannot read the host window's attributes; falling back "
                        "to the screen's default visual\n");
    }

    // A parent with no colormap of its own cannot lend us one either ("the
    // parent must not have a colormap of None"), so make one for its visual.
    if (colormap == None) {
        colormap = XCreateColormap(mDisplay, colormapRoot, visual, AllocNone);
        mOwnedColormap = colormap;
    }

    XSetWindowAttributes attrs;
    std::memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixmap = None; // we paint every pixel ourselves
    attrs.border_pixel = 0;         // required whenever our depth differs from the parent's
    attrs.colormap = colormap;
    // KeyPressMask is a DELIBERATE DEPARTURE from the SDK, which says a view "must not handle
    // keyboard events by the means of platform callbacks, but let the host pass them to the view"
    // (pluginterfaces/gui/iplugview.h). It is selected because no host tested here ever calls
    // IPlugView::onKeyDown, so the SDK's route delivers nothing and the editor's one text field
    // was unusable. Selecting the mask costs nothing on its own: X delivers a key to this window
    // only while this window holds the input focus, and focus is taken only around an open text
    // field (setKeyboardFocus). At every other moment keys propagate to the host exactly as
    // before, so the host's own key commands are untouched.
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       LeaveWindowMask | StructureNotifyMask | PropertyChangeMask | KeyPressMask |
                       FocusChangeMask;

    const unsigned long errorsBefore = gErrorCount.load();
    mWindow = XCreateWindow(mDisplay, parent, 0, 0, width, height, 0, depth, InputOutput, visual,
                            CWBackPixmap | CWBorderPixel | CWColormap | CWEventMask, &attrs);

    // XCreateWindow allocates the id locally and sends the request; a rejected
    // request surfaces later as a protocol error, never as a null id. Round-trip
    // once and check, because every later call — ChangeProperty, the cairo
    // surface, XMapWindow — would otherwise be issued against a window that does
    // not exist, and the editor would appear as a silent blank rectangle.
    XSync(mDisplay, False);
    if (!mWindow || gErrorCount.load() != errorsBefore) {
        fprintf(stderr,
                "Rations: the host rejected the editor window (parent 0x%lx, depth %d, "
                "visual 0x%lx)\n",
                parent, depth, visual ? static_cast<unsigned long>(visual->visualid) : 0UL);
        mWindow = 0; // never created, so there is nothing to destroy
        closeWindow();
        return false;
    }
    trace("openWindow: created 0x%lx (%ux%u) depth=%d visual=0x%lx in parent 0x%lx, fd=%d", mWindow,
          width, height, depth, visual ? static_cast<unsigned long>(visual->visualid) : 0UL, parent,
          ConnectionNumber(mDisplay));

    // Announce XEmbed support BEFORE the parent's connection can see the
    // window, because a strict embedder reads this the moment it gets the
    // CreateNotify. The SDK's own reference host does exactly that and calls
    // it a fatal error if the property is missing.
    //
    // flags is 0, NOT XEMBED_MAPPED: under XEmbed the embedder owns mapping,
    // and a window already mapped at CreateNotify is an error to that same
    // reference host. ensureMapped() below covers embedders that never get
    // round to mapping us.
    mXEmbedInfoAtom = XInternAtom(mDisplay, "_XEMBED_INFO", False);
    if (mXEmbedInfoAtom != None) {
        const unsigned long info[2] = {kXEmbedVersion, 0};
        XChangeProperty(mDisplay, mWindow, mXEmbedInfoAtom, mXEmbedInfoAtom, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(info), 2);
    }
    XFlush(mDisplay);

    mTarget = cairo_xlib_surface_create(mDisplay, mWindow, visual, static_cast<int>(width),
                                        static_cast<int>(height));
    mBuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(width),
                                         static_cast<int>(height));
    if (cairo_surface_status(mTarget) != CAIRO_STATUS_SUCCESS ||
        cairo_surface_status(mBuffer) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Rations: cannot create the editor drawing surfaces\n");
        closeWindow();
        return false;
    }

    return true;
}

//------------------------------------------------------------------------
// Take the X input focus for this window, or give it straight back.
//
// WHY THIS EXISTS, and it is a deliberate departure from the SDK. iplugview.h states that a view
// "must not handle keyboard events by the means of platform callbacks, but let the host pass them
// to the view", and the editor does implement IPlugView::onKeyDown for exactly that. Measured,
// no host available here ever calls it: the SDK's own editorhost selects KeyPressMask and then
// never calls onKeyDown anywhere in its source, and Reaper delivers nothing to it either — the
// build in which the text field was first written does not accept a key today, so the SDK route
// has never once carried one. A field that cannot be typed into is not a field.
//
// What the SDK rule protects is the host's key commands, and that is preserved rather than
// traded away: focus is taken only while a text field is actually open, and is handed back to
// whoever held it the moment the field closes. Outside that window this view holds no focus and
// X routes every key to the host exactly as it did before. onKeyDown is still implemented and
// still preferred, so a host that does route keys keeps working unchanged.
void X11PlugView::setKeyboardFocus(bool wanted)
{
    if (!mDisplay || !mWindow || wanted == mKeyFocus)
        return;

    if (wanted) {
        // XSetInputFocus on a window that is not viewable is a BadMatch, and an unmapped editor
        // is an ordinary state here (see ensureMapped) rather than a rare one.
        XWindowAttributes attrs;
        if (XGetWindowAttributes(mDisplay, mWindow, &attrs) == 0 || attrs.map_state != IsViewable)
            return;
        ::Window focus = None;
        int revert = RevertToParent;
        XGetInputFocus(mDisplay, &focus, &revert);
        mPrevFocus = focus;
        mPrevRevert = revert;
        XSetInputFocus(mDisplay, mWindow, RevertToParent, CurrentTime);
        XFlush(mDisplay);
        mKeyFocus = true;
        trace("keyboard focus taken, previous=0x%lx", mPrevFocus);
        return;
    }

    mKeyFocus = false;
    const ::Window prev = mPrevFocus;
    mPrevFocus = None;
    // PointerRoot and None are legal focus values in their own right and are handed back as they
    // are; a real window may have been destroyed while we held the focus, so it is probed first
    // rather than trusted. A failed probe leaves the focus where it is, which is this window —
    // wrong, but far better than pointing it at a dead id.
    if (prev == PointerRoot || prev == None) {
        XSetInputFocus(mDisplay, prev, mPrevRevert, CurrentTime);
    } else if (prev != mWindow) {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(mDisplay, prev, &attrs) != 0)
            XSetInputFocus(mDisplay, prev, mPrevRevert, CurrentTime);
    }
    XFlush(mDisplay);
    trace("keyboard focus released to 0x%lx", prev);
}

//------------------------------------------------------------------------
void X11PlugView::closeWindow()
{
    // Never leave the focus pointed at a window that is about to stop existing.
    setKeyboardFocus(false);

    if (mBuffer) {
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
    }
    if (mTarget) {
        cairo_surface_destroy(mTarget);
        mTarget = nullptr;
    }
    if (mDisplay) {
        trace("closeWindow: destroying 0x%lx", mWindow);
        if (mWindow) {
            XDestroyWindow(mDisplay, mWindow);
            mWindow = 0;
        }
        if (mOwnedColormap != None) {
            XFreeColormap(mDisplay, mOwnedColormap);
            mOwnedColormap = None;
        }
        unregisterDisplay(mDisplay);
        XCloseDisplay(mDisplay);
        mDisplay = nullptr;
    }
    mWindow = 0;
    mMapped = false;
    mTicksUnmapped = 0;
}

//------------------------------------------------------------------------
// Grow/shrink the X window and both drawing surfaces to w x h. The xlib surface
// is told its new size in place (it wraps a window we resized); the offscreen
// buffer has a fixed allocation and has to be rebuilt.
bool X11PlugView::resizeSurfaces(int w, int h)
{
    if (!mDisplay || !mWindow || w <= 0 || h <= 0)
        return false;

    XResizeWindow(mDisplay, mWindow, static_cast<unsigned>(w), static_cast<unsigned>(h));
    XFlush(mDisplay);

    if (mTarget)
        cairo_xlib_surface_set_size(mTarget, w, h);

    cairo_surface_t *buffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(buffer) != CAIRO_STATUS_SUCCESS) {
        // Keep the old buffer: drawing at the previous size is wrong but
        // survivable, whereas a null buffer would stop the editor painting at
        // all for the rest of the session.
        fprintf(stderr, "Rations: cannot resize the editor buffer to %dx%d\n", w, h);
        cairo_surface_destroy(buffer);
        return false;
    }
    if (mBuffer)
        cairo_surface_destroy(mBuffer);
    mBuffer = buffer;
    return true;
}

//------------------------------------------------------------------------
void X11PlugView::attachedToParent()
{
    const ::Window parent = static_cast<::Window>(reinterpret_cast<uintptr_t>(systemWindow));
    if (!parent)
        return;

    mTrace = std::getenv("RATIONS_X11_TRACE") != nullptr;
    trace("attachedToParent: parent=0x%lx existing window=0x%lx mapped=%d", parent, mWindow,
          mMapped ? 1 : 0);

    if (!mWindow && !openWindow(parent))
        return;

    // The run loop belongs to the host and reaches us through IPlugFrame.
    // Without it the editor cannot receive X events or tick, so say so
    // loudly rather than presenting a window that never repaints.
    if (plugFrame) {
        Linux::IRunLoop *runLoop = nullptr;
        if (plugFrame->queryInterface(Linux::IRunLoop::iid, reinterpret_cast<void **>(&runLoop)) ==
                kResultTrue &&
            runLoop) {
            mRunLoop = owned(runLoop);
        }
    }

    if (mRunLoop) {
        if (mRunLoop->registerEventHandler(this, ConnectionNumber(mDisplay)) == kResultTrue)
            mEventHandlerRegistered = true;
        else
            fprintf(stderr, "Rations: the host refused to register the editor's event handler\n");

        if (mRunLoop->registerTimer(this, kTimerMs) == kResultTrue)
            mTimerRegistered = true;
        else
            fprintf(stderr, "Rations: the host refused to register the editor's timer\n");
    } else {
        fprintf(stderr, "Rations: this host provides no Linux::IRunLoop; "
                        "the editor cannot receive events\n");
    }

    onAttached();
    // The subclass caches art at device resolution, so it needs the current
    // size before the first paint — attaching at a restored, non-default size
    // is otherwise drawn once at the wrong scale.
    onResized(rect.getWidth(), rect.getHeight());
    mDirty = true;

    // Notify the controller (EditorView::attachedToParent -> editorAttached)
    // only once the window exists, so it may immediately push values in.
    Vst::EditorView::attachedToParent();

    redraw();
    traceWindowState("attached");
}

//------------------------------------------------------------------------
void X11PlugView::removedFromParent()
{
    trace("removedFromParent: window=0x%lx mapped=%d", mWindow, mMapped ? 1 : 0);

    // Controller first, so it stops touching this view before anything is
    // torn down.
    Vst::EditorView::removedFromParent();

    // Then the run loop: unregistering BEFORE the window and display go away
    // is what stops the host calling onFDIsSet() on a closed connection.
    if (mRunLoop) {
        if (mEventHandlerRegistered)
            mRunLoop->unregisterEventHandler(this);
        if (mTimerRegistered)
            mRunLoop->unregisterTimer(this);
    }
    mEventHandlerRegistered = false;
    mTimerRegistered = false;
    mRunLoop = nullptr;

    onRemoved();
    closeWindow();
}

//------------------------------------------------------------------------
tresult PLUGIN_API X11PlugView::canResize()
{
    return isResizable() ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
// The host asking whether a size is acceptable, typically once per mouse move
// during a window drag. Answer by rewriting the rect through the subclass's
// rule; kResultTrue means "I changed it", kResultFalse means "it was already
// fine", and the SDK expects the adjusted rect either way.
tresult PLUGIN_API X11PlugView::checkSizeConstraint(ViewRect *proposed)
{
    if (!proposed)
        return kInvalidArgument;
    if (!isResizable())
        return kResultFalse;

    int w = proposed->getWidth();
    int h = proposed->getHeight();
    constrainSize(w, h);
    proposed->right = proposed->left + w;
    proposed->bottom = proposed->top + h;
    // kResultTrue WHETHER OR NOT the rect was changed, which is what VSTGUI does
    // (vstgui4/vstgui/plug-in-bindings/vst3editor.cpp, VST3Editor::checkSizeConstraint and
    // AspectRatioVST3Editor::checkSizeConstraint: both write the adjusted rect and then return
    // kResultTrue unconditionally). The SDK header only says "check if the view can be resized to
    // the given rect, if not adjust the rect to the allowed size", which reads either way, so the
    // reference implementation is the tie-breaker: every host is tested against VSTGUI plug-ins,
    // so behaving exactly like one is what keeps a host from surprising us.
    //
    // Reporting "unchanged" as kResultFalse looks more informative and buys nothing — the host
    // has the rect either way and can compare it itself.
    return kResultTrue;
}

//------------------------------------------------------------------------
// The host telling us it has resized the parent. This is the ONLY place the X
// window is resized, per the SDK's "please only resize the platform
// representation of the view when onSize() is called".
tresult PLUGIN_API X11PlugView::onSize(ViewRect *newSize)
{
    if (!newSize)
        return kInvalidArgument;

    int w = newSize->getWidth();
    int h = newSize->getHeight();
    if (isResizable())
        constrainSize(w, h);

    const bool changed = (w != rect.getWidth() || h != rect.getHeight());
    rect = *newSize;
    rect.right = rect.left + w;
    rect.bottom = rect.top + h;

    if (changed && isWindowOpen()) {
        resizeSurfaces(w, h);
        onResized(w, h);
        mDirty = true;
        // Repaint now rather than on the next tick: during a drag the host
        // blits our window at its new size immediately, and waiting up to
        // 33 ms for the timer shows a stretched or torn frame at every step.
        redraw();
    }
    return kResultTrue;
}

//------------------------------------------------------------------------
void X11PlugView::onFDIsSet(Linux::FileDescriptor /*fd*/)
{
    ++mFdCount;
    drainEvents();
}

//------------------------------------------------------------------------
void X11PlugView::drainEvents()
{
    if (!mDisplay)
        return;

    while (XPending(mDisplay)) {
        XEvent event;
        XNextEvent(mDisplay, &event);
        ++mEventCount;
        traceEvent(event);
        if (event.xany.window != mWindow)
            continue;

        switch (event.type) {
            case Expose:
                // Coalesce: only the last expose in a burst needs a repaint,
                // and the repaint itself happens on the next tick.
                if (event.xexpose.count == 0)
                    mDirty = true;
                break;

            case ButtonPress:
                if (event.xbutton.button == kWheelUp || event.xbutton.button == kWheelDown) {
                    onMouseWheel(event.xbutton.x, event.xbutton.y,
                                 event.xbutton.button == kWheelUp ? 1 : -1);
                } else {
                    onMouseDown(event.xbutton.x, event.xbutton.y,
                                static_cast<int>(event.xbutton.button));
                }
                break;

            case ButtonRelease:
                if (event.xbutton.button != kWheelUp && event.xbutton.button != kWheelDown)
                    onMouseUp(event.xbutton.x, event.xbutton.y,
                              static_cast<int>(event.xbutton.button));
                break;

            case MotionNotify: {
                // Compress motion: only the most recent position matters for
                // a knob drag, and X can deliver these far faster than 30 Hz.
                XEvent latest = event;
                while (XPending(mDisplay)) {
                    XEvent peek;
                    XPeekEvent(mDisplay, &peek);
                    if (peek.type != MotionNotify || peek.xany.window != mWindow)
                        break;
                    XNextEvent(mDisplay, &latest);
                }
                onMouseMove(latest.xmotion.x, latest.xmotion.y);
                break;
            }

            case LeaveNotify:
                onMouseLeave();
                break;

            case KeyPress: {
                // XLookupString applies the shift and lock state for us and yields the Latin-1
                // byte, which is why the character is taken from it rather than decoded from the
                // keysym by hand. It is asked for one byte because the field being fed is ASCII
                // only; a longer answer is a multi-byte character this editor cannot store.
                XKeyEvent ke = event.xkey;
                char text[8] = {0};
                KeySym sym = NoSymbol;
                const int n = XLookupString(&ke, text, sizeof(text) - 1, &sym, nullptr);
                const unsigned char byte = (n >= 1) ? static_cast<unsigned char>(text[0]) : 0;
                const char16 ch = (byte >= 0x20 && byte < 0x7F) ? static_cast<char16>(byte) : 0;
                // KeyModifier's own mapping, from pluginterfaces/base/keycodes.h: kCommandKey is
                // documented as "Windows: ctrl key" and kControlKey as "Windows: win key", so
                // ControlMask is kCommandKey here and Mod4 (Super) is kControlKey. Getting these
                // the wrong way round would make Ctrl-C read as a plain C.
                int16 mods = 0;
                if (ke.state & ShiftMask)
                    mods |= kShiftKey;
                if (ke.state & ControlMask)
                    mods |= kCommandKey;
                if (ke.state & Mod1Mask)
                    mods |= kAlternateKey;
                if (ke.state & Mod4Mask)
                    mods |= kControlKey;
                if (onKeyDownNative(ch, virtualKeyFromKeySym(sym), mods))
                    mDirty = true;
                break;
            }

            case FocusOut:
                // Focus can be taken away by the host or the window manager at any moment. Let
                // the flag follow reality, or a later release would hand focus somewhere it no
                // longer is and steal it from whoever has it now.
                mKeyFocus = false;
                mPrevFocus = None;
                break;

            case ConfigureNotify:
                // A host that resizes the parent without routing through
                // onSize() still reaches us here. Follow it, so the surfaces
                // never disagree with the window they are painting.
                if (event.xconfigure.width > 0 && event.xconfigure.height > 0 &&
                    (event.xconfigure.width != rect.getWidth() ||
                     event.xconfigure.height != rect.getHeight())) {
                    rect.right = rect.left + event.xconfigure.width;
                    rect.bottom = rect.top + event.xconfigure.height;
                    if (mTarget)
                        cairo_xlib_surface_set_size(mTarget, event.xconfigure.width,
                                                    event.xconfigure.height);
                    cairo_surface_t *buffer = cairo_image_surface_create(
                        CAIRO_FORMAT_ARGB32, event.xconfigure.width, event.xconfigure.height);
                    if (cairo_surface_status(buffer) == CAIRO_STATUS_SUCCESS) {
                        if (mBuffer)
                            cairo_surface_destroy(mBuffer);
                        mBuffer = buffer;
                    } else {
                        cairo_surface_destroy(buffer);
                    }
                    onResized(event.xconfigure.width, event.xconfigure.height);
                    mDirty = true;
                }
                break;

            case MapNotify:
                mMapped = true;
                mDirty = true;
                break;

            case UnmapNotify:
                mMapped = false;
                trace("UnmapNotify: unmapped by someone else, ticksUnmapped=%d", mTicksUnmapped);
                break;

            case PropertyNotify:
                // Reaper sets _XEMBED_INFO on the plug-in's window rather than
                // mapping it, and expects the plug-in to map itself in
                // response. VSTGUI carries the same workaround, flagged
                // "needed for Reaper"; without it the editor never appears.
                if (mXEmbedInfoAtom != None && event.xproperty.atom == mXEmbedInfoAtom)
                    mapWindow();
                break;

            default:
                break;
        }
    }
}

//------------------------------------------------------------------------
void X11PlugView::onTimer()
{
    ++mTickCount;
    ++mTicksSinceRedraw;

    if (mTrace && mTickCount % kSummaryTicks == 0) {
        trace("tick %lu: fdCalls=%lu events=%lu redraws=%lu pending=%d dirty=%d mapped=%d",
              mTickCount, mFdCount, mEventCount, mRedrawCount, mDisplay ? XPending(mDisplay) : -1,
              mDirty ? 1 : 0, mMapped ? 1 : 0);
        traceWindowState("tick");
    }

    ensureMapped();
    onTick();
    if (mDirty)
        redraw();
}

//------------------------------------------------------------------------
void X11PlugView::mapWindow()
{
    if (!mDisplay || !mWindow)
        return;
    if (mMapped) {
        trace("mapWindow: skipped, we believe we are already mapped");
        return;
    }
    trace("mapWindow: mapping 0x%lx ourselves", mWindow);
    XMapWindow(mDisplay, mWindow);
    XFlush(mDisplay);
    mMapped = true;
    mDirty = true;
}

//------------------------------------------------------------------------
// Hosts differ on who maps the plug-in's window. A strict XEmbed embedder maps
// it (and rejects a window that mapped itself); Reaper signals via
// _XEMBED_INFO; and some hosts do neither. Waiting a few ticks and then
// mapping ourselves is what makes all three work: by then a conforming
// embedder has already mapped us and this is a no-op.
void X11PlugView::ensureMapped()
{
    if (mMapped || !mDisplay || !mWindow)
        return;
    if (++mTicksUnmapped < kMapFallbackTicks)
        return;
    trace("ensureMapped: no embedder mapped us after %d ticks", mTicksUnmapped);
    mapWindow();
}

//------------------------------------------------------------------------
void X11PlugView::redraw()
{
    if (!mBuffer || !mTarget || !mDisplay)
        return;
    // A repaint after a long quiet spell is the interesting one: it is what
    // "the editor came back" looks like in the log.
    if (mTicksSinceRedraw >= kQuietRedrawTicks)
        trace("redraw after %lu quiet ticks", mTicksSinceRedraw);
    ++mRedrawCount;
    mTicksSinceRedraw = 0;
    mDirty = false;

    // Compose into the offscreen buffer...
    cairo_t *cr = cairo_create(mBuffer);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS)
        onDraw(cr);
    cairo_destroy(cr);

    // ...then blit it to the window in one operation, so no partially drawn
    // frame is ever visible.
    cairo_t *out = cairo_create(mTarget);
    if (cairo_status(out) == CAIRO_STATUS_SUCCESS) {
        cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(out, mBuffer, 0.0, 0.0);
        cairo_paint(out);
    }
    cairo_destroy(out);

    cairo_surface_flush(mTarget);
    XFlush(mDisplay);
}

//------------------------------------------------------------------------
} // namespace Steinberg
