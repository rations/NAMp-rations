// ChainBuilder implementation. See chainbuilder.h for the ownership and deferred-destruction rules,
// and chainmodel.h for the slot layout the compiler below assigns.

#include "chainbuilder.h"
#include "backend_vst3.h"
#if NAMPRACK_HAVE_LV2
#include "backend_lv2.h"
#endif

#include <cstdio>

namespace NAMp::host
{

//------------------------------------------------------------------------
PluginBackend *createBackend(const PluginRef &ref, std::string &error)
{
    error.clear();
    if (!ref.valid()) {
        error = "invalid plug-in reference";
        return nullptr;
    }

    switch (ref.format) {
        case PluginFormat::Vst3:
            return Vst3Backend::load(ref, error);
        case PluginFormat::Lv2:
#if NAMPRACK_HAVE_LV2
            return Lv2Backend::load(ref, error);
#else
            // LV2 hosting is lilv + suil + jalv's event buffer, and this build has none of them
            // (Windows). The enumerator stays regardless: a rack saved on a machine that DOES
            // host LV2 must come back as a named placeholder that writes itself out again
            // verbatim, not as a node silently dropped from the middle of someone's chain.
            error = "LV2 hosting is not built into this binary";
            return nullptr;
#endif
        case PluginFormat::Vst2:
            error = "VST2 hosting is not implemented yet";
            return nullptr;
        case PluginFormat::Count:
            break;
    }
    error = "unknown plug-in format";
    return nullptr;
}

//------------------------------------------------------------------------
// A pedal that reports more delay than this is not a pedal, and matching it would cost 384 KiB per
// node at 48 kHz stereo for a case that does not occur in a guitar chain. One second is far past
// any linear-phase EQ or look-ahead limiter and still bounds the memory.
void ChainBuilder::sizeDryDelay(Node &node, const ProcessConfig &config)
{
    node.dry.reset();

    if (!node.backend)
        return;
    const uint32_t latency = node.backend->latencySamples();
    if (latency == 0)
        return; // the usual case: no delay line, no memory, nothing on the audio path

    const uint32_t limit = static_cast<uint32_t>(config.sampleRate > 0.0 ? config.sampleRate : 0.0);
    if (latency > limit) {
        std::fprintf(
            stderr,
            "namp-rack: %s reports %u samples of latency, past the %u this host will delay a "
            "dry path by; leave it fully wet\n",
            node.name.c_str(), latency, limit);
        return;
    }

    auto dry = std::make_unique<DryDelay>();
    dry->ring.assign(static_cast<size_t>(latency) * static_cast<size_t>(config.channels), 0.0f);
    dry->len = static_cast<int32_t>(latency);
    node.dry = std::move(dry);
}

//------------------------------------------------------------------------
ChainBuilder::~ChainBuilder()
{
    // The engine must already have been abandoned, or its audio thread stopped; either way nothing
    // outstanding is anyone else's to free.
    collectAll();
}

//------------------------------------------------------------------------
bool ChainBuilder::configure(double sampleRate, int32_t maxBlock)
{
    if (sampleRate <= 0.0 || maxBlock <= 0)
        return false;

    mPreConfig.sampleRate = sampleRate;
    mPreConfig.maxBlock = maxBlock;
    mPreConfig.channels = 1;

    mPostConfig = mPreConfig;
    mPostConfig.channels = kMaxChainChannels;

    mConfigured = true;

    // Re-preparing an already-running plug-in means setupProcessing, which VST3 only permits while
    // the component is inactive. The caller is responsible for having suspended the audio thread;
    // this only handles the plug-in side of it.
    for (const auto section : {ChainSection::Pre, ChainSection::Post}) {
        const ProcessConfig &config = (section == ChainSection::Pre) ? mPreConfig : mPostConfig;
        for (auto &node : list(section)) {
            if (!node.backend)
                continue;
            node.backend->deactivate();
            if (!node.backend->prepare(config)) {
                std::fprintf(stderr, "namp-rack: %s refused %d channels at %.0f Hz / %d frames\n",
                             node.name.c_str(), config.channels, sampleRate, maxBlock);
                continue;
            }
            node.backend->activate();
            // A new sample rate or block size can change what a plug-in reports, and this is one of
            // the two places the audio thread is known to be stopped, so it is one of the two
            // places the ring may move.
            sizeDryDelay(node, config);
        }
    }

    publish();
    return true;
}

//------------------------------------------------------------------------
int ChainBuilder::add(ChainSection section, const PluginRef &ref, std::string &error)
{
    std::unique_ptr<PluginBackend> backend(createBackend(ref, error));
    if (!backend)
        return -1;
    return adopt(section, std::move(backend), ref, error);
}

//------------------------------------------------------------------------
int ChainBuilder::adopt(ChainSection section, std::unique_ptr<PluginBackend> backend,
                        const PluginRef &ref, std::string &error)
{
    error.clear();
    if (!backend) {
        error = "no instance to adopt";
        return -1;
    }
    if (!mConfigured) {
        error = "the chain has no sample rate yet";
        return -1;
    }

    std::vector<Node> &nodes = list(section);
    if (static_cast<int32_t>(nodes.size()) >= kMaxChainNodes) {
        error = "the chain is full";
        return -1;
    }

    const ProcessConfig &config = (section == ChainSection::Pre) ? mPreConfig : mPostConfig;
    if (!backend->prepare(config)) {
        error = "could not configure " + std::string(backend->displayName());
        return -1;
    }
    backend->activate();

    Node node;
    node.id = mNextNodeId++;
    node.name = backend->displayName();
    node.backend = std::move(backend);
    node.ref = ref;
    // Before the node joins the list, so it is provably not reachable from a snapshot yet.
    sizeDryDelay(node, config);
    nodes.push_back(std::move(node));
    return static_cast<int>(nodes.size()) - 1;
}

//------------------------------------------------------------------------
bool ChainBuilder::loadNodeState(ChainSection section, int index, const uint8_t *data, size_t len,
                                 const char *stateDir)
{
    std::vector<Node> &nodes = list(section);
    if (!indexOk(nodes, index) || !data || len == 0)
        return false;

    Node &node = nodes[static_cast<size_t>(index)];
    if (!node.backend)
        return false;

    node.backend->deactivate();
    node.backend->stateSetDirectory(stateDir);
    const bool ok = node.backend->stateLoad(data, len);
    node.backend->stateSetDirectory(nullptr);
    node.backend->activate();

    // The node is not in a published snapshot between add() and publish(), so this is one of the
    // moments the ring may move.
    sizeDryDelay(node, section == ChainSection::Pre ? mPreConfig : mPostConfig);
    return ok;
}

//------------------------------------------------------------------------
int ChainBuilder::addPlaceholder(ChainSection section, const PluginRef &ref,
                                 const std::string &name)
{
    std::vector<Node> &nodes = list(section);
    if (static_cast<int32_t>(nodes.size()) >= kMaxChainNodes)
        return -1;

    Node node;
    node.id = mNextNodeId++;
    node.ref = ref;
    node.name = name.empty() ? std::string("(missing plug-in)") : name;
    nodes.push_back(std::move(node));
    return static_cast<int>(nodes.size()) - 1;
}

//------------------------------------------------------------------------
bool ChainBuilder::remove(ChainSection section, int index)
{
    std::vector<Node> &nodes = list(section);
    if (!indexOk(nodes, index))
        return false;

    // Deactivated here, on this thread, but NOT destroyed: the audio thread may still be running a
    // snapshot that references it.
    if (nodes[static_cast<size_t>(index)].backend)
        nodes[static_cast<size_t>(index)].backend->deactivate();
    bury(std::move(nodes[static_cast<size_t>(index)].backend),
         std::move(nodes[static_cast<size_t>(index)].dry));

    nodes.erase(nodes.begin() + index);
    return true;
}

//------------------------------------------------------------------------
bool ChainBuilder::move(ChainSection section, int from, int to)
{
    std::vector<Node> &nodes = list(section);
    if (!indexOk(nodes, from) || !indexOk(nodes, to) || from == to)
        return false;

    Node moved = std::move(nodes[static_cast<size_t>(from)]);
    nodes.erase(nodes.begin() + from);
    nodes.insert(nodes.begin() + to, std::move(moved));
    return true;
}

//------------------------------------------------------------------------
bool ChainBuilder::setEnabled(ChainSection section, int index, bool enabled)
{
    std::vector<Node> &nodes = list(section);
    if (!indexOk(nodes, index))
        return false;
    nodes[static_cast<size_t>(index)].enabled = enabled;
    return true;
}

//------------------------------------------------------------------------
bool ChainBuilder::setMix(ChainSection section, int index, float mix)
{
    std::vector<Node> &nodes = list(section);
    if (!indexOk(nodes, index))
        return false;
    if (mix < 0.0f)
        mix = 0.0f;
    if (mix > 1.0f)
        mix = 1.0f;
    nodes[static_cast<size_t>(index)].mix = mix;
    return true;
}

//------------------------------------------------------------------------
int ChainBuilder::count(ChainSection section) const
{
    return static_cast<int>(list(section).size());
}

//------------------------------------------------------------------------
bool ChainBuilder::nodeInfo(ChainSection section, int index, ChainNodeInfo &out) const
{
    const std::vector<Node> &nodes = list(section);
    if (!indexOk(nodes, index))
        return false;

    const Node &node = nodes[static_cast<size_t>(index)];
    out.id = node.id;
    out.ref = node.ref;
    out.name = node.name;
    out.enabled = node.enabled;
    out.mix = node.mix;
    out.latency = node.backend ? node.backend->latencySamples() : 0;
    out.placeholder = (node.backend == nullptr);
    return true;
}

//------------------------------------------------------------------------
bool ChainBuilder::findById(uint64_t id, ChainSection &section, int &index) const
{
    if (id == 0)
        return false;
    for (const auto s : {ChainSection::Pre, ChainSection::Post}) {
        const std::vector<Node> &nodes = list(s);
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].id == id) {
                section = s;
                index = static_cast<int>(i);
                return true;
            }
        }
    }
    return false;
}

