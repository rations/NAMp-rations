// Vst3Backend — hosting a VST3 plug-in as one node of the chain.
//
// The heavy lifting is the SDK's: VST3::Hosting::Module loads the bundle, Vst::PlugProvider builds
// and connects the component/controller pair, Vst::HostProcessData carries the bus plumbing and
// Vst::ParameterChangeTransfer is a ready-made lock-free ring for UI-to-RT parameter edits. This
// class is the glue plus four decisions that are easy to get wrong:
//
//   1. NO BUFFER COPIES. HostProcessData::prepare() is called with bufferSamples == 0, which leaves
//      channelBufferOwner false and lets us aim each channel pointer straight at a chain bus with
//      setChannelBuffer(). The parent project instead memcpys the block in, memsets every output
//      channel, calls process(), and memcpys the result back out — three passes per plug-in per
//      block, multiplying by chain length. None of that happens here.
//
//   2. BUSES MUST BE ACTIVATED. VST3 audio buses are inactive by default and most plug-ins emit
//      silence if the host forgets. Every default-active bus gets activateBus().
//
//   3. IN AND OUT ARE NEVER ALIASED. VST3 does not promise a plug-in tolerates identical input and
//      output pointers, so prefersInPlace() returns false and the chain compiler always gives this
//      node distinct slots. That costs nothing: the chain ping-pongs between pre-allocated buses,
//      so distinct slots are still zero copies.
//
//   4. PARAMETER EDITS ARE ENQUEUED, NEVER APPLIED DIRECTLY. paramSetFromUi() updates the
//      controller (a UI-thread object) and pushes into the transfer ring; the audio thread drains
//      the ring into the input queues at the top of process(). The controller is never touched from
//      the audio thread and the plug-in's processing state is never touched from the UI thread.
//
//   5. THE PLUG-IN'S OWN EDITOR NEEDS A WAY BACK, AND IT IS THE ONE THE HOST MUST SUPPLY. A knob
//      turned in a plug-in's own window reaches its processor by exactly one route: the editor
//      calls IEditController::performEdit, the SDK's EditController forwards that to whatever
//      IComponentHandler the host installed, and the host writes the value into the next block's
//      input parameter changes. PlugProvider builds and connects the component/controller pair but
//      installs NO handler, and with none of our own the SDK's forward is
//
//          if (componentHandler) return componentHandler->performEdit (tag, value);
//          return kResultFalse;
//
//      — a dead end returning a failure nobody checks. The plug-in loads, its editor opens and
//      draws, its controls move, and its audio never changes. Measured on the pedals shipped beside
//      this host: a Boost with its footswitch stomped in its own window passed its input through
//      bit-identical, and the same pedal engaged through paramSetFromUi() clipped as it should.
//
// Channel adaptation: when the plug-in's negotiated channel count matches the chain's, the fast
// path applies and nothing is copied. When it does not — a stereo-only plug-in placed in the mono
// pre-amp section — the node falls back to internal scratch buffers with copies at both ends. That
// fallback costs exactly what the parent project always pays, so the win is opportunistic; it is
// documented rather than hidden.

#pragma once

