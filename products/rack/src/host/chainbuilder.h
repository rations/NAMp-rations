// ChainBuilder — the non-real-time half of the rack: what the user is editing.
//
// It owns every hosted plug-in instance, holds the editable list the UI manipulates, compiles that
// list into an immutable RtChain and publishes it to ChainEngine. Every expensive or dangerous
// thing — dlopen, JSON and Turtle parsing, setupProcessing, setActive, state loading, destruction —
// happens here and never on the audio thread.
//
// THREADING. Everything public here runs on ONE owning thread, which for the standalone is the run
// loop. That thread may block: loading a plug-in opens a shared object and runs its static
// initialisers, which takes milliseconds and occasionally much longer. It does not block audio,
// because publication is a single atomic exchange and the audio thread keeps playing the previous
// chain until it adopts the new one.
//
//   MEASURED IN P7, AND DELIBERATELY LEFT HERE. `namp_chaincheck --preset-verify` times a whole
//   preset load, which is the worst case because it is every plug-in at once:
//
//       native VST3 and LV2      12-18 ms per plug-in   (3 plug-ins: 53 ms)
//       Windows VST3 over Wine   ~860 ms per plug-in    (a visible freeze)
//
//   For a native rack that is one dropped frame and a worker thread would be machinery bought for
//   nothing. For a bridged plug-in it is a real stall — but moving instantiation to a worker is not
//   the free fix it looks like: the VST3 SDK talks throughout about "the main thread" for non-RT
//   plug-in calls (ivstdataexchange.h:101 is explicit about it), plug-in static initialisers
//   routinely bring up a GUI toolkit, and a bridge spawns a helper process during load. None of
//   that can be verified for plug-ins that are not installed here, and an unverifiable assumption
//   is flagged rather than adopted on the grounds that other hosts seem to get away with it.
//
//   So it stays on the run loop, and the cost is stated rather than hidden. Changing this needs
//   evidence that main-thread instantiation is not required, not a decision that it probably is
//   not; the change itself is still contained to add()/configure(), which are the only things here
//   that touch a plug-in binary.
//
// DESTRUCTION IS DEFERRED, ALWAYS. A backend removed from the chain is not deleted when it is
// removed: the audio thread may still be running the snapshot that references it. It goes into the
// graveyard tagged with the generation of the snapshot that dropped it, and is destroyed by
// collect() once every older snapshot has come back through the retirement queue. This is the
// generalisation of the plug-in's own bank retirement to N slots.

#pragma once

#include "chainengine.h"
#include "pluginbackend.h"
#include "pluginref.h"

#include <memory>
#include <string>
#include <vector>

namespace NAMp::host
{

//------------------------------------------------------------------------
// Which side of the amp a node sits on. This is not cosmetic: it fixes the node's channel count for
// the life of the instance, because the pre-section is mono and the post-section is stereo.
enum class ChainSection {
    Pre,
    Post,
};

//------------------------------------------------------------------------
// The dry-path delay line one node needs to blend wet against dry without comb-filtering. Allocated
// off the audio thread, read and advanced on it, and retired with the instance it belongs to.
struct DryDelay {
    std::vector<float> ring; // channels * len, channel-major
    int32_t len = 0;         // per channel
    int32_t pos = 0;         // write cursor; the audio thread's while a snapshot names it
};

//------------------------------------------------------------------------
// One node as the UI sees it.
struct ChainNodeInfo {
    // Stable for the life of the instance and unique across both sections. An index is not: it
    // shifts under every move() and remove(), so anything that remembers a node across an edit —
    // the rack's card positions, a selection, an open editor window — has to key on this instead.
    // Never reused, so a stale id resolves to "gone" rather than to whatever took its place.
    uint64_t id = 0;
    PluginRef ref;
    std::string name;
    bool enabled = true;
    float mix = 1.0f;
    uint32_t latency = 0;
    bool placeholder = false; // the plug-in could not be loaded; audio passes through
};

//------------------------------------------------------------------------
class ChainBuilder
{
public:
    ChainBuilder() = default;
    ~ChainBuilder();

    ChainBuilder(const ChainBuilder &) = delete;
    ChainBuilder &operator=(const ChainBuilder &) = delete;

    // The engine this builder publishes to. Must outlive the builder.
    void setEngine(ChainEngine *engine)
    {
        mEngine = engine;
    }

    // Sample rate and maximum block every node is configured for. Changing either tears down and
    // rebuilds nothing: it re-prepares each existing node in place, so it MUST be called with the
    // audio thread suspended (the standalone's suspendProcessing()/resumeProcessing() handshake).
    // Publishes the recompiled chain itself.
    bool configure(double sampleRate, int32_t maxBlock);

    //--- editing (owning thread) ----------------------------------------
    // Loads the plug-in, configures it for its section and appends it. Returns the new index, or -1
    // with `error` set. Does not publish; call publish() when a batch of edits is done.
    int add(ChainSection section, const PluginRef &ref, std::string &error);

    // Appends an instance the caller has already created, rather than one this builder loaded. The
    // instance is prepared and activated here; ownership transfers on success. Used by the test
    // tools, which build synthetic backends that no file could produce.
    int adopt(ChainSection section, std::unique_ptr<PluginBackend> backend, const PluginRef &ref,
              std::string &error);