//------------------------------------------------------------------------
PluginBackend *ChainBuilder::backend(ChainSection section, int index) const
{
    const std::vector<Node> &nodes = list(section);
    if (!indexOk(nodes, index))
        return nullptr;
    return nodes[static_cast<size_t>(index)].backend.get();
}

//------------------------------------------------------------------------
uint32_t ChainBuilder::latencySamples() const
{
    uint32_t total = 0;
    for (const auto *nodes : {&mPre, &mPost}) {
        for (const auto &node : *nodes) {
            if (node.enabled && node.backend)
                total += node.backend->latencySamples();
        }
    }
    return total;
}

//------------------------------------------------------------------------
// A node at full wet needs no dry copy at all, so it gets no delay line either — the ring is left
// unattached rather than run and thrown away. Turning the mix down attaches it on the next publish
// with the ring's history intact, because the ring belongs to the node and not to the snapshot.
void ChainBuilder::attachDryDelay(RtNode &rt, const Node &node)
{
    if (rt.mix >= 1.0f || !node.dry || node.dry->len <= 0)
        return;
    rt.dryRing = node.dry->ring.data();
    rt.dryLen = node.dry->len;
    rt.dryPos = &node.dry->pos;
}

//------------------------------------------------------------------------
void ChainBuilder::publish()
{
    if (!mEngine)
        return;

    auto *chain = new RtChain;
    chain->generation = ++mGeneration;

    // Ping-pong assignment. `cursor` is where the signal currently is; `spare` is the free slot of
    // the pair. A disabled node or a placeholder is simply not emitted, so bypass shortens the
    // chain rather than adding a test to the audio path.
    int8_t cursor = kSlotIn;
    int8_t spare = kSlotPing;
    const auto flip = [](int8_t slot) { return slot == kSlotPing ? kSlotPong : kSlotPing; };

    for (const auto &node : mPre) {
        if (!node.enabled || !node.backend)
            continue;
        RtNode &rt = chain->pre[chain->preCount++];
        rt.backend = node.backend.get();
        rt.inSlot = cursor;
        rt.outSlot = spare;
        rt.channels = 1;
        rt.mix = node.mix;
        attachDryDelay(rt, node);
        cursor = spare;
        spare = flip(spare);
    }
    chain->anchorIn = cursor;

    int32_t postEnabled = 0;
    for (const auto &node : mPost) {
        if (node.enabled && node.backend)
            ++postEnabled;
    }

    if (postEnabled == 0) {
        // Nothing after the amp: it writes JACK's outputs directly, exactly as it does today.
        chain->anchorOut = kSlotOut;
    } else {
        chain->anchorOut = spare;
        cursor = spare;
        spare = flip(spare);

        for (const auto &node : mPost) {
            if (!node.enabled || !node.backend)
                continue;
            RtNode &rt = chain->post[chain->postCount++];
            rt.backend = node.backend.get();
            rt.inSlot = cursor;
            // The last node writes straight into JACK's buffers, so the chain costs no copy at its
            // own end either.
            rt.outSlot = (chain->postCount == postEnabled) ? kSlotOut : spare;
            rt.channels = kMaxChainChannels;
            rt.mix = node.mix;
            attachDryDelay(rt, node);
            cursor = rt.outSlot;
            spare = flip(spare);
        }
    }

    // JACK does not promise a clean output buffer, and a hosted plug-in is not obliged to fill one.
    // When the amp is the last writer this stays false: it always writes every sample.
    chain->clearOutput = (postEnabled > 0);
    // No hosted node means no third-party code touched the signal, so the safety pass has nothing
    // to protect against and an empty rack costs exactly what it does today.
    chain->clampOutput = (chain->preCount > 0 || chain->postCount > 0);
    chain->latency = latencySamples();

    if (RtChain *displaced = mEngine->publish(chain))
        delete displaced; // never adopted: the exchange is atomic, so nobody else can hold it
}