#include "pluginbackend.h"

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "pluginterfaces/gui/iplugview.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace NAMp::host
{

//------------------------------------------------------------------------
// Split a VST3 key into its bundle path and class uid. Returns false if the key is malformed —
// a saved chain is untrusted input.
bool parseVst3Key(const std::string &key, std::string &bundlePath, std::string &uidString);

// Build a key from the two halves.
std::string makeVst3Key(const std::string &bundlePath, const std::string &uidString);

//------------------------------------------------------------------------
class Vst3Backend final : public PluginBackend
{
public:
    ~Vst3Backend() override;

    // Loads the bundle and builds the component/controller pair. Returns null on any failure, with
    // a reason in `error`. Does no audio configuration — that is prepare().
    static Vst3Backend *load(const PluginRef &ref, std::string &error);

    //--- identity ------------------------------------------------------
    PluginFormat format() const override
    {
        return PluginFormat::Vst3;
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

    uint32_t latencySamples() const override;
    // Always false — see decision 3 in the file comment.
    bool prefersInPlace() const override
    {
        return false;
    }
    int32_t audioInCount() const override
    {
        return mPlugInChannels;
    }
    int32_t audioOutCount() const override
    {
        return mPlugOutChannels;
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
    bool stateSave(std::vector<uint8_t> &out) const override;
    bool stateLoad(const uint8_t *data, size_t len) override;

    //--- editor --------------------------------------------------------
    EditorKind editorKind() const override;
    bool editorOpen(const EditorOpenRequest &request, EditorSurface &out) override;
    // Not empty, and what it does is not drawing. A VST3 editor draws itself; what it cannot do is
    // notice that its own processor moved a parameter, because the two halves never speak directly.
    // See paramPollFromRt: draining is what carries the value across, and while a plug-in's own
    // editor is the window on screen this is the only thing calling it.
    void editorIdle() override;
    bool editorTakeResizeRequest(int32_t &w, int32_t &h) override;
    bool editorCheckSize(int32_t &w, int32_t &h) const override;
    void editorSetSize(int32_t w, int32_t h) override;
    void editorShow() override
    {
    }
    void editorHide() override
    {
    }
    void editorClose() override;

    //--- host extensions ------------------------------------------------
    bool hasFileLoader() const override;
    bool fileGet(int32_t which, char *buf, int32_t bufLen) const override;
    bool fileSet(int32_t which, const char *path) override;

    //--- diagnostics ----------------------------------------------------
    int64_t diagTakeMaxMicros() override;
    uint64_t diagRtAllocCount() const override
    {
        return mRtAllocCount.load(std::memory_order_relaxed);
    }

private:
    Vst3Backend() = default;

    //--------------------------------------------------------------------
    // The way back from the plug-in's own editor. Every call arrives on the UI thread, which is
    // the thread paramSetFromUi() is already the entry point for, so an edit made in a plug-in's
    // window and one made in the generic panel take the identical route into the chain: enqueue,
    // and nothing else.
    //
    // Not reference counted. It is a member of the backend and dies with it, and a plug-in that
    // released it once too often would otherwise take the backend with it — which is the same
    // reason the SDK's own hosting sample pins its handler's count.
    class EditHandler final : public Steinberg::Vst::IComponentHandler
    {
    public:
        explicit EditHandler(Vst3Backend &owner) : mOwner(owner)
        {
        }

        Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override;
        Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                                  Steinberg::Vst::ParamValue value) override;
        Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override;
        Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

        Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                     void **obj) override;
        Steinberg::uint32 PLUGIN_API addRef() override
        {
            return 1000;
        }
        Steinberg::uint32 PLUGIN_API release() override
        {
            return 1000;
        }

    private:
        Vst3Backend &mOwner;
    };

    //--------------------------------------------------------------------
    // Where each kind of MIDI message has to be delivered to THIS plug-in, worked out once on the
    // owning thread. Ported from the amp standalone's own MidiRoute, which had to answer the same
    // question for the amp, and kept per instance because every answer here is a property of one
    // plug-in: two pedals in the same rack map the same controller number onto different parameters
    // and neither is wrong.
    //
    // The three things a footswitch sends do NOT arrive at a VST3 plug-in by the same route, and
    // only one of them is still MIDI by the time it lands:
    //
    //   * Control Change  -> a PARAMETER change, on the parameter IMidiMapping names for that
    //                        controller number. The MIDI channel is gone by then.
    //   * Program Change  -> a PARAMETER change too, on the parameter carrying kIsProgramChange in
    //                        the unit IUnitInfo::getUnitByBus names for the incoming channel.
    //   * Note On / Off   -> an EVENT, in ProcessData::inputEvents, still carrying its channel.
    //
    // Both lookups are IEditController calls and the audio thread may never make one, so they are
    // resolved in prepare() and what the audio thread gets afterwards is an array subscript.
    //
    // A plug-in offering neither interface is not an error. The affected route is simply dead and
    // the others still work, which is also what happens in a DAW.
    struct MidiRoute {
        static constexpr int kChannels = 16;
        static constexpr int kControllers = 128;

        void resolve(Steinberg::Vst::IEditController *controller);

        // The parameter a Control Change lands on, or kNoParamId if none does.
        Steinberg::Vst::ParamID ccParam(int channel, int cc) const
        {
            if (channel < 0 || channel >= kChannels || cc < 0 || cc >= kControllers)
                return Steinberg::Vst::kNoParamId;
            return mCc[channel][cc];
        }

        // The parameter a Program Change lands on, or kNoParamId.
        Steinberg::Vst::ParamID programParam(int channel) const
        {
            if (channel < 0 || channel >= kChannels)
                return Steinberg::Vst::kNoParamId;
            return mProgram[channel].id;
        }

        // A program number as that parameter's normalized value. The denominator is the list's own
        // length, read from the plug-in rather than assumed to be 128, because it is the plug-in
        // that decides how long its program list is.
        double programValue(int channel, int program) const;

        bool routesAnything() const
        {
            return mHasCc || mHasProgram;
        }

        struct Program {
            Steinberg::Vst::ParamID id = Steinberg::Vst::kNoParamId;
            Steinberg::int32 count = 0;
        };

        // kNoParamId is 0xffffffff rather than 0, so these cannot be left to zero-initialise:
        // "no destination" has to be written in explicitly or every controller number would appear
        // to be mapped to parameter 0. clear() is what writes it.
        void clear();

        Steinberg::Vst::ParamID mCc[kChannels][kControllers] = {};
        Program mProgram[kChannels] = {};
        bool mHasCc = false;
        bool mHasProgram = false;
    };

    // Audio thread. Open whichever of the three doors each message belongs to.
    void deliverMidi(const AudioBlock &block) noexcept;

    // Index of the parameter carrying `id`, or mParams.size() if the plug-in named one it never
    // published. A plug-in is entitled to do that and it must not be a crash.
    size_t indexOfParamId(Steinberg::Vst::ParamID id) const;

    void teardown();
    // Turn on every audio bus the plug-in marks default-active. Without this most plug-ins are
    // silent.
    void activateDefaultBuses();
    // Ask for `channels` in and out; record what the plug-in actually accepted.
    void negotiateArrangements(int32_t channels);

    //--- identity, immutable after load() -------------------------------
    std::string mKey;
    std::string mName;
    std::string mCategory;

    //--- the plug-in ----------------------------------------------------
    VST3::Hosting::Module::Ptr mModule;
    Steinberg::IPtr<Steinberg::Vst::PlugProvider> mProvider;
    Steinberg::IPtr<Steinberg::Vst::IComponent> mComponent;
    Steinberg::IPtr<Steinberg::Vst::IEditController> mController;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> mProcessor;
    Steinberg::IPtr<Steinberg::IPlugView> mView;

    //--- audio configuration --------------------------------------------
    ProcessConfig mConfig;
    bool mPrepared = false;
    bool mActive = false;
    int32_t mPlugInChannels = 0;
    int32_t mPlugOutChannels = 0;
    // True when the plug-in's channel count matches the chain's, so pointers can be aimed straight
    // at the chain buses and nothing is copied.
    bool mDirectBuffers = false;

    // Only used on the fallback path (see the file comment). Empty otherwise.
    std::vector<float> mScratch;
    std::vector<float *> mScratchIn;
    std::vector<float *> mScratchOut;

    Steinberg::Vst::HostProcessData mData;
    Steinberg::Vst::ProcessContext mContext = {};
    Steinberg::Vst::ParameterChanges mInputChanges;
    Steinberg::Vst::ParameterChanges mOutputChanges;
    Steinberg::Vst::ParameterChangeTransfer mToRt;
    Steinberg::Vst::ParameterChangeTransfer mFromRt;

    //--- MIDI ------------------------------------------------------------
    MidiRoute mMidiRoute;
    // Notes are the one message type that stays an event. Sized in prepare(); addEvent() on a full
    // list is a drop, never a growth, which is what keeps this off the allocator.
    Steinberg::Vst::EventList mEvents;

    //--- parameters -----------------------------------------------------
    struct Param {
        Steinberg::Vst::ParamID id = 0;
        int32_t stepCount = 0;
        double defaultNormalized = 0.0;
        bool isBypass = false;
        bool isReadOnly = false;
        bool isMidiMapped = false;
        char title[128] = {};
        char units[32] = {};
    };
    std::vector<Param> mParams;

    // Installed on the controller as soon as there is a controller, so an editor opened at any
    // point afterwards has somewhere to send its edits. Cleared in teardown() before the
    // controller is dropped.
    EditHandler mEditHandler{*this};

    //--- editor ---------------------------------------------------------
    std::atomic<bool> mResizePending{false};
    std::atomic<int32_t> mResizeW{0};
    std::atomic<int32_t> mResizeH{0};

    //--- diagnostics ----------------------------------------------------
    std::atomic<int64_t> mMaxMicros{0};
    std::atomic<uint64_t> mRtAllocCount{0};
};

} // namespace NAMp::host
