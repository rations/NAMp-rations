// Lv2World implementation. See lv2world.h for the ownership and threading rules.

#include "lv2world.h"
#include "format.h"
#include "chainmodel.h"

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/event/event.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/port-props/port-props.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/state/state.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace NAMp::host
{

namespace
{

// The same bounds the VST3 walk uses, for the same reason: a search root is untrusted input and a
// deep or looping tree must not turn a scan into a hang.
constexpr int kMaxLv2Bundles = 4096;
constexpr int kMaxLv2WalkDepth = 8;

//------------------------------------------------------------------------
// Every LV2 bundle under `root`. A bundle is a directory carrying a manifest — checking for the
// manifest rather than trusting the ".lv2" suffix means a directory merely NAMED that way is
// skipped instead of being handed to lilv to reject.
void collectLv2Bundles(const std::string &root, int depth, std::vector<std::string> &out)
{
    if (depth <= 0 || static_cast<int>(out.size()) >= kMaxLv2Bundles)
        return;

    std::error_code ec;
    std::filesystem::directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
        return;

    for (const auto &entry : it) {
        if (static_cast<int>(out.size()) >= kMaxLv2Bundles)
            return;

        std::error_code e;
        const auto st = entry.symlink_status(e);
        if (e || std::filesystem::is_symlink(st) || !std::filesystem::is_directory(st))
            continue;

        const std::string path = entry.path().string();
        if (std::filesystem::exists(entry.path() / "manifest.ttl", e) && !e) {
            out.push_back(path);
            continue; // a bundle is not searched for bundles nested inside it
        }
        collectLv2Bundles(path, depth - 1, out);
    }
}

// Every feature this host actually implements. A plug-in that requires anything not on this list is
// left out of the catalogue with a reason, because instantiating it would produce something that
// looks loaded and does not work.
//
// urid:map and urid:unmap come from Lv2UridMap; worker:schedule from Lv2Worker; the buf-size
// promises are all true because ChainEngine drives every node at a fixed, pre-negotiated block size
// that is a power of two (see chainengine.h) — they are claims this host can actually keep, which
// is why they are listed rather than the more generous set a host is tempted to claim.
//
// THE LAST TWO ARE A DIFFERENT KIND OF THING and are listed for a different reason. Every feature
// above is data the host hands the plug-in; lv2:isLive and lv2:inPlaceBroken are declarations the
// PLUG-IN makes about itself, which a host answers by being the right shape rather than by
// supplying anything. jalv, the reference for how a minimal LV2 host is built, returns true for
// exactly these two ahead of its own feature list, and both are true here:
//
//   * isLive says the plug-in has a real-time dependency. This host runs every node inside a live
//     audio callback and caches nothing, which is the condition.
//   * inPlaceBroken says the plug-in needs input and output at separate addresses. The LV2 backend
//     already reads this — from the required-feature list AND from the plain-property form — and
//     answers prefersInPlace() == false, at which point the chain compiler gives the node its own
//     output slot. Refusing a plug-in for a promise the backend keeps forty lines away was simply
//     an inconsistency, and it cost real plug-ins: fomp's reverb requires isLive and was dropped
//     from the catalogue by a host that could run it.
const char *const kSupportedFeatures[] = {
    LV2_URID__map,
    LV2_URID__unmap,
    LV2_WORKER__schedule,
    LV2_OPTIONS__options,
    LV2_BUF_SIZE__boundedBlockLength,
    LV2_BUF_SIZE__fixedBlockLength,
    LV2_BUF_SIZE__powerOf2BlockLength,
    LV2_STATE__loadDefaultState,
    LV2_STATE__threadSafeRestore,
    LV2_CORE__isLive,
    LV2_CORE__inPlaceBroken,
};

bool isSupportedFeature(const char *uri)
{
    if (!uri)
        return false;
    for (const char *supported : kSupportedFeatures) {
        if (std::strcmp(uri, supported) == 0)
            return true;
    }
    return false;
}

// A catalogue field must not contain a tab or a newline: the payload format is tab-separated and
// one line per class. A plug-in's own metadata is untrusted input like any other file on disk.
std::string sanitiseField(const char *text)
{
    std::string out;
    if (!text)
        return out;
    for (const char *p = text; *p && out.size() < 256; ++p)
        out.push_back((*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p);
    return out;
}

} // namespace

//------------------------------------------------------------------------
bool lv2NodesContain(const LilvNodes *list, const LilvNode *value)
{
    if (!list || !value)
        return false;
    LILV_FOREACH(nodes, i, list)
    {
        if (lilv_node_equals(lilv_nodes_get(list, i), value))
            return true;
    }
    return false;
}

//------------------------------------------------------------------------
// Never destroyed, and the destructor is deleted so nothing can try. Every LilvPlugin, LilvPort and
// LilvNode a loaded plug-in still refers to is owned by this world; freeing it while any LV2 node
// is alive would invalidate all of them at once, and static destruction order across a process full
// of third-party shared objects gives no way to guarantee it happens last. The world is a few
// megabytes of RDF held for the life of the process, which is what every LV2 host does.
Lv2World *Lv2World::instance()
{
    static Lv2World *const sWorld = []() -> Lv2World * {
        Lv2World *w = new Lv2World();
        if (!w->mWorld) {
            std::fprintf(stderr, "namp-rack: lilv could not be initialised; LV2 plug-ins are "
                                 "unavailable in this session\n");
        }
        return w;
    }();
    return sWorld->mWorld ? sWorld : nullptr;
}

//------------------------------------------------------------------------
Lv2World::Lv2World()
{
    mWorld = lilv_world_new();
    if (!mWorld)
        return;

    // Reads LV2_PATH when set, and the standard search path otherwise. This is the expensive call —
    // it parses the Turtle of every bundle it finds — and it is why there is exactly one world.
    lilv_world_load_all(mWorld);
    mPlugins = lilv_world_get_all_plugins(mWorld);

    cacheNodes();
}

//------------------------------------------------------------------------
void Lv2World::cacheNodes()
{
    auto uri = [this](const char *u) { return lilv_new_uri(mWorld, u); };

    mNodes.audioPort = uri(LV2_CORE__AudioPort);
    mNodes.controlPort = uri(LV2_CORE__ControlPort);
    mNodes.cvPort = uri(LV2_CORE__CVPort);
    mNodes.atomPort = uri(LV2_ATOM__AtomPort);
    mNodes.eventPort = uri(LV2_EVENT__EventPort);
    mNodes.inputPort = uri(LV2_CORE__InputPort);
    mNodes.outputPort = uri(LV2_CORE__OutputPort);

    mNodes.toggled = uri(LV2_CORE__toggled);
    mNodes.integer = uri(LV2_CORE__integer);
    mNodes.enumeration = uri(LV2_CORE__enumeration);
    mNodes.sampleRate = uri(LV2_CORE__sampleRate);
    mNodes.connectionOptional = uri(LV2_CORE__connectionOptional);
    mNodes.notOnGui = uri(LV2_PORT_PROPS__notOnGUI);
    mNodes.reportsLatency = uri(LV2_CORE__reportsLatency);
    mNodes.designation = uri(LV2_CORE__designation);
    mNodes.latency = uri(LV2_CORE__latency);
    mNodes.enabled = uri(LV2_CORE__enabled);

    mNodes.atomSupports = uri(LV2_ATOM__supports);
    mNodes.midiEvent = uri(LV2_MIDI__MidiEvent);
    mNodes.atomChunk = uri(LV2_ATOM__Chunk);
    mNodes.atomSequence = uri(LV2_ATOM__Sequence);
    mNodes.bufferType = uri(LV2_ATOM__bufferType);
    mNodes.rszMinimumSize = uri(LV2_RESIZE_PORT__minimumSize);

    mNodes.uridMap = uri(LV2_URID__map);
    mNodes.uridUnmap = uri(LV2_URID__unmap);
    mNodes.workerSchedule = uri(LV2_WORKER__schedule);
    mNodes.workerInterface = uri(LV2_WORKER__interface);
    mNodes.optionsInterface = uri(LV2_OPTIONS__interface);
    mNodes.boundedBlockLength = uri(LV2_BUF_SIZE__boundedBlockLength);
    mNodes.fixedBlockLength = uri(LV2_BUF_SIZE__fixedBlockLength);
    mNodes.powerOf2BlockLength = uri(LV2_BUF_SIZE__powerOf2BlockLength);
    mNodes.coarseBlockLength = uri(LV2_BUF_SIZE__coarseBlockLength);
    mNodes.stateLoadDefaultState = uri(LV2_STATE__loadDefaultState);
    mNodes.stateThreadSafeRestore = uri(LV2_STATE__threadSafeRestore);
    mNodes.isLive = uri(LV2_CORE__isLive);
    mNodes.inPlaceBroken = uri(LV2_CORE__inPlaceBroken);
    mNodes.hardRtCapable = uri(LV2_CORE__hardRTCapable);

    mNodes.uiX11 = uri(LV2_UI__X11UI);
    mNodes.uiShowInterface = uri(LV2_UI__showInterface);
    mNodes.uiIdleInterface = uri(LV2_UI__idleInterface);
    mNodes.uiParent = uri(LV2_UI__parent);
    mNodes.uiFixedSize = uri(LV2_UI__fixedSize);
    mNodes.uiNoUserResize = uri(LV2_UI__noUserResize);

    mNodes.rdfsLabel = uri("http://www.w3.org/2000/01/rdf-schema#label");
}

//------------------------------------------------------------------------
std::string Lv2World::unsupportedReason(const LilvPlugin *plugin) const
{
    if (!plugin)
        return "not installed";

    // A required feature we do not implement means the plug-in is entitled to misbehave if
    // instantiated anyway, so it is refused by name.
    if (LilvNodes *required = lilv_plugin_get_required_features(plugin)) {
        std::string missing;
        LILV_FOREACH(nodes, i, required)
        {
            const char *uri = lilv_node_as_uri(lilv_nodes_get(required, i));
            if (!isSupportedFeature(uri)) {
                missing = uri ? uri : "an unnamed feature";
                break;
            }
        }
        lilv_nodes_free(required);
        if (!missing.empty())
            return "requires " + missing;
    }

    // Ports decide the rest. A CV port carries a per-sample control signal that this chain has no
    // way to produce, and a plug-in with no audio at all is not a pedal.
    const uint32_t portCount = lilv_plugin_get_num_ports(plugin);
    int audioIn = 0;
    int audioOut = 0;
    for (uint32_t i = 0; i < portCount; ++i) {
        const LilvPort *port = lilv_plugin_get_port_by_index(plugin, i);
        if (!port)
            return "has a port lilv cannot describe";

        const bool isInput = lilv_port_is_a(plugin, port, mNodes.inputPort);
        const bool optional = lilv_port_has_property(plugin, port, mNodes.connectionOptional);

        if (lilv_port_is_a(plugin, port, mNodes.audioPort)) {
            isInput ? ++audioIn : ++audioOut;
        } else if (lilv_port_is_a(plugin, port, mNodes.cvPort)) {
            if (!optional)
                return "needs a CV port, which a pedal chain cannot drive";
        } else if (lilv_port_is_a(plugin, port, mNodes.eventPort)) {
            // The deprecated pre-atom event extension. Nothing modern uses it, and implementing a
            // second event ABI to host something nothing ships would be pure cost.
            if (!optional)
                return "uses the deprecated LV2 event extension";
        } else if (!lilv_port_is_a(plugin, port, mNodes.controlPort) &&
                   !lilv_port_is_a(plugin, port, mNodes.atomPort)) {
            if (!optional)
                return "has a port type this host does not implement";
        }
    }

    if (audioIn == 0 || audioOut == 0)
        return "is not an audio effect (it has no audio input or no audio output)";
    if (audioIn > kMaxChainChannels || audioOut > kMaxChainChannels)
        return "needs more than two channels";

    return {};
}

//------------------------------------------------------------------------
const LilvPlugin *Lv2World::pluginByUri(const std::string &uri)
{
    if (!mPlugins || uri.empty())
        return nullptr;

    std::lock_guard<std::mutex> guard(mLock);
    LilvNode *node = lilv_new_uri(mWorld, uri.c_str());
    if (!node)
        return nullptr;
    const LilvPlugin *plugin = lilv_plugins_get_by_uri(mPlugins, node);
    lilv_node_free(node);
    return plugin;
}

//------------------------------------------------------------------------
std::vector<PluginDesc> Lv2World::describePlugins()
{
    std::vector<PluginDesc> out;
    if (!mPlugins)
        return out;

    std::lock_guard<std::mutex> guard(mLock);

    LILV_FOREACH(plugins, i, mPlugins)
    {
        const LilvPlugin *plugin = lilv_plugins_get(mPlugins, i);
        if (!plugin)
            continue;

        const LilvNode *uriNode = lilv_plugin_get_uri(plugin);
        const char *uri = uriNode ? lilv_node_as_uri(uriNode) : nullptr;
        if (!uri || !uri[0])
            continue;

        if (!unsupportedReason(plugin).empty())
            continue;

        // A plug-in this session loaded from a folder the user has since removed. It stays in the
        // world (see loadRoots) but must stop being offered, or removing a folder would look like
        // it had done nothing until the next launch.
        if (isFromRemovedRoot(plugin))
            continue;

        PluginDesc desc;
        desc.ref.format = PluginFormat::Lv2;
        desc.ref.key = uri;

        if (LilvNode *name = lilv_plugin_get_name(plugin)) {
            desc.name = sanitiseField(lilv_node_as_string(name));
            lilv_node_free(name);
        }
        if (desc.name.empty())
            desc.name = uri;

        if (const LilvPluginClass *cls = lilv_plugin_get_class(plugin)) {
            if (const LilvNode *label = lilv_plugin_class_get_label(cls))
                desc.category = sanitiseField(lilv_node_as_string(label));
        }
        if (LilvNode *author = lilv_plugin_get_author_name(plugin)) {
            desc.vendor = sanitiseField(lilv_node_as_string(author));
            lilv_node_free(author);
        }

        out.push_back(std::move(desc));
    }

    return out;
}

//------------------------------------------------------------------------
int Lv2World::loadRoots(const std::vector<std::string> &roots)
{
    if (!mWorld)
        return 0;

    std::lock_guard<std::mutex> guard(mLock);

    // Which extra roots are live RIGHT NOW, for the filter in describePlugins(). A root the user
    // has taken away has to stop contributing to the catalogue in the same session, and lilv gives
    // no safe way to take a bundle back out from under a plug-in that may be playing: unloading it
    // would free the LilvPlugin a live node is still holding. Filtering costs nothing and cannot
    // reach into a running chain.
    mActiveRoots = roots;

    if (roots.empty())
        return 0;

    const unsigned before = mPlugins ? lilv_plugins_size(mPlugins) : 0;

    std::vector<std::string> bundles;
    for (const std::string &root : roots)
        collectLv2Bundles(root, kMaxLv2WalkDepth, bundles);
    for (const std::string &root : roots) {
        if (std::find(mLoadedRoots.begin(), mLoadedRoots.end(), root) == mLoadedRoots.end())
            mLoadedRoots.push_back(root);
    }

    for (const std::string &bundle : bundles) {
        // "with the trailing slash" is lilv's own requirement for a bundle URI (lilv.h), not a
        // convention — a URI without it names the parent directory's resource, not the bundle.
        LilvNode *uri = lilv_new_file_uri(mWorld, nullptr, (bundle + "/").c_str());
        if (!uri)
            continue;
        lilv_world_load_bundle(mWorld, uri);
        lilv_node_free(uri);
    }

    if (!bundles.empty()) {
        // Required after loading bundles explicitly rather than through lilv_world_load_all(),
        // which does both itself — without them a plug-in's class and any specification its data
        // refers to are simply absent from the model.
        lilv_world_load_specifications(mWorld);
        lilv_world_load_plugin_classes(mWorld);
    }

    mPlugins = lilv_world_get_all_plugins(mWorld);
    const unsigned after = mPlugins ? lilv_plugins_size(mPlugins) : 0;
    return after > before ? static_cast<int>(after - before) : 0;
}

//------------------------------------------------------------------------
// Caller holds mLock.
bool Lv2World::isFromRemovedRoot(const LilvPlugin *plugin) const
{
    if (mLoadedRoots.empty())
        return false;

    const LilvNode *bundle = lilv_plugin_get_bundle_uri(plugin);
    if (!bundle)
        return false;
    char *path = lilv_file_uri_parse(lilv_node_as_uri(bundle), nullptr);
    if (!path)
        return false;
    const std::string dir(path);
    lilv_free(path);

    auto under = [&dir](const std::string &root) {
        return dir.size() >= root.size() && dir.compare(0, root.size(), root) == 0;
    };

    bool fromExtra = false;
    for (const std::string &root : mLoadedRoots)
        fromExtra = fromExtra || under(root);
    if (!fromExtra)
        return false; // it came off the standard path, which the user cannot remove

    for (const std::string &root : mActiveRoots) {
        if (under(root))
            return false;
    }
    return true;
}

//------------------------------------------------------------------------
std::vector<std::string> Lv2World::bundleRoots()
{
    std::vector<std::string> roots;
    if (!mPlugins)
        return roots;

    std::lock_guard<std::mutex> guard(mLock);

    LILV_FOREACH(plugins, i, mPlugins)
    {
        const LilvPlugin *plugin = lilv_plugins_get(mPlugins, i);
        if (!plugin)
            continue;
        if (isFromRemovedRoot(plugin))
            continue;
        const LilvNode *bundle = lilv_plugin_get_bundle_uri(plugin);
        if (!bundle)
            continue;
        char *path = lilv_file_uri_parse(lilv_node_as_uri(bundle), nullptr);
        if (!path)
            continue;
        std::string dir(path);
        lilv_free(path);
        while (dir.size() > 1 && dir.back() == '/')
            dir.pop_back();
        const std::string parent = std::filesystem::path(dir).parent_path().string();
        if (!parent.empty())
            roots.push_back(parent);
    }

    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

//------------------------------------------------------------------------
int loadLv2Roots(const std::vector<std::string> &roots)
{
    Lv2World *world = Lv2World::instance();
    return world ? world->loadRoots(roots) : 0;
}

//------------------------------------------------------------------------
std::vector<std::string> lv2BundleRoots()
{
    Lv2World *world = Lv2World::instance();
    return world ? world->bundleRoots() : std::vector<std::string>();
}

//------------------------------------------------------------------------
std::vector<std::string> describeLv2Plugins()
{
    std::vector<std::string> lines;

    Lv2World *world = Lv2World::instance();
    if (!world)
        return lines;

    // <FORMAT>\t<key-suffix>\t<name>\t<category>, exactly as catalog.h documents. For LV2 the
    // key-suffix IS the whole key: a plug-in URI is globally unique and carries no path, which is
    // why an LV2 chain survives the bundle being moved and a VST3 one does not.
    for (const PluginDesc &desc : world->describePlugins()) {
        std::string line = formatTag(PluginFormat::Lv2);
        line += '\t';
        line += desc.ref.key;
        line += '\t';
        line += desc.name;
        line += '\t';
        line += desc.category;
        lines.push_back(std::move(line));
    }

    return lines;
}

} // namespace NAMp::host
