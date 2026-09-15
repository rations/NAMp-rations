// Lv2Backend — hosting an LV2 plug-in as one node of the chain.
//
// LV2 is a very different shape from VST3 and the differences are all in this file:
//
//   1. PORTS ARE STATIC AND CONNECTED, NOT PASSED. There is no process() taking buffers. The host
//      points every port at a piece of memory once with lilv_instance_connect_port(), and run(n)
//      reads and writes through those pointers. Connecting is therefore setup work, done in
//      prepare(), and the audio path only ever re-points the two audio buses — which is exactly
//      what the chain's ping-pong slots need, and why an LV2 node costs no copy at all in the
//      common case.
//
//   2. A CONTROL PORT IS ONE FLOAT, not a callback. A parameter edit is a store into the float the
//      plug-in is already reading. That store still goes through a ring rather than straight from
//      the UI thread, because a torn read is not the only hazard: an edit applied halfway through a
//      block is an edit the plug-in cannot smooth. The ring is drained once, at the top of the
//      block.
//
//   3. MONO PLUG-INS ARE INSTANTIATED ONCE PER CHANNEL, NOT RUN TWICE. A mono pedal placed before
//      the amp runs a single instance, because the pre-amp chain is mono — that is the win over the
//      parent project, which instantiates and runs every mono LV2 plug-in twice regardless. Placed
//      AFTER the amp the chain is stereo and a mono plug-in genuinely needs two instances, each
//      with
//      its own filter state; one instance run twice over two channels would smear one channel's
//      tail into the other. So the count is decided by the section, and the parameter ring feeds
//      every instance so the two never drift apart.
//
//   4. ATOM PORTS MUST BE PRESENTED EVEN WITH NO MIDI. Most modern plug-ins have an atom input, and
//      a plug-in whose atom port is connected to null or to a buffer that is not a valid
//      LV2_Atom_Sequence will read garbage or crash. Every atom port therefore gets a real buffer,
//      reset to an empty sequence (input) or a chunk (output) before every run. This is what jalv's
//      lv2_evbuf exists for and why it is vendored.
//
//   5. THE WORKER IS THE PLUG-IN'S ONLY ESCAPE HATCH. See lv2worker.h.
//
// FEATURES ARE PROMISES. Everything in the feature list handed to a plug-in is something this host
// can actually do; bufsz:fixedBlockLength in particular is only true because ChainEngine drives
// every node at one pre-negotiated size. Claiming a feature that is not kept is how a plug-in ends
// up quietly misbehaving with nothing to point at.

#pragma once

#include "pluginbackend.h"
#include "lv2worker.h"
#include "spscbytequeue.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/options/options.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct LV2_Evbuf_Impl;
typedef struct LV2_Evbuf_Impl LV2_Evbuf;

typedef struct SuilHostImpl SuilHost;
typedef struct SuilInstanceImpl SuilInstance;

namespace NAMp::host
{

//------------------------------------------------------------------------
class Lv2Backend final : public PluginBackend
{
public:
    ~Lv2Backend() override;

    // Looks the plug-in up in the shared world and reads its port layout. Does not instantiate —
    // that needs a sample rate and a block size, which is prepare(). Returns null with a reason in
    // `error` when the URI is not installed or the plug-in cannot be hosted.
    static Lv2Backend *load(const PluginRef &ref, std::string &error);

    //--- identity ------------------------------------------------------
    PluginFormat format() const override
    {
        return PluginFormat::Lv2;
    }
    const char *key() const override
    {
        return mKey.c_str();
    }
    const char *displayName() const override
    {
        return mName.c_str();
    }
    const char *category() const override
    {
        return mCategory.c_str();
    }

    //--- lifecycle -----------------------------------------------------
    bool prepare(const ProcessConfig &config) override;
    void activate() override;
    void deactivate() override;
    void reset() override;

    uint32_t latencySamples() const override
    {
        return mLatency.load(std::memory_order_relaxed);
    }
    // True unless the plug-in declares lv2:inPlaceBroken. LV2 is explicit about this, unlike VST3
    // where it has to be assumed false — so an LV2 node can usually be given one slot instead of
    // two. It costs no copies either way; it costs a bus.
    bool prefersInPlace() const override
    {
        return mInPlaceOk;
    }
    // What the NODE carries, not what one instance's ports say. A mono plug-in in the stereo
    // section is two instances and therefore a stereo node; reporting 1 there would describe the
    // binary rather than the thing in the chain, and every caller wants the latter.
    int32_t audioInCount() const override
    {
        return mEffectiveIn;
    }
    int32_t audioOutCount() const override
    {
        return mEffectiveOut;
    }

    //--- real time -----------------------------------------------------
    void process(const AudioBlock &block) noexcept override;

