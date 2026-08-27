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
// Driven by editor-drive.sh, which knows how to find the window in the first place.

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

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

static void button(int down)
{
    XTestFakeButtonEvent(display, 1, down ? True : False, CurrentTime);
    XFlush(display);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: editor-drive <click|drag> <window-id> <x> <y> [dx dy]\n");
        return 2;
    }
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "editor-drive: cannot open display\n");
        return 1;
    }
    window = (Window)strtoul(argv[2], NULL, 0);
    const int x = atoi(argv[3]);
    const int y = atoi(argv[4]);

    if (strcmp(argv[1], "click") == 0) {
        moveTo(x, y);
        button(1);
        button(0);
    } else if (strcmp(argv[1], "drag") == 0 && argc >= 7) {
        // Twenty steps with a real pause between them, because the editor coalesces motion and
        // repaints on a timer: a drag delivered as one jump exercises the hit test and nothing
        // else, and would not show a readout that is only drawn while the button is held.
        const int dx = atoi(argv[5]);
        const int dy = atoi(argv[6]);
        moveTo(x, y);
        button(1);
        for (int i = 1; i <= 20; ++i) {
            moveTo(x + dx * i / 20, y + dy * i / 20);
            usleep(60000);
        }
        button(0);
    } else {
        fprintf(stderr, "editor-drive: unknown command '%s'\n", argv[1]);
        XCloseDisplay(display);
        return 2;
    }

    XCloseDisplay(display);
    return 0;
}