//------------------------------------------------------------------------
void ChainBuilder::collect()
{
    if (!mEngine)
        return;

    while (RtChain *chain = mEngine->takeRetired())
        delete chain;

    const uint64_t live = mEngine->liveGeneration();
    for (size_t i = mGraveyard.size(); i > 0; --i) {
        const Grave &grave = mGraveyard[i - 1];
        // Safe when nothing at all is running, or when the audio thread has moved on to a snapshot
        // newer than any that could name this instance.
        if (live != 0 && live <= grave.lastReferencedBy)
            continue;
        delete grave.backend;
        delete grave.dry;
        mGraveyard.erase(mGraveyard.begin() + static_cast<long>(i) - 1);
    }
}

//------------------------------------------------------------------------
void ChainBuilder::clear()
{
    for (std::vector<Node> *nodes : {&mPre, &mPost}) {
        for (Node &node : *nodes) {
            if (node.backend)
                node.backend->deactivate();
            // Destroyed rather than buried: with the audio thread stopped there is nobody left who
            // could still be inside a published snapshot.
            node.backend.reset();
            node.dry.reset();
        }
        nodes->clear();
    }
    if (mEngine)
        publish();
    collectAll();
}

//------------------------------------------------------------------------
void ChainBuilder::collectAll()
{
    if (mEngine) {
        while (RtChain *chain = mEngine->takeRetired())
            delete chain;
    }
    for (const Grave &grave : mGraveyard) {
        delete grave.backend;
        delete grave.dry;
    }
    mGraveyard.clear();
}

//------------------------------------------------------------------------
void ChainBuilder::bury(std::unique_ptr<PluginBackend> backend, std::unique_ptr<DryDelay> dry)
{
    if (!backend && !dry)
        return;
    Grave grave;
    grave.backend = backend.release();
    grave.dry = dry.release();
    // Every snapshot compiled so far may name it; the next publish() is the first that will not.
    grave.lastReferencedBy = mGeneration;
    mGraveyard.push_back(grave);
}

} // namespace NAMp::host
