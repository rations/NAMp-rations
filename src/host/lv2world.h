// Lv2World — the one LilvWorld in the process, and the vocabulary looked up out of it once.
//
// lilv builds an RDF model of every installed bundle. That model is expensive to build (it parses
// the Turtle of every bundle on the search path) and every LilvPlugin, LilvPort and LilvNode handed
// out of it is owned by it and dies with it, so there is exactly one and it is never torn down
// while a plug-in is loaded.
//
// NOT THREAD SAFE, and lilv does not claim to be. Every call here takes a mutex and every LilvNode
// this class hands back is one of its own cached, immutable ones — so a caller can read a cached
// node's value without holding anything, but may not call into lilv itself off this class's
// methods. Nothing here is ever reached from the audio thread; by the time a node is playing,
// everything it needed from lilv has already been copied into plain C++ members.
//
// WHY LV2 DISCOVERY IS IN PROCESS, unlike VST3. Scanning an LV2 bundle parses Turtle and reads a
// manifest; it does not dlopen anything and does not run one line of plug-in code. There is nothing
// to isolate, so a subprocess per bundle would buy nothing and cost a fork. The binary is opened
// only when a plug-in is actually instantiated, which is in process by design for every format.
//
// AND WHY THERE IS NO SCAN CACHE FOR IT. The VST3 cache exists because loading a bundle to ask its
// name costs a dlopen and arbitrary static initialisers. lilv's world load is one pass over a few
// hundred small Turtle files and is measured in the low hundreds of milliseconds for the whole
// system; caching it would add a staleness question to something that is already fast and always
// right. Measured, not assumed — see the timing printed by `namp-standalone --list-plugins`.

#pragma once

#include "pluginref.h"

#include <lilv/lilv.h>

#include <mutex>
#include <string>
#include <vector>

namespace NAMp::host
{

//------------------------------------------------------------------------
// Every URI this host needs to ask lilv about, resolved once into a LilvNode. Held by value in
// Lv2World; each member is owned by it and freed with it.
struct Lv2Nodes {
    // Port classes.
    LilvNode *audioPort = nullptr;
    LilvNode *controlPort = nullptr;
    LilvNode *cvPort = nullptr;
    LilvNode *atomPort = nullptr;
    LilvNode *eventPort = nullptr;
    LilvNode *inputPort = nullptr;
    LilvNode *outputPort = nullptr;

    // Port properties that change how a control is presented or driven.
    LilvNode *toggled = nullptr;
    LilvNode *integer = nullptr;
    LilvNode *enumeration = nullptr;
    LilvNode *sampleRate = nullptr;
    LilvNode *connectionOptional = nullptr;
    LilvNode *notOnGui = nullptr;
    LilvNode *reportsLatency = nullptr;
    LilvNode *designation = nullptr;
    LilvNode *latency = nullptr;
    LilvNode *enabled = nullptr;

    // Atom port contents, so a port that carries only MIDI can be told from one that carries patch
    // messages — the first is dead weight in a chain that has no MIDI, the second is how a UI talks
    // to its plug-in.
    LilvNode *atomSupports = nullptr;
    LilvNode *midiEvent = nullptr;
    LilvNode *atomChunk = nullptr;
    LilvNode *atomSequence = nullptr;
    LilvNode *bufferType = nullptr;
    LilvNode *rszMinimumSize = nullptr;

    // Features and extensions we implement, so a plug-in requiring something else can be refused
    // with a reason rather than instantiated and left half-working.
    LilvNode *uridMap = nullptr;
    LilvNode *uridUnmap = nullptr;
    LilvNode *workerSchedule = nullptr;
    LilvNode *workerInterface = nullptr;
    LilvNode *optionsInterface = nullptr;
    LilvNode *boundedBlockLength = nullptr;
    LilvNode *fixedBlockLength = nullptr;
    LilvNode *powerOf2BlockLength = nullptr;
    LilvNode *coarseBlockLength = nullptr;
    LilvNode *stateLoadDefaultState = nullptr;
    LilvNode *stateThreadSafeRestore = nullptr;
    LilvNode *isLive = nullptr;
    LilvNode *inPlaceBroken = nullptr;
    LilvNode *hardRtCapable = nullptr;

    // UI types.
    LilvNode *uiX11 = nullptr;
    LilvNode *uiShowInterface = nullptr;
    LilvNode *uiIdleInterface = nullptr;
    LilvNode *uiParent = nullptr;
    LilvNode *uiFixedSize = nullptr;
    LilvNode *uiNoUserResize = nullptr;