    // Restore one node's opaque state. This is the entry point a preset load uses, and it exists
    // as a method rather than as three calls at the call site because the ordering is not
    // negotiable and getting it wrong fails silently:
    //
    //   * AFTER prepare(), never before. An LV2 plug-in has no instance at all until it is
    //     prepared — lilv_instance_instantiate happens there — so state handed to it earlier is
    //     dropped and the node comes up at its defaults. That is exactly what a first attempt at
    //     this did, and the only symptom was a preset that sounded wrong.
    //   * BEFORE the node is published, which is why this must be called between add() and
    //     publish(). See the threading note in pluginbackend.h.
    //   * Deactivated across the load, because a plug-in restoring state is entitled to reallocate
    //     and reset, and doing that under an active instance is the hazard the plan's risk 5 names.
    //   * And the dry-delay line is re-sized afterwards: a plug-in's reported latency can depend on
    //     what its state just set.
    //
    // `stateDir` may be null; see PluginBackend::stateSetDirectory.
    bool loadNodeState(ChainSection section, int index, const uint8_t *data, size_t len,
                       const char *stateDir);

    // A node with no instance behind it. This is what a saved chain naming a plug-in that is not
    // installed becomes: the key, the name and (later) the opaque state are all kept, the node is
    // drawn greyed, audio passes straight through it, and it is written back out intact. Dropping
    // it instead — silently destroying a user's chain because they booted without one plug-in —
    // is not acceptable. Never emitted into a published snapshot, so it costs nothing on the audio
    // path.
    int addPlaceholder(ChainSection section, const PluginRef &ref, const std::string &name);

    bool remove(ChainSection section, int index);
    // Moves a node within its section. Crossing sections is deliberately not supported: the channel
    // count is fixed at instantiation, so it is a remove plus an add.
    bool move(ChainSection section, int from, int to);
    bool setEnabled(ChainSection section, int index, bool enabled);
    bool setMix(ChainSection section, int index, float mix);

    int count(ChainSection section) const;
    bool nodeInfo(ChainSection section, int index, ChainNodeInfo &out) const;
    // Where a node with this id is now, or false if it has been removed. The UI's route back from a
    // remembered node to a current index.
    bool findById(uint64_t id, ChainSection &section, int &index) const;
    // The live instance, for the generic panel and the editor windows. Null for a placeholder or an
    // out-of-range index. Never call process() through this.
    PluginBackend *backend(ChainSection section, int index) const;

    // Compile the current list and hand it to the engine. Cheap: no plug-in is touched.
    void publish();

    // Free retired snapshots and any backend whose last snapshot has come back. Call from the
    // owning thread's idle tick. Doing nothing here leaks nothing permanently, it just delays.
    void collect();
    // As collect(), but for a stopped audio thread: frees everything outstanding regardless of
    // whether the engine handed it back. The caller must know the audio thread is not running.
    void collectAll();

    // Tears the whole rack down: deactivates and destroys every instance and publishes an empty
    // chain. The caller must know the audio thread is not running. This exists so shutdown happens
    // at a chosen point — while the plug-in modules are still loaded and the host context is still
    // published — rather than wherever this object's destructor happens to land.
    void clear();

    // Summed latency of the enabled nodes, in samples. The amp's own is not included.
    uint32_t latencySamples() const;

private:
    struct Node {
        uint64_t id = 0;
        std::unique_ptr<PluginBackend> backend;
        PluginRef ref;
        std::string name;
        bool enabled = true;
        float mix = 1.0f;

        // Dry-path delay for wet/dry mixing; see RtNode::dryRing for why it lives with the node
        // rather than in the snapshot. Held behind a unique_ptr rather than inline because a
        // snapshot points at `pos`, and a Node sitting directly in a std::vector is MOVED by every
        // push_back, erase and reorder — which would leave the audio thread advancing a cursor that
        // had walked off to a different address. A separately allocated object does not move when
        // the list around it does.
        std::unique_ptr<DryDelay> dry;
    };

    struct Grave {
        PluginBackend *backend = nullptr;
        // Retired alongside the instance: a live snapshot names both, so freeing either early is
        // the same use-after-free.
        DryDelay *dry = nullptr;
        // The newest snapshot generation that can still reference this instance. It is safe to
        // destroy once a LATER generation is live, or once nothing is live at all.
        uint64_t lastReferencedBy = 0;
    };

    std::vector<Node> &list(ChainSection section)
    {
        return section == ChainSection::Pre ? mPre : mPost;
    }
    const std::vector<Node> &list(ChainSection section) const
    {
        return section == ChainSection::Pre ? mPre : mPost;
    }
    static bool indexOk(const std::vector<Node> &nodes, int index)
    {
        return index >= 0 && index < static_cast<int>(nodes.size());
    }
    void bury(std::unique_ptr<PluginBackend> backend, std::unique_ptr<DryDelay> dry);
    // (Re)build one node's dry-path delay for its current reported latency. Call only where the
    // node is not in a published chain. A plug-in whose latency is beyond what is worth
    // compensating gets no delay line and one warning, and its dry path stays undelayed.
    static void sizeDryDelay(Node &node, const ProcessConfig &config);
    // Point one snapshot node at its builder node's delay line, if it needs one.
    static void attachDryDelay(RtNode &rt, const Node &node);

    ChainEngine *mEngine = nullptr;
    ProcessConfig mPreConfig;  // channels == 1
    ProcessConfig mPostConfig; // channels == 2
    bool mConfigured = false;

    std::vector<Node> mPre;
    std::vector<Node> mPost;

    std::vector<Grave> mGraveyard;
    uint64_t mGeneration = 0;
    // Monotonic, never reset and never reused. See ChainNodeInfo::id.
    uint64_t mNextNodeId = 1;
};

//------------------------------------------------------------------------
// Instantiate one plug-in by reference. VST3 today; LV2 and VST2 are P5 and P6 and currently fail
// with a named error rather than being silently dropped.
PluginBackend *createBackend(const PluginRef &ref, std::string &error);

} // namespace NAMp::host
