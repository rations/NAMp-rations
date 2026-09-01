// editor-drive helper — synthesises real pointer input at a window with the X11 XTEST extension.
//
// Real input, not a synthetic ButtonPress sent with XSendEvent: the editor is embedded in the
// host's window and reads the pointer through the ordinary event path, and XSendEvent events
// arrive with send_event set, which a host or toolkit may filter. XTEST goes in at the server, so
// what the plug-in sees is indistinguishable from a hand on the mouse.
//
// Coordinates are given in the target window's own space and translated to root coordinates here,
// so a caller can work in the same logical layout the geometry header describes rather than
// having to know where the host put its window.
//
// It also types, and that is a different question from clicking rather than the same one twice.
// A plug-in view is forbidden by the SDK from taking keys off its own platform window: the host
// receives them and passes them in through IPlugView::onKeyDown. So a synthesised key press proves
// something a synthesised click does not — whether the HOST is willing to route keyboard input to
// an embedded editor at all, which is host policy and varies. The one control here that reads
// typed text is deliberately the second way to do its job for exactly that reason.
//
// Driven by editor-drive.sh, which knows how to find the window in the first place.

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display *display;
static Window window;

static void moveTo(int x, int y)
{
    Window child;
    int rootX = 0, rootY = 0;
    XTranslateCoordinates(display, window, DefaultRootWindow(display), x, y, &rootX, &rootY,
                          &child);
    XTestFakeMotionEvent(display, -1, rootX, rootY, 0);
    XFlush(display);
}

// `which` is the X11 button number: 1 is left, 3 is right. Right matters because two gestures in
// this editor answer on it and nothing here could ask them - resetting a channel trim to 0 dB, and
// dismissing the Slim overlay.
static void button(int down, unsigned int which)
{
    XTestFakeButtonEvent(display, which, down ? True : False, CurrentTime);
    XFlush(display);
}

// The wheel is buttons 4 (up) and 5 (down) in X11, one press-and-release per click. Needed to
// test a scrolling page: the settings page scrolls, and the only two ways to move it are this and
// dragging the scrollbar.
static void wheel(int clicks)
{
    const unsigned int b = clicks > 0 ? 4u : 5u;
    int n = clicks > 0 ? clicks : -clicks;
    for (; n > 0; --n) {
        XTestFakeButtonEvent(display, b, True, CurrentTime);
        XTestFakeButtonEvent(display, b, False, CurrentTime);
        XFlush(display);
        usleep(40000);
    }
}

// Resize the host's TOP-LEVEL window, which is what makes the host call the plug-in's
// checkSizeConstraint and then onSize. Resizing the plug-in's own child window directly would
// change some pixels and tell the plug-in nothing, so the editor would never re-lay-out and the
// test would prove the opposite of what it looked like it proved.
//
// The caller passes the plug-in's child window (it is the one findable by size), so walk up to
// the ancestor whose parent is the root and resize that.
static void resizeTopLevel(int w, int h)
{
    Window top = window;
    for (;;) {
        Window r = 0, parent = 0, *children = NULL;
        unsigned int n = 0;
        if (!XQueryTree(display, top, &r, &parent, &children, &n))
            break;
        if (children)
            XFree(children);
        if (parent == 0 || parent == r)
            break;
        top = parent;
    }
    XResizeWindow(display, top, (unsigned)w, (unsigned)h);
    XFlush(display);
}

// Whether this keysym sits on a SHIFTED level of the keycode that carries it, which is the only
// layout-correct way to know that Shift has to be held. Asking isupper() instead would be right
// for letters on a US layout and wrong for everything else, and it is exactly the capitals that
// have to be provable here: the field's own handler once rejected any key with a modifier set,
// so a rig that could not press Shift could not have caught it.
static int keysymNeedsShift(KeySym sym, KeyCode code)
{
    int perCode = 0;
    KeySym *map = XGetKeyboardMapping(display, code, 1, &perCode);
    if (!map)
        return 0;
    int shifted = 0;
    for (int level = 0; level < perCode; ++level) {
        if (map[level] == sym) {
            shifted = (level % 2) == 1; // levels alternate unshifted, shifted
            break;
        }
    }
    XFree(map);
    return shifted;
}