    LilvNode *rdfsLabel = nullptr;
};

//------------------------------------------------------------------------
class Lv2World
{
public:
    // Builds the world on first call. Returns null only if lilv itself could not be initialised, in
    // which case LV2 is simply unavailable and the rest of the rack carries on.
    static Lv2World *instance();

    LilvWorld *world() const
    {
        return mWorld;
    }
    const Lv2Nodes &nodes() const
    {
        return mNodes;
    }
    // Held by callers that need to make several lilv calls that must not interleave with another
    // thread's — instantiation, for one.
    std::mutex &lock()
    {
        return mLock;
    }

    // All installed plug-ins, as catalogue rows. Plug-ins that could never work here (no audio
    // ports, a CV port, a required feature we do not implement) are left out, because a picker
    // entry that always fails to load is worse than no entry.
    std::vector<PluginDesc> describePlugins();

    // Find one plug-in by URI. Null when it is not installed, which the chain turns into a
    // placeholder rather than an error.
    const LilvPlugin *pluginByUri(const std::string &uri);

    // Why this plug-in cannot be hosted, or an empty string if it can. Reasons are meant to be
    // shown to a person, so they name the missing thing.
    std::string unsupportedReason(const LilvPlugin *plugin) const;

    // Load every LV2 bundle found under `roots`, IN ADDITION to whatever lilv found on its standard
    // path, and record `roots` as the ones currently in force. Returns how many plug-ins appeared.
    //
    // Call it on every scan, including with an empty list: a root that has been taken away since
    // the last call stops contributing to describePlugins() from that moment, which is the only way
    // removing a folder can take effect without a restart. The bundles themselves are NOT unloaded
    // — lilv would free a LilvPlugin that a node in the running chain is still holding.
    //
    // Bundle by bundle rather than by search path, and that is the whole reason this is safe to
    // call on a running host. lilv's public way to name extra directories is
    // LILV_OPTION_LV2_PATH, which lilv.h documents as an OVERRIDE — "lilv will only look inside the
    // given path" — so using it would mean reproducing lilv's built-in default from the outside and
    // getting it wrong on any distribution that built lilv with a different libdir.
    // lilv_world_load_bundle() adds; it never removes, so every LilvPlugin already handed out stays
    // valid and a plug-in that is playing right now is undisturbed. The plugin list itself is owned
    // by the world (lilv_world_get_all_plugins returns world->plugins), so a re-scan sees the new
    // bundles without anything being rebuilt.
    int loadRoots(const std::vector<std::string> &roots);

    // The directories the standard search actually found bundles in. Measured for the interface to
    // show; see automaticLv2Roots() in catalog.h for why it is not a hardcoded list.
    std::vector<std::string> bundleRoots();

private:
    Lv2World();
    ~Lv2World() = delete; // see the note in the .cpp

    Lv2World(const Lv2World &) = delete;
    Lv2World &operator=(const Lv2World &) = delete;

    void cacheNodes();
    // Caller holds mLock. True for a plug-in that came from an extra root that is no longer in the
    // list — see loadRoots().
    bool isFromRemovedRoot(const LilvPlugin *plugin) const;

    LilvWorld *mWorld = nullptr;
    const LilvPlugins *mPlugins = nullptr;
    Lv2Nodes mNodes;
    std::vector<std::string> mLoadedRoots; // every extra root loaded this session
    std::vector<std::string> mActiveRoots; // ...and the ones still in the user's list
    std::mutex mLock;
};

//------------------------------------------------------------------------
// Convenience for the many "does this node's list contain that URI" questions lilv asks by
// iteration. Returns false for a null list, which lilv returns freely.
bool lv2NodesContain(const LilvNodes *list, const LilvNode *value);

//------------------------------------------------------------------------
// One catalogue payload line per plug-in, in the format catalog.h documents. Used by the in-process
// LV2 scan; kept here so the format lives next to the code that knows what an LV2 key is.
std::vector<std::string> describeLv2Plugins();

//------------------------------------------------------------------------
// Free-function wrappers, so catalog.cpp does not have to reach through the singleton (and does
// nothing at all when lilv failed to initialise).
int loadLv2Roots(const std::vector<std::string> &roots);
std::vector<std::string> lv2BundleRoots();

} // namespace NAMp::host