    //--- parameters ----------------------------------------------------
    uint32_t paramCount() const override
    {
        return static_cast<uint32_t>(mParams.size());
    }
    bool paramInfo(uint32_t index, ParamInfo &out) const override;
    double paramGet(uint32_t index) const override;
    void paramSetFromUi(uint32_t index, double normalized) override;
    bool paramDisplay(uint32_t index, double normalized, char *buf, int32_t bufLen) const override;
    bool paramPollFromRt(uint32_t &index, double &normalized) override;
    void paramFlushToPlugin() override;

    //--- state ---------------------------------------------------------
    void stateSetDirectory(const char *dir) override;
    bool stateSave(std::vector<uint8_t> &out) const override;
    bool stateLoad(const uint8_t *data, size_t len) override;

    //--- editor --------------------------------------------------------
    EditorKind editorKind() const override
    {
        return mEditorKind;
    }
    bool editorOpen(const EditorOpenRequest &request, EditorSurface &out) override;
    void editorIdle() override;
    bool editorTakeResizeRequest(int32_t &w, int32_t &h) override;
    bool editorCheckSize(int32_t &w, int32_t &h) const override;
    void editorSetSize(int32_t w, int32_t h) override;
    void editorShow() override;
    void editorHide() override;
    void editorClose() override;

    //--- host extensions ------------------------------------------------
    // The NAMp file loader is a VST3 interface; an LV2 plug-in cannot expose it.
    bool hasFileLoader() const override
    {
        return false;
    }
    bool fileGet(int32_t, char *, int32_t) const override
    {
        return false;
    }
    bool fileSet(int32_t, const char *) override
    {
        return false;
    }

    //--- diagnostics ----------------------------------------------------
    int64_t diagTakeMaxMicros() override;
    uint64_t diagRtAllocCount() const override
    {
        return mRtAllocCount.load(std::memory_order_relaxed);
    }

private:
    Lv2Backend() = default;

    //--- one port, as read from the plug-in's Turtle once at load() ------
    struct Port {
        enum class Kind : uint8_t { Control, Audio, Atom, Ignored };

        uint32_t index = 0;
        Kind kind = Kind::Ignored;
        bool isInput = false;
        // The output control port designated lv2:latency. Read after every block.
        bool isLatency = false;
        bool notOnGui = false;

        float minimum = 0.0f;
        float maximum = 1.0f;
        float defaultValue = 0.0f;
        // Scaled by the sample rate at prepare() time, per lv2:sampleRate.
        bool scaledBySampleRate = false;
        int32_t stepCount = 0;
        bool isToggled = false;
        bool isEnumeration = false;

        // Atom ports only.
        uint32_t bufferSize = 0;
        bool carriesMidiOnly = false;
        // Will take midi:MidiEvent, whether or not it takes anything else. See the note where it
        // is set: this and carriesMidiOnly answer different questions and a port can accept MIDI
        // without carrying only MIDI.
        bool acceptsMidi = false;

        char name[128] = {};
        char symbol[64] = {};
    };

    //--- one instantiation. Two of these when a mono plug-in sits in the stereo section.
    struct Voice {
        LilvInstance *instance = nullptr;
        // Port index -> the float the plug-in reads or writes. Control ports only; sized to the
        // full port count so indexing needs no second table.
        std::vector<float> values;
        // Port index -> event buffer, or null. Same sizing rule.
        std::vector<LV2_Evbuf *> atoms;
        std::unique_ptr<Lv2Worker> worker;
    };

    void teardown();
    void teardownVoices();
    bool readPorts(std::string &error);
    bool buildVoice(Voice &voice, int32_t channelOffset, std::string &error);
    void connectAudio(Voice &voice, int32_t channelOffset, float *const *in, float *const *out,
                      int32_t channels) noexcept;
    void buildFeatures();
    void applyLatency() noexcept;

    // The UI's port write callback, and the two lookups suil needs alongside it.
    static void uiWrite(void *controller, uint32_t portIndex, uint32_t bufferSize,
                        uint32_t protocol, const void *buffer);
    static uint32_t uiPortIndex(void *controller, const char *symbol);
    static void uiTouch(void *controller, uint32_t portIndex, bool grabbed);
    static int uiResize(LV2UI_Feature_Handle handle, int width, int height);

    //--- identity, immutable after load() -------------------------------
    std::string mKey;
    std::string mName;
    std::string mCategory;
    const LilvPlugin *mPlugin = nullptr;

    // Where the next stateSave()/stateLoad() may keep files the state refers to. Empty means
    // nowhere, which is the in-memory case and what everything before presets existed did. Owning
    // thread only; it is read by nothing on the audio path.
    std::string mStateDir;

    //--- port layout, immutable after load() ----------------------------
    std::vector<Port> mPorts;
    // Indices into mPorts of the audio ports, in declaration order — which is the channel order.
    std::vector<uint32_t> mAudioIn;
    std::vector<uint32_t> mAudioOut;
    // Indices into mPorts of the input control ports the panel shows, in declaration order.
    std::vector<uint32_t> mParams;
    int32_t mAudioInPorts = 0;
    int32_t mAudioOutPorts = 0;
    // The node's channel count, which prepare() sets once it knows how many instances the section
    // needs. Before prepare() it is the plug-in's own, which is the only truth available then.
    int32_t mEffectiveIn = 0;
    int32_t mEffectiveOut = 0;
    bool mInPlaceOk = true;

