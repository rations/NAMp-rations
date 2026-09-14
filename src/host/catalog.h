// Catalog — what plug-ins are installed, discovered once and remembered.
//
// Scanning is split by risk, not by convenience:
//
//   * VST3 is scanned OUT OF PROCESS. Loading a .vst3 runs arbitrary third-party code, including
//     static initialisers, before anything has had a chance to validate it. A plug-in that crashes
//     or hangs during discovery must not take the rack with it, so each bundle is probed by a
//     separate, expendable process under a deadline and an output cap.
//
//   * LV2 will be scanned IN PROCESS when that backend lands. Discovery there parses Turtle only;
//     no plug-in binary is opened, so there is nothing to isolate and a subprocess per bundle would
//     be pure cost.
//
// That expendable process is normally THIS binary re-exec'd in scan-child mode, so the standalone
// is one file rather than two that have to travel together; an external helper can be supplied
// instead for the case that cannot re-exec itself. Both are described in scanchild.h.
//
// PAYLOAD FORMAT — one line per audio-effect class, written by the helper on stdout and stored
// verbatim in the cache:
//
//   <FORMAT>\t<key-suffix>\t<name>\t<category>
//
// The path is NOT in the payload; it is already the cache row's first field, and keeping it out
// means a bundle that moves invalidates cleanly instead of carrying a stale path inside its own
// record. `key-suffix` is whatever the format needs beyond the path — for VST3, the 32-hex class
// uid. Catalog::rescan() recombines the two into a PluginRef.

#pragma once

#include "pluginpaths.h"
#include "pluginref.h"
#include "scancache.h"

#include <functional>
#include <string>
#include <vector>

namespace NAMp::host
{

//------------------------------------------------------------------------
// Every .vst3 bundle worth probing.
//
// Note that Module::getModulePaths() already returns BUNDLES, not search roots — it walks
// $HOME/.vst3, /usr/lib/vst3, /usr/local/lib/vst3 and the application directory itself and hands
// back the .vst3 paths it found. Treating its result as a set of roots to search finds nothing,
// because the only thing inside a bundle is the bundle's own contents.
//
// $VST3_PATH is this host's own addition and not part of the VST3 specification. It IS a list of
// roots, so
// those are walked, which is what lets a build tree be scanned without installing into ~/.vst3.
//
// `extraRoots` are the user's own folders (PluginPaths). They are walked the same way $VST3_PATH is
// and are strictly additive: with none, this returns exactly what it always did.
std::vector<std::string> vst3BundlePaths(const std::vector<std::string> &extraRoots = {});

//------------------------------------------------------------------------
// The directories discovery reaches with no configuration at all, for the interface to show beside
// the user's own folders.
//
// MEASURED, NOT DECLARED: these are the parent directories of the bundles that were actually found,
// not a list of where plug-ins are conventionally installed. A hardcoded list would be a claim
// about someone else's machine — lilv's built-in default, for one, is baked in at ITS build time
// and differs between distributions — and a wrong row here would send a user hunting for a folder
// this host never looked in.
std::vector<std::string> automaticVst3Roots();
std::vector<std::string> automaticLv2Roots();

//------------------------------------------------------------------------
// Describe one VST3 bundle by actually loading it. THIS RUNS THIRD-PARTY CODE — it is what the
// helper process calls, and it is not called in the rack process during a scan.
// Returns one payload line per audio-effect class; an empty result means "nothing here", which is
// a legitimate answer worth caching.
std::vector<std::string> describeVst3Bundle(const std::string &bundlePath);

//------------------------------------------------------------------------
struct ScanProgress {
    int index = 0;
    int total = 0;
    std::string current;
};

//------------------------------------------------------------------------
class Catalog
{
public:
    // How to spawn a probe: the command and its leading arguments, ready for the bundle's format
    // tag and path to be appended. Resolution order:
    //
    //   1. $NAMPRACK_SCAN_HELPER, for the case that cannot re-exec itself (this host library
    //      linked into a plug-in bundle, where /proc/self/exe is the DAW);
    //   2. this binary, re-exec'd with the scan-child flag, when main() installed that mode;
    //
    // Empty when neither resolves, in which case rescan() falls back to scanning in process and
    // says so once.
    static std::vector<std::string> findScanHelper();

    // The user's extra search roots. Not owned; must outlive this, and is read at each rescan so a
    // folder added in the interface takes effect on the next scan without rebuilding anything.
    // Null (the default) means "the standard locations only".
    void setSearchPaths(const PluginPaths *paths)
    {
        mPaths = paths;
    }

    // Rebuild the catalogue. Uses the cache for anything whose modification time is unchanged, so a
    // warm rescan opens no plug-in at all. `onProgress` is called on the scanning thread and must
    // not block.
    void rescan(const std::function<void(const ScanProgress &)> &onProgress = {});

    const std::vector<PluginDesc> &entries() const
    {
        return mEntries;
    }

    // Cache file in use; empty when no cache directory could be resolved.
    const std::string &cachePath() const
    {
        return mCachePath;
    }

    // How many bundles were probed by loading them, as opposed to answered from the cache. Zero on
    // a warm rescan is the point of the cache and is what the phase gate checks.
    int probedCount() const
    {
        return mProbed;
    }

    // How many entries this scan had never seen before — the count of entries() carrying
    // freshlyFound. Zero on the very first run, by design: with no baseline to compare against,
    // "new" would mean "everything installed on this machine", which tells the user nothing and
    // buries the case the flag exists for.
    int newCount() const
    {
        return mNewCount;
    }

    // Where the identity baseline lives: $XDG_CACHE_HOME/NAMp-Rack/pluginindex. Beside the scan
    // cache rather than the configuration, because like the cache it is derived state — losing it
    // costs one scan's worth of "new" markers and nothing else.
    static std::string defaultIndexPath();

private:
    // Reads the baseline, marks everything absent from it, and writes the new baseline. Called at
    // the end of rescan(), once the catalogue is complete.
    void diffAgainstIndex();

    const PluginPaths *mPaths = nullptr;
    int mNewCount = 0;
    // Runs the probe command against one bundle. Returns false if it crashed, hung or overran,
    // which the caller records as a negative result so it is not retried every launch.
    bool scanViaHelper(const std::vector<std::string> &helper, const std::string &bundlePath,
                       std::vector<std::string> &lines) const;

    std::vector<PluginDesc> mEntries;
    ScanCache mCache;
    std::string mCachePath;
    int mProbed = 0;
};

//------------------------------------------------------------------------
// Parse one payload line into a PluginDesc, given the path it was found at. Returns false for
// anything malformed — the cache is untrusted input.
bool parsePayload(const std::string &path, const std::string &payload, PluginDesc &out);

} // namespace NAMp::host
