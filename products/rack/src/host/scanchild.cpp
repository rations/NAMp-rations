// Scan-child mode. See scanchild.h.

#include "scanchild.h"

#include "catalog.h"
#include "format.h"
#include "hostapp.h"
#include "scancache.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace NAMp::host
{

namespace
{

// Set once from main() before anything else runs, read afterwards by findScanHelper(). Single
// assignment on the process's first thread, so no synchronisation is needed or wanted.
bool gSelfScanAvailable = false;

//------------------------------------------------------------------------
// The child process: probe ONE plug-in, print what it contains, exit.
//
// This function exists to be crashed. Everything it does is deliberately minimal — no window, no
// audio, no state file — because whatever it touches, a hostile plug-in gets to touch too.
int scanChildMain(int argc, char *argv[])
{
    // argv is <argv0> <flag> <FORMAT> <path>.
    if (argc != 4) {
        std::fprintf(stderr, "%s %s: expected <FORMAT> <path>\n", argv[0], kScanChildFlag);
        return 2;
    }

    PluginFormat format = PluginFormat::Vst3;
    if (!formatFromTag(argv[2], format)) {
        std::fprintf(stderr, "namp-rack scan: unknown format '%s'\n", argv[2]);
        return 2;
    }

    const std::string path = argv[3];
    if (!pathIsSafe(path)) {
        std::fprintf(stderr, "namp-rack scan: refusing unsafe path\n");
        return 2;
    }

    if (format != PluginFormat::Vst3) {
        // LV2 is scanned in process by the parent (Turtle only, no plug-in code runs), and VST2
        // has no backend yet. Neither should ever reach the child.
        std::fprintf(stderr, "namp-rack scan: %s is not scanned out of process\n", argv[2]);
        return 2;
    }

    // A plug-in is entitled to a host context even while merely being described: some build their
    // class list against it.
    installHostApp();

    for (const auto &line : describeVst3Bundle(path))
        std::printf("%s\n", line.c_str());

    std::fflush(stdout);
    uninstallHostApp();
    return 0;
}

} // namespace

//------------------------------------------------------------------------
const char *const kScanChildFlag = "--namp-rack-scan-child";

//------------------------------------------------------------------------
bool runScanChildIfRequested(int argc, char *argv[], int &exitCode)
{
    if (argc >= 2 && argv[1] && std::strcmp(argv[1], kScanChildFlag) == 0) {
        exitCode = scanChildMain(argc, argv);
        return true;
    }

    gSelfScanAvailable = true;
    return false;
}

//------------------------------------------------------------------------
bool selfScanAvailable()
{
    return gSelfScanAvailable;
}

} // namespace NAMp::host