    //--- audio configuration --------------------------------------------
    ProcessConfig mConfig;
    bool mPrepared = false;
    bool mActive = false;
    std::vector<Voice> mVoices;
    // Set when the plug-in's channel count does not match the chain's and buffers must be
    // marshalled rather than pointed at. Costs what the parent project always pays; documented, not
    // hidden.
    bool mNeedsScratch = false;
    std::vector<float> mScratch;
    std::vector<float *> mScratchIn;
    std::vector<float *> mScratchOut;

    //--- parameters -----------------------------------------------------
    // UI -> RT. One fixed-size record per edit, so the audio thread copies rather than
    // dereferences.
    struct ParamEdit {
        uint32_t param = 0;
        float value = 0.0f;
    };
    SpscByteQueue<8192> mToRt;

    // The largest atom that can travel between the plug-in and its own UI. Bigger than a patch:Set
    // carrying a file path, small enough to live on the audio thread's stack while it is copied.
    static constexpr uint32_t kUiAtomStagingBytes = 4096;
    // UI -> RT and RT -> UI atom transfer, each a plain SPSC ring carrying
    // [uint32 port index][LV2_Atom header][body]. Separate from the parameter ring because an atom
    // is variable length and a control value is not.
    SpscByteQueue<64 * 1024> mUiToRtAtoms;
    SpscByteQueue<64 * 1024> mRtToUiAtoms;
    // Read before touching mUiToRtAtoms so that a block with no UI traffic — which is almost every
    // block — costs one relaxed load rather than a ring inspection.
    std::atomic<bool> mUiToRtPending{false};
    // Gates the RT -> UI copy entirely. A plug-in with a busy notification port must cost nothing
    // while nobody is looking at it.
    std::atomic<bool> mEditorOpen{false};
    // The UI-side shadow, so paramGet() never reads a float the audio thread is writing.
    std::vector<float> mShadow;
    // What the UI last saw of each output control port, so paramPollFromRt reports changes once.
    std::vector<float> mOutputShadow;
    uint32_t mPollCursor = 0;

    //--- features --------------------------------------------------------
    LV2_Feature mMapFeature{};
    LV2_Feature mUnmapFeature{};
    LV2_Feature mWorkerFeature{};
    LV2_Feature mOptionsFeature{};
    LV2_Feature mBoundedBlockFeature{};
    LV2_Feature mFixedBlockFeature{};
    LV2_Feature mPowerOf2BlockFeature{};
    LV2_Feature mThreadSafeRestoreFeature{};
    std::vector<LV2_Options_Option> mOptions;
    std::vector<const LV2_Feature *> mFeatures;
    // Backing storage for the option values; the option array holds pointers into it.
    int32_t mOptMinBlock = 0;
    int32_t mOptMaxBlock = 0;
    int32_t mOptNominalBlock = 0;
    int32_t mOptSequenceSize = 0;
    float mOptSampleRate = 48000.0f;

    //--- urids used on the audio path, resolved once ---------------------
    uint32_t mUridAtomChunk = 0;
    uint32_t mUridAtomSequence = 0;
    uint32_t mUridAtomEventTransfer = 0;
    uint32_t mUridMidiEvent = 0;
    uint32_t mUridFloat = 0;

    //--- editor ----------------------------------------------------------
    EditorKind mEditorKind = EditorKind::NoEditor;
    const LilvUI *mUi = nullptr;
    const LilvNode *mUiType = nullptr;
    LilvUIs *mUis = nullptr;
    SuilHost *mSuilHost = nullptr;
    SuilInstance *mSuilInstance = nullptr;
    const LV2UI_Idle_Interface *mUiIdle = nullptr;
    const LV2UI_Show_Interface *mUiShow = nullptr;
    LV2UI_Resize mUiResize{};
    LV2_Feature mUiParentFeature{};
    LV2_Feature mUiResizeFeature{};
    LV2_Feature mUiIdleFeature{};
    std::vector<const LV2_Feature *> mUiFeatures;
    bool mUiFixedSize = false;
    // The UI's resize callback may arrive from a thread we do not control, so it only latches.
    std::atomic<bool> mResizePending{false};
    std::atomic<int32_t> mResizeW{0};
    std::atomic<int32_t> mResizeH{0};
    // What the UI was last told about each parameter, so the idle tick only sends changes.
    std::vector<float> mUiSent;

    //--- diagnostics ------------------------------------------------------
    std::atomic<int64_t> mMaxMicros{0};
    std::atomic<uint64_t> mRtAllocCount{0};
    std::atomic<uint32_t> mLatency{0};
};

} // namespace NAMp::host