// One key, by keysym. Focus is set to the target window first: XTEST delivers to whatever has
// the input focus, and without this the keys land wherever the desktop last put it - which looks
// exactly like a plug-in that ignored them.
static void pressKeysym(KeySym sym)
{
    const KeyCode code = XKeysymToKeycode(display, sym);
    if (code == 0)
        return;
    const int shift = keysymNeedsShift(sym, code);
    const KeyCode shiftCode = shift ? XKeysymToKeycode(display, XK_Shift_L) : 0;
    if (shiftCode)
        XTestFakeKeyEvent(display, shiftCode, True, CurrentTime);
    XTestFakeKeyEvent(display, code, True, CurrentTime);
    XTestFakeKeyEvent(display, code, False, CurrentTime);
    if (shiftCode)
        XTestFakeKeyEvent(display, shiftCode, False, CurrentTime);
    XFlush(display);
    usleep(30000);
}

static void typeString(const char *text)
{
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XFlush(display);
    usleep(100000);
    for (const char *p = text; *p; ++p) {
        // ASCII only, which is all the field being driven accepts. A Latin-1 keysym IS the
        // character's own code for this range, so no table is needed.
        if (*p == ' ')
            pressKeysym(XK_space);
        else
            pressKeysym((KeySym)(unsigned char)*p);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: editor-drive <click|drag> <window-id> <x> <y> [dx dy]\n"
                        "       editor-drive wheel <window-id> <x> <y> <clicks>\n"
                        "       editor-drive resize <window-id> <w> <h>\n"
                        "       editor-drive <type|key> <window-id> <text|keysym-name>\n");
        return 2;
    }
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "editor-drive: cannot open display\n");
        return 1;
    }
    window = (Window)strtoul(argv[2], NULL, 0);

    if (strcmp(argv[1], "type") == 0) {
        typeString(argv[3]);
        XCloseDisplay(display);
        return 0;
    }
    if (strcmp(argv[1], "key") == 0) {
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        XFlush(display);
        usleep(100000);
        const KeySym sym = XStringToKeysym(argv[3]);
        if (sym == NoSymbol) {
            fprintf(stderr, "editor-drive: unknown keysym '%s'\n", argv[3]);
            XCloseDisplay(display);
            return 2;
        }
        pressKeysym(sym);
        XCloseDisplay(display);
        return 0;
    }

    if (strcmp(argv[1], "resize") == 0) {
        if (argc < 5) {
            fprintf(stderr, "editor-drive: resize needs w and h\n");
            XCloseDisplay(display);
            return 2;
        }
        resizeTopLevel(atoi(argv[3]), atoi(argv[4]));
        XCloseDisplay(display);
        return 0;
    }

    if (argc < 5) {
        fprintf(stderr, "editor-drive: click and drag need x and y\n");
        XCloseDisplay(display);
        return 2;
    }
    const int x = atoi(argv[3]);
    const int y = atoi(argv[4]);

    if (strcmp(argv[1], "wheel") == 0) {
        if (argc < 6) {
            fprintf(stderr, "editor-drive: wheel needs a click count\n");
            XCloseDisplay(display);
            return 2;
        }
        moveTo(x, y);
        usleep(50000);
        wheel(atoi(argv[5]));
        XCloseDisplay(display);
        return 0;
    }

    if (strcmp(argv[1], "rclick") == 0) {
        button(1, 3);
        button(0, 3);
    } else if (strcmp(argv[1], "click") == 0) {
        moveTo(x, y);
        button(1, 1);
        button(0, 1);
    } else if (strcmp(argv[1], "drag") == 0 && argc >= 7) {
        // Twenty steps with a real pause between them, because the editor coalesces motion and
        // repaints on a timer: a drag delivered as one jump exercises the hit test and nothing
        // else, and would not show a readout that is only drawn while the button is held.
        const int dx = atoi(argv[5]);
        const int dy = atoi(argv[6]);
        moveTo(x, y);
        button(1, 1);
        for (int i = 1; i <= 20; ++i) {
            moveTo(x + dx * i / 20, y + dy * i / 20);
            usleep(60000);
        }
        button(0, 1);
    } else {
        fprintf(stderr, "editor-drive: unknown command '%s'\n", argv[1]);
        XCloseDisplay(display);
        return 2;
    }

    XCloseDisplay(display);
    return 0;
}
