// Vst3Backend implementation. See backend_vst3.h for the four decisions this file exists to get
// right, and pluginbackend.h's threading note for the real-time contract process() obeys.

#include "backend_vst3.h"
#include "diagnostics.h"
#include "hostapp.h"
#include "rtdenormal.h"

#include "inampfileloader.h"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "chainmodel.h" // kMaxChunkMidi: the engine's ceiling on messages per cycle
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "scancache.h" // pathIsSafe, whose notion of "absolute" is the platform's

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <cstring>

using namespace Steinberg;

namespace NAMp::host
{

namespace
{

// The chain never gives a plug-in more than this many channels, so a plug-in that negotiates a
// wider bus gets its surplus channels pointed at a dump buffer rather than at chain memory.
constexpr int32_t kMaxChannels = 2;

// A saved chain is untrusted input: cap what a key is allowed to be before touching the filesystem.
constexpr size_t kMaxKeyLength = 4096;
constexpr size_t kUidStringLength = 32;

// Copy a String128 into a fixed char buffer as UTF-8, always NUL-terminated.
void copyString128(const Vst::TChar *src, char *dst, size_t dstLen)
{
    if (dstLen == 0)
        return;
    dst[0] = 0;
    if (!src)
        return;
    const std::string utf8 = Vst::StringConvert::convert(src);
    std::snprintf(dst, dstLen, "%s", utf8.c_str());
}

//------------------------------------------------------------------------
// The window type a hosted editor is embedded into on THIS platform, and the answer differs.
//
// Both of the calls that need it — isPlatformTypeSupported, to decide whether this plug-in has an
// editor this host can show at all, and attached, to give it the window — used to name the X11
// constant outright. That is correct on Linux and silently wrong on Windows: every hosted plug-in
// would report no embeddable editor, and the rack would quietly fall back to the generic parameter
// panel for all of them. Nothing would look broken; the editors would just never appear.
//
// It is chosen here rather than taken from the standalone's own window seam because this file is
// the host layer: it is built into the offline tools and the plug-in bundle as well, neither of
// which owns a window. The SDK's own platform macro is the whole dependency.
inline const FIDString kHostPlatformType =
#if SMTG_OS_WINDOWS
    kPlatformTypeHWND;
#else
    kPlatformTypeX11EmbedWindowID;
#endif

} // namespace

//------------------------------------------------------------------------
bool parseVst3Key(const std::string &key, std::string &bundlePath, std::string &uidString)
{
    if (key.empty() || key.size() > kMaxKeyLength)
        return false;
    const size_t sep = key.rfind(kKeySeparator);
    if (sep == std::string::npos)
        return false;

    bundlePath = key.substr(0, sep);
    uidString = key.substr(sep + 1);

    // pathIsSafe() rather than a test spelled out again here, because "absolute" is not the same
    // string on both platforms: a key made on Windows begins "C:\\" or "\\\\server\\share", and a
    // leading-'/' test would refuse every plug-in there — discovery would list them and nothing
    // would ever load. That function is where the platform's answer lives, and it also rejects the
    // "..", the tab and the newline this used to check for separately.
    if (!pathIsSafe(bundlePath))
        return false;
    if (uidString.size() != kUidStringLength)
        return false;
    for (const char c : uidString) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!hex)
            return false;
    }
    return true;
}

//------------------------------------------------------------------------
std::string makeVst3Key(const std::string &bundlePath, const std::string &uidString)
{
    return bundlePath + kKeySeparator + uidString;
}

//------------------------------------------------------------------------
Vst3Backend::~Vst3Backend()
{
    teardown();
}

//------------------------------------------------------------------------
Vst3Backend *Vst3Backend::load(const PluginRef &ref, std::string &error)
{
    std::string bundlePath;
    std::string uidString;
    if (!parseVst3Key(ref.key, bundlePath, uidString)) {
        error = "malformed VST3 key";
        return nullptr;
    }

    // dlopen runs the shared object's static initialisers, which is third-party code executing
    // before anything has had a chance to validate it — and it is entitled to throw. Letting that
    // escape aborts the whole rack over one pedal, so it is caught here and reported as an ordinary
    // load failure.
    //
    // FLAGGED, and it is a real limit rather than an oversight: this catches a throw only when the
    // plug-in's frames are unwindable. A throw from a static initialiser runs inside dlopen, under
    // frames that carry no unwind information, so it reaches std::terminate before the stack can
    // reach here and no host can survive it in-process.
    //
    // The way that gets provoked is two plug-ins statically linking the same library and exporting
    // its internals: GCC gives a function-local static in an inline function STB_GNU_UNIQUE binding
    // ('u' in nm -D), which glibc resolves through a table that RTLD_LOCAL does not scope, so the
    // second plug-in inherits the first one's supposedly-private static. A registry that throws on
    // duplicate registration then aborts the process. This host is built with hidden visibility
    // and -Wl,--exclude-libs,ALL, and its dynamic symbol table was measured to hold three entries,
    // all of them libc's own — so it can no longer be either party (see the visibility block in
    // CMakeLists.txt); two THIRD-PARTY plug-ins doing it to each other is still
    // fatal and is not something a host can prevent short of loading one in its own link-map
    // namespace. That is what the out-of-process scanner protects discovery from; a load is
    // in-process by design.
    VST3::Hosting::Module::Ptr module;
    try {
        module = VST3::Hosting::Module::create(bundlePath, error);
    } catch (const std::exception &e) {
        error = std::string("loading ") + bundlePath + " threw: " + e.what();
        return nullptr;
    } catch (...) {
        error = "loading " + bundlePath + " threw an unknown exception";
        return nullptr;
    }
    if (!module)
        return nullptr;

    // The key carries the class UID rather than the class name, because a vendor renaming a
    // plug-in between versions would otherwise orphan every saved chain that referenced it. Fall
    // back to name matching only if the uid is genuinely absent.
    const VST3::Hosting::PluginFactory &factory = module->getFactory();
    const VST3::Hosting::ClassInfo *match = nullptr;
    auto classInfos = factory.classInfos();
    for (const auto &info : classInfos) {
        if (info.category() != kVstAudioEffectClass)
            continue;
        if (info.ID().toString(false) == uidString) {
            match = &info;
            break;
        }
    }
    if (!match) {
        error = "class uid " + uidString + " not found in " + bundlePath;
        return nullptr;
    }

    auto *self = new Vst3Backend;
    self->mModule = module;
    self->mKey = ref.key;
    self->mName = match->name();
    self->mCategory = match->subCategoriesString();

    // Instantiation is more third-party code, and equally entitled to throw.
    bool initialized = false;
    try {
        self->mProvider = owned(new Vst::PlugProvider(factory, *match, true));
        initialized = self->mProvider->initialize();
    } catch (const std::exception &e) {
        error = "instantiating " + self->mName + " threw: " + e.what();
        delete self;
        return nullptr;
    } catch (...) {
        error = "instantiating " + self->mName + " threw an unknown exception";
        delete self;
        return nullptr;
    }
    if (!initialized) {
        error = "PlugProvider::initialize failed for " + self->mName;
        delete self;
        return nullptr;
    }

    self->mComponent = self->mProvider->getComponentPtr();
    self->mController = self->mProvider->getControllerPtr();

    // PlugProvider connects the component/controller pair but installs no component handler, and
    // without one IEditController::performEdit returns kResultFalse and the edit is gone. See
    // decision 5 at the top of the header: this is what makes a plug-in's own editor able to
    // change its own sound.
    // A controller that refuses the handler is not fatal — its audio still works and the generic
    // panel still drives it — but its own editor will be a decoration, so say which plug-in it was
    // rather than leaving the user to wonder why one window in the rack does nothing.
    if (self->mController &&
        self->mController->setComponentHandler(&self->mEditHandler) != kResultOk)
        std::fprintf(stderr,
                     "[NAMp-Rack] %s refused the component handler: edits made in its own "
                     "editor will not reach its audio\n",
                     self->mName.c_str());
    if (!self->mComponent) {
        error = "no IComponent for " + self->mName;
        delete self;
        return nullptr;
    }

    self->mProcessor = FUnknownPtr<Vst::IAudioProcessor>(self->mComponent);
    if (!self->mProcessor) {
        error = self->mName + " has no IAudioProcessor";
        delete self;
        return nullptr;
    }

    return self;
}

//------------------------------------------------------------------------
void Vst3Backend::teardown()
{
    editorClose();

    if (mActive)
        deactivate();

    if (mController)
        mController->setComponentHandler(nullptr);

    mData.unprepare();

    mProcessor = nullptr;
    mController = nullptr;
    mComponent = nullptr;
    mProvider = nullptr;
    mModule = nullptr;
}

//------------------------------------------------------------------------
void Vst3Backend::negotiateArrangements(int32_t channels)
{
    if (channels < 1)
        channels = 1;
    if (channels > kMaxChannels)
        channels = kMaxChannels;

    Vst::SpeakerArrangement wanted =
        (channels == 1) ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;

    // Best effort: a plug-in is entitled to refuse, and many stereo-only ones do. Whatever it
    // settles on is read back below rather than assumed.
    mProcessor->setBusArrangements(&wanted, 1, &wanted, 1);

    Vst::SpeakerArrangement got = 0;
    mPlugInChannels = 0;
    mPlugOutChannels = 0;
    if (mProcessor->getBusArrangement(Vst::kInput, 0, got) == kResultTrue)
        mPlugInChannels = Vst::SpeakerArr::getChannelCount(got);
    if (mProcessor->getBusArrangement(Vst::kOutput, 0, got) == kResultTrue)
        mPlugOutChannels = Vst::SpeakerArr::getChannelCount(got);

    // A plug-in with no main bus at all still has to be given something sane to write into.
    if (mPlugInChannels < 0)
        mPlugInChannels = 0;
    if (mPlugOutChannels < 0)
        mPlugOutChannels = 0;
}

//------------------------------------------------------------------------
void Vst3Backend::activateDefaultBuses()
{
    // VST3 buses are INACTIVE by default. A host that forgets this gets silence out of most
    // plug-ins and no error anywhere to explain it.
    //
    // EVENT buses as well as audio ones, and the pedals beside this host say why in their own
    // source: without an active event input NO MIDI arrives at all — not the notes that come
    // through inputEvents, and not the CC and Program Change that come through the parameter
    // queues either, because a plug-in that sees no event input has no reason to believe the host
    // will route MIDI to it.
    for (const auto type : {Vst::kAudio, Vst::kEvent}) {
        for (const auto dir : {Vst::kInput, Vst::kOutput}) {
            const int32 count = mComponent->getBusCount(type, dir);
            for (int32 i = 0; i < count; ++i) {
                Vst::BusInfo info = {};
                if (mComponent->getBusInfo(type, dir, i, info) != kResultTrue)
                    continue;
                if (info.flags & Vst::BusInfo::kDefaultActive)
                    mComponent->activateBus(type, dir, i, true);
            }
        }
    }
}

//------------------------------------------------------------------------
bool Vst3Backend::prepare(const ProcessConfig &config)
{
    if (!mComponent || !mProcessor)
        return false;

    mConfig = config;

    negotiateArrangements(config.channels);

    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = config.maxBlock;
    setup.sampleRate = config.sampleRate;
    if (mProcessor->setupProcessing(setup) != kResultTrue)
        return false;

    if (mProcessor->canProcessSampleSize(Vst::kSample32) != kResultTrue)
        return false;

    activateDefaultBuses();

    // bufferSamples == 0 is the whole no-copy design: HostProcessData then owns no sample memory
    // and setChannelBuffer() is legal, so each channel pointer can be aimed straight at a chain
    // bus. Passing a non-zero size here would make the class allocate its own buffers, make
    // setChannelBuffer() return false, and force the memcpy-in/memcpy-out shape this host exists to
    // avoid.
    if (!mData.prepare(*mComponent, 0, Vst::kSample32))
        return false;

    mData.processContext = &mContext;
    mData.inputParameterChanges = &mInputChanges;
    mData.outputParameterChanges = &mOutputChanges;
    mData.inputEvents = &mEvents;
    mData.processMode = Vst::kRealtime;
    mData.symbolicSampleSize = Vst::kSample32;

    mContext.sampleRate = config.sampleRate;
    mContext.state = 0;

    // Enumerate parameters once so the generic panel never has to call the controller per frame.
    mParams.clear();
    if (mController) {
        const int32 count = mController->getParameterCount();
        mParams.reserve(static_cast<size_t>(count < 0 ? 0 : count));
        for (int32 i = 0; i < count; ++i) {
            Vst::ParameterInfo info = {};
            if (mController->getParameterInfo(i, info) != kResultTrue)
                continue;
            Param p;
            p.id = info.id;
            p.stepCount = info.stepCount;
            p.defaultNormalized = info.defaultNormalizedValue;
            p.isBypass = (info.flags & Vst::ParameterInfo::kIsBypass) != 0;
            p.isReadOnly = (info.flags & Vst::ParameterInfo::kIsReadOnly) != 0 ||
                           (info.flags & Vst::ParameterInfo::kIsHidden) != 0;
            // The other MIDI door: a Program Change is delivered by writing the program number
            // onto the unit's program-list parameter, which the SDK marks with this flag
            // (pluginterfaces/vst/ivsteditcontroller.h). Same category as a CC destination and
            // caught here because IMidiMapping does not report it.
            p.isMidiMapped = (info.flags & Vst::ParameterInfo::kIsProgramChange) != 0;
            copyString128(info.title, p.title, sizeof(p.title));
            copyString128(info.units, p.units, sizeof(p.units));
            mParams.push_back(p);
        }

        // Which of them are MIDI controller destinations rather than settings. Asked of the
        // plug-in through IMidiMapping (pluginterfaces/vst/ivsteditcontroller.h) rather than
        // guessed from a parameter's name or flags: a CC destination is required to carry FLAGS 0,
        // which is indistinguishable from an ordinary knob, and the SDK's own mda samples name
        // theirs "MIDI CC n" only by convention.
        //
        // Bus 0 and channel 0 only. The interface is per bus and per channel, and a plug-in is
        // free to map a controller differently on each — but a parameter that is a destination
        // anywhere is not a setting, and one pass over the first bus finds the block every plug-in
        // that does this publishes. A plug-in with no event input, or none of this interface,
        // answers nothing and every parameter stays a setting.
        if (auto mapping = FUnknownPtr<Vst::IMidiMapping>(mController)) {
            for (int16 cc = 0; cc < Vst::kCountCtrlNumber; ++cc) {
                Vst::ParamID id = 0;
                if (mapping->getMidiControllerAssignment(0, 0, cc, id) != kResultTrue)
                    continue;
                for (Param &p : mParams) {
                    if (p.id == id) {
                        p.isMidiMapped = true;
                        break;
                    }
                }
            }
        }
    }

    // Pre-size both rings and both queues so nothing grows on the audio thread. The +8 leaves room
    // for a plug-in that publishes output parameters we did not enumerate.
    // Where this plug-in wants each kind of MIDI message. Owning thread, before any block runs,
    // because both lookups call the controller.
    mMidiRoute.resolve(mController);

    // One cycle's worth of notes. kMaxChunkMidi is the engine's own ceiling on messages per cycle,
    // so a list that size cannot be overrun by anything the engine is able to hand over.
    mEvents.setMaxSize(kMaxChunkMidi);

    const int32 paramSlots = static_cast<int32>(mParams.size()) + 8;
    mToRt.setMaxParameters(paramSlots);
    mFromRt.setMaxParameters(paramSlots);
    mInputChanges.setMaxParameters(paramSlots);
    mOutputChanges.setMaxParameters(paramSlots);

    // The fast path needs the plug-in's channel count to match the chain's on both sides. When it
    // does not, fall back to internal scratch with copies at the edges — see the header.
    mDirectBuffers = (mPlugInChannels == config.channels && mPlugOutChannels == config.channels);

    mScratch.clear();
    mScratchIn.clear();
    mScratchOut.clear();
    if (!mDirectBuffers) {
        const int32_t total = mPlugInChannels + mPlugOutChannels;
        if (total > 0) {
            mScratch.assign(static_cast<size_t>(total) * static_cast<size_t>(config.maxBlock),
                            0.0f);
            mScratchIn.resize(static_cast<size_t>(mPlugInChannels));
            mScratchOut.resize(static_cast<size_t>(mPlugOutChannels));
            for (int32_t c = 0; c < mPlugInChannels; ++c)
                mScratchIn[static_cast<size_t>(c)] = mScratch.data() + c * config.maxBlock;
            for (int32_t c = 0; c < mPlugOutChannels; ++c)
                mScratchOut[static_cast<size_t>(c)] =
                    mScratch.data() + (mPlugInChannels + c) * config.maxBlock;
        }
    }

    mPrepared = true;
    return true;
}

//------------------------------------------------------------------------
void Vst3Backend::activate()
{
    if (!mPrepared || mActive)
        return;
    mComponent->setActive(true);
    mProcessor->setProcessing(true);
    mActive = true;
}

//------------------------------------------------------------------------
void Vst3Backend::deactivate()
{
    if (!mActive)
        return;
    mProcessor->setProcessing(false);
    mComponent->setActive(false);
    mActive = false;
}

//------------------------------------------------------------------------
void Vst3Backend::reset()
{
    // Not RT-safe, and not called from the audio thread. Cycling active state is the only portable
    // way to make a VST3 plug-in drop its tails.
    if (!mPrepared)
        return;
    const bool wasActive = mActive;
    deactivate();
    if (wasActive)
        activate();
}

//------------------------------------------------------------------------
uint32_t Vst3Backend::latencySamples() const
{
    if (!mProcessor)
        return 0;
    return mProcessor->getLatencySamples();
}

//------------------------------------------------------------------------
void Vst3Backend::process(const AudioBlock &block) noexcept
{
    if (!mActive || block.frames <= 0)
        return;

    // A plug-in may clear MXCSR and not restore it, so the mode is re-armed per node rather than
    // once per block: one misbehaving pedal must not leave the rest of the chain in subnormals.
    rtSetDenormalMode();

    const bool diag = diagArmed();
    std::chrono::steady_clock::time_point started;
    uint64_t allocBefore = 0;
    if (diag) {
        started = std::chrono::steady_clock::now();
        if (auto *app = hostApp())
            allocBefore = app->rtAllocCount();
    }

    mData.numSamples = block.frames;

    if (mDirectBuffers) {
        // The fast path: aim the plug-in's channel pointers straight at the chain buses. No copy
        // in, no memset, no copy out.
        for (int32_t c = 0; c < mPlugInChannels; ++c)
            mData.setChannelBuffer(Vst::kInput, 0, c, block.in[c]);
        for (int32_t c = 0; c < mPlugOutChannels; ++c)
            mData.setChannelBuffer(Vst::kOutput, 0, c, block.out[c]);
    } else {
        // Fallback: the plug-in refused the chain's channel count. Feed every plug-in input from
        // the block (duplicating the last available channel) and take the block's outputs from the
        // first plug-in outputs.
        for (int32_t c = 0; c < mPlugInChannels; ++c) {
            const int32_t src = (c < block.channels) ? c : block.channels - 1;
            std::memcpy(mScratchIn[static_cast<size_t>(c)], block.in[src],
                        sizeof(float) * static_cast<size_t>(block.frames));
            mData.setChannelBuffer(Vst::kInput, 0, c, mScratchIn[static_cast<size_t>(c)]);
        }
        for (int32_t c = 0; c < mPlugOutChannels; ++c)
            mData.setChannelBuffer(Vst::kOutput, 0, c, mScratchOut[static_cast<size_t>(c)]);
    }

    mInputChanges.clearQueue();
    mOutputChanges.clearQueue();
    mEvents.clear();
    mToRt.transferChangesTo(mInputChanges);

    // MIDI goes in after the UI's edits and before process(), so a footswitch and a knob turned in
    // the same cycle both reach the plug-in in the same block.
    deliverMidi(block);

    // The chain never hands a bus that is known to be silent, and a stale flag from the previous
    // block would tell the plug-in its input is silence when it is not.
    for (int32 bus = 0; bus < mData.numInputs; ++bus)
        mData.inputs[bus].silenceFlags = 0;

    mProcessor->process(mData);

    // A plug-in that sets an output channel's silence flag is entitled to leave that buffer
    // untouched, so its contents are undefined. The next node in the chain reads it either way,
    // which is why silence has to be made real here rather than assumed.
    for (int32 bus = 0; bus < mData.numOutputs; ++bus) {
        const uint64 flags = mData.outputs[bus].silenceFlags;
        if (!flags)
            continue;
        const int32 channels = mData.outputs[bus].numChannels;
        for (int32 c = 0; c < channels && c < 64; ++c) {
            if (flags & (static_cast<uint64>(1) << c)) {
                if (float *buffer = mData.outputs[bus].channelBuffers32[c])
                    std::memset(buffer, 0, sizeof(float) * static_cast<size_t>(block.frames));
            }
        }
        mData.outputs[bus].silenceFlags = 0;
    }

    mFromRt.transferChangesFrom(mOutputChanges);

    if (!mDirectBuffers) {
        for (int32_t c = 0; c < block.channels; ++c) {
            const int32_t src = (c < mPlugOutChannels) ? c : mPlugOutChannels - 1;
            if (src < 0)
                continue;
            std::memcpy(block.out[c], mScratchOut[static_cast<size_t>(src)],
                        sizeof(float) * static_cast<size_t>(block.frames));
        }
    }

    if (diag) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        int64_t previous = mMaxMicros.load(std::memory_order_relaxed);
        while (elapsed > previous &&
               !mMaxMicros.compare_exchange_weak(previous, elapsed, std::memory_order_relaxed)) {
        }
        if (auto *app = hostApp()) {
            const uint64_t made = app->rtAllocCount() - allocBefore;
            if (made)
                mRtAllocCount.fetch_add(made, std::memory_order_relaxed);
        }
    }
}

//------------------------------------------------------------------------
bool Vst3Backend::paramInfo(uint32_t index, ParamInfo &out) const
{
    if (index >= mParams.size())
        return false;
    const Param &p = mParams[index];
    std::snprintf(out.name, sizeof(out.name), "%s", p.title);
    std::snprintf(out.unit, sizeof(out.unit), "%s", p.units);
    out.stepCount = p.stepCount;
    out.defaultNormalized = p.defaultNormalized;
    out.isBypass = p.isBypass;
    out.isReadOnly = p.isReadOnly;
    out.isMidiMapped = p.isMidiMapped;
    return true;
}

//------------------------------------------------------------------------
double Vst3Backend::paramGet(uint32_t index) const
{
    if (index >= mParams.size() || !mController)
        return 0.0;
    return mController->getParamNormalized(mParams[index].id);
}

//------------------------------------------------------------------------
// The program list a unit uses, or kNoProgramListId. Walks getUnitInfo comparing ids rather than
// indexing, because a unit's index and its id are different numbers.
static Vst::ProgramListID programListOfUnit(Vst::IUnitInfo &units, Vst::UnitID unitId)
{
    const int32 count = units.getUnitCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::UnitInfo info = {};
        if (units.getUnitInfo(i, info) != kResultOk)
            continue;
        if (info.id == unitId)
            return info.programListId;
    }
    return Vst::kNoProgramListId;
}

static int32 programsInList(Vst::IUnitInfo &units, Vst::ProgramListID listId)
{
    const int32 count = units.getProgramListCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::ProgramListInfo info = {};
        if (units.getProgramListInfo(i, info) != kResultOk)
            continue;
        if (info.id == listId)
            return info.programCount;
    }
    return 0;
}

// Being handed an id is not an assertion that the parameter exists: IMidiMapping and IUnitInfo both
// hand back an id, and a host that writes to an id nothing was declared under simply loses the
// message with no error anywhere. So both routes are checked against the declared parameters before
// they are believed.
static bool parameterExists(Vst::IEditController &controller, Vst::ParamID id, int32 requiredFlags)
{
    const int32 count = controller.getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info = {};
        if (controller.getParameterInfo(i, info) != kResultOk)
            continue;
        if (info.id != id)
            continue;
        return requiredFlags == 0 || (info.flags & requiredFlags) == requiredFlags;
    }
    return false;
}

//------------------------------------------------------------------------
void Vst3Backend::MidiRoute::clear()
{
    for (int ch = 0; ch < kChannels; ++ch) {
        for (int cc = 0; cc < kControllers; ++cc)
            mCc[ch][cc] = Vst::kNoParamId;
        mProgram[ch] = Program{};
    }
    mHasCc = false;
    mHasProgram = false;
}

//------------------------------------------------------------------------
double Vst3Backend::MidiRoute::programValue(int channel, int program) const
{
    if (channel < 0 || channel >= kChannels)
        return 0.0;
    const int32 count = mProgram[channel].count;
    if (count < 2)
        return 0.0;
    if (program < 0)
        program = 0;
    if (program > count - 1)
        program = count - 1;
    return static_cast<double>(program) / static_cast<double>(count - 1);
}

//------------------------------------------------------------------------
void Vst3Backend::MidiRoute::resolve(Vst::IEditController *controller)
{
    clear();
    if (!controller)
        return;

    // --- Control Change --------------------------------------------------
    if (FUnknownPtr<Vst::IMidiMapping> mapping = FUnknownPtr<Vst::IMidiMapping>(controller)) {
        for (int ch = 0; ch < kChannels; ++ch) {
            for (int cc = 0; cc < kControllers; ++cc) {
                Vst::ParamID id = Vst::kNoParamId;
                if (mapping->getMidiControllerAssignment(0, static_cast<int16>(ch),
                                                         static_cast<Vst::CtrlNumber>(cc),
                                                         id) != kResultTrue)
                    continue;
                if (id == Vst::kNoParamId || !parameterExists(*controller, id, 0))
                    continue;
                mCc[ch][cc] = id;
                mHasCc = true;
            }
        }
    }

    // --- Program Change --------------------------------------------------
    if (FUnknownPtr<Vst::IUnitInfo> units = FUnknownPtr<Vst::IUnitInfo>(controller)) {
        for (int ch = 0; ch < kChannels; ++ch) {
            Vst::UnitID unitId = Vst::kRootUnitId;
            if (units->getUnitByBus(Vst::kEvent, Vst::kInput, 0, static_cast<int16>(ch), unitId) !=
                kResultTrue)
                continue;

            const Vst::ProgramListID listId = programListOfUnit(*units, unitId);
            if (listId == Vst::kNoProgramListId)
                continue;

            // EditControllerEx1 builds a program list's parameter with the LIST's own id as the
            // ParamID, so the two numbers are the same number by construction rather than by
            // coincidence.
            const Vst::ParamID id = static_cast<Vst::ParamID>(listId);
            if (!parameterExists(*controller, id, Vst::ParameterInfo::kIsProgramChange))
                continue;

            const int32 count = programsInList(*units, listId);
            if (count < 2)
                continue;

            mProgram[ch].id = id;
            mProgram[ch].count = count;
            mHasProgram = true;
        }
    }
}

//------------------------------------------------------------------------
// Audio thread. Open whichever of the three doors each message belongs to. No allocation, no lock,
// no controller call: every lookup here is an array subscript into the table prepare() built.
void Vst3Backend::deliverMidi(const AudioBlock &block) noexcept
{
    for (int32_t i = 0; i < block.midiCount; ++i) {
        const RtMidiEvent &m = block.midi[i];
        const int status = m.status & 0xf0;
        const int channel = m.status & 0x0f;
        const int data1 = m.data1 & 0x7f;
        const int data2 = m.data2 & 0x7f;
        const int32 offset = m.frame;

        switch (status) {
            case 0xb0: { // Control Change
                const Vst::ParamID id = mMidiRoute.ccParam(channel, data1);
                if (id == Vst::kNoParamId)
                    break;
                int32 index = 0;
                if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(id, index)) {
                    int32 point = 0;
                    queue->addPoint(offset, data2 / 127.0, point);
                }
                break;
            }
            case 0xc0: { // Program Change
                const Vst::ParamID id = mMidiRoute.programParam(channel);
                if (id == Vst::kNoParamId)
                    break;
                int32 index = 0;
                if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(id, index)) {
                    int32 point = 0;
                    queue->addPoint(offset, mMidiRoute.programValue(channel, data1), point);
                }
                break;
            }
            case 0x90:   // Note On
            case 0x80: { // Note Off
                // A note on at velocity 0 is a note OFF — the oldest convention in MIDI, and one a
                // controller is entitled to use. Delivering it as an on would latch a footswitch
                // again when the foot came up.
                const bool on = (status == 0x90) && data2 > 0;
                Vst::Event e = {};
                e.busIndex = 0;
                e.sampleOffset = offset;
                e.type = on ? static_cast<uint16>(Vst::Event::kNoteOnEvent)
                            : static_cast<uint16>(Vst::Event::kNoteOffEvent);
                if (on) {
                    e.noteOn.channel = static_cast<int16>(channel);
                    e.noteOn.pitch = static_cast<int16>(data1);
                    e.noteOn.velocity = static_cast<float>(data2) / 127.0f;
                    e.noteOn.noteId = -1;
                } else {
                    e.noteOff.channel = static_cast<int16>(channel);
                    e.noteOff.pitch = static_cast<int16>(data1);
                    e.noteOff.velocity = static_cast<float>(data2) / 127.0f;
                    e.noteOff.noteId = -1;
                }
                mEvents.addEvent(e); // full is a drop, not a growth
                break;
            }
            default:
                break;
        }
    }
}

//------------------------------------------------------------------------
size_t Vst3Backend::indexOfParamId(Vst::ParamID id) const
{
    for (size_t i = 0; i < mParams.size(); ++i)
        if (mParams[i].id == id)
            return i;
    return mParams.size();
}

//------------------------------------------------------------------------
tresult PLUGIN_API Vst3Backend::EditHandler::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<Vst::IComponentHandler *>(this);
        addRef();
        return kResultTrue;
    }
    *obj = nullptr;
    return kNoInterface;
}

//------------------------------------------------------------------------
// beginEdit / endEdit bracket a gesture so a host can group it into one automation event. This host
// records no automation, so there is nothing to open and nothing to close, and kNotImplemented is
// the honest answer rather than a kResultOk claiming work that was not done.
tresult PLUGIN_API Vst3Backend::EditHandler::beginEdit(Vst::ParamID)
{
    return kNotImplemented;
}

tresult PLUGIN_API Vst3Backend::EditHandler::endEdit(Vst::ParamID)
{
    return kNotImplemented;
}

//------------------------------------------------------------------------
// The edit itself, on the UI thread. Straight into paramSetFromUi(), which is the only entry point
// that touches the transfer ring, so an edit from a plug-in's own window is indistinguishable
// downstream from one made in the generic panel — including in never touching the plug-in's live
// processing state from this thread.
//
// An edit arriving before prepare() has enumerated the parameters has nowhere to go: mParams is
// empty and the ring has no capacity yet. It is not lost, because the plug-in's controller holds
// the value and prepare() reads the controller.
tresult PLUGIN_API Vst3Backend::EditHandler::performEdit(Vst::ParamID id, Vst::ParamValue value)
{
    const size_t index = mOwner.indexOfParamId(id);
    if (index >= mOwner.mParams.size())
        return kResultFalse;
    mOwner.paramSetFromUi(static_cast<uint32_t>(index), value);
    return kResultTrue;
}

//------------------------------------------------------------------------
// A plug-in asking to be restarted. kParamValuesChanged needs nothing from us: paramGet() reads the
// controller live, so the panel and the next preset save already see the new values.
//
// TODO: kLatencyChanged is acknowledged and not yet acted on. Honouring it means recompiling the
// chain to re-balance delay compensation, and the backend interface has no way to ask for that;
// adding one is a chain-builder change and belongs with that work rather than being smuggled in
// here. Until then a plug-in that changes its latency while loaded stays compensated at the latency
// it reported when the chain was built.
tresult PLUGIN_API Vst3Backend::EditHandler::restartComponent(int32 flags)
{
    if (flags & Vst::kParamValuesChanged)
        return kResultTrue;
    return kNotImplemented;
}

//------------------------------------------------------------------------
void Vst3Backend::paramSetFromUi(uint32_t index, double normalized)
{
    if (index >= mParams.size())
        return;
    if (normalized < 0.0)
        normalized = 0.0;
    if (normalized > 1.0)
        normalized = 1.0;

    // The controller is a UI-thread object, so it is updated here and never from process(); the
    // audio thread only ever sees the value through the transfer ring.
    if (mController)
        mController->setParamNormalized(mParams[index].id, normalized);
    mToRt.addChange(mParams[index].id, normalized, 0);
}

//------------------------------------------------------------------------
void Vst3Backend::paramFlushToPlugin()
{
    if (!mProcessor || !mPrepared)
        return;

    // The SDK's own mechanism, not a workaround: ivstaudioprocessor.h says numChannels "could be
    // set to value '0' when the host wants to flush the parameters (when the plug-in is not
    // processed)". So this is a process() call with no audio at all — no buses, no samples, just
    // the queued changes — which is how the processor learns about knobs the user moved while the
    // audio thread was not running.
    //
    // A separate ProcessData rather than the one the audio path uses: that one is prepared with
    // real bus counts and channel pointers, and quietly zeroing its numSamples would leave those
    // buses claiming channels the plug-in would be entitled to read.
    Vst::ProcessData flush = {};
    flush.processMode = Vst::kRealtime;
    flush.symbolicSampleSize = Vst::kSample32;
    flush.numSamples = 0;
    flush.numInputs = 0;
    flush.numOutputs = 0;
    flush.inputs = nullptr;
    flush.outputs = nullptr;
    flush.processContext = &mContext;
    flush.inputParameterChanges = &mInputChanges;
    flush.outputParameterChanges = &mOutputChanges;

    mInputChanges.clearQueue();
    mOutputChanges.clearQueue();
    mToRt.transferChangesTo(mInputChanges);
    if (mInputChanges.getParameterCount() == 0)
        return; // nothing was queued, so nothing to tell it

    mProcessor->process(flush);
    mFromRt.transferChangesFrom(mOutputChanges);
}

//------------------------------------------------------------------------
bool Vst3Backend::paramDisplay(uint32_t index, double normalized, char *buf, int32_t bufLen) const
{
    if (!buf || bufLen <= 0)
        return false;
    buf[0] = 0;
    if (index >= mParams.size() || !mController)
        return false;

    Vst::String128 text = {};
    if (mController->getParamStringByValue(mParams[index].id, normalized, text) != kResultTrue)
        return false;

    const std::string utf8 = Vst::StringConvert::convert(text);
    std::snprintf(buf, static_cast<size_t>(bufLen), "%s", utf8.c_str());
    return true;
}

//------------------------------------------------------------------------
bool Vst3Backend::paramPollFromRt(uint32_t &index, double &normalized)
{
    Vst::ParamID id = 0;
    Vst::ParamValue value = 0.0;
    int32 offset = 0;
    if (!mFromRt.getNextChange(id, value, offset))
        return false;

    // THE CONTROLLER IS UPDATED HERE, AND THAT IS THE WHOLE POINT OF DRAINING.
    //
    // A plug-in that changes a parameter BY ITSELF — a footswitch stomped by MIDI, a tempo-synced
    // knob following the transport — reports it by writing a point into the block's output
    // parameter changes. That is a statement from the PROCESSOR, and the processor has no way to
    // reach its own controller: the two halves of a VST3 plug-in are separate objects and it is the
    // host that carries values between them.
    //
    // So without this line the plug-in's own editor never learns, and neither does anything else,
    // because paramGet() reads the controller too. The sound changes and every display of it stays
    // where it was — which is exactly how a footswitch behaves with the lamp painted on.
    if (mController)
        mController->setParamNormalized(id, value);

    for (size_t i = 0; i < mParams.size(); ++i) {
        if (mParams[i].id == id) {
            index = static_cast<uint32_t>(i);
            normalized = value;
            return true;
        }
    }
    // A parameter the plug-in publishes but did not enumerate. Drop it rather than reporting an
    // index the caller cannot use.
    return false;
}

//------------------------------------------------------------------------
bool Vst3Backend::stateSave(std::vector<uint8_t> &out) const
{
    if (!mComponent)
        return false;

    // Layout, mirroring what every VST3 host has to persist:
    //   int32 LE componentLen | component bytes | int32 LE controllerLen | controller bytes
    MemoryStream componentState;
    if (mComponent->getState(&componentState) != kResultTrue)
        return false;

    MemoryStream controllerState;
    const bool haveController =
        mController && mController->getState(&controllerState) == kResultTrue;

    const int32_t componentLen = static_cast<int32_t>(componentState.getSize());
    const int32_t controllerLen =
        haveController ? static_cast<int32_t>(controllerState.getSize()) : 0;
    if (componentLen < 0 || controllerLen < 0)
        return false;

    out.clear();
    out.reserve(8 + static_cast<size_t>(componentLen) + static_cast<size_t>(controllerLen));

    auto appendLen = [&out](int32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    };

    appendLen(componentLen);
    const auto *p = reinterpret_cast<const uint8_t *>(componentState.getData());
    out.insert(out.end(), p, p + componentLen);

    appendLen(controllerLen);
    if (controllerLen > 0) {
        const auto *q = reinterpret_cast<const uint8_t *>(controllerState.getData());
        out.insert(out.end(), q, q + controllerLen);
    }
    return true;
}

//------------------------------------------------------------------------
bool Vst3Backend::stateLoad(const uint8_t *data, size_t len)
{
    // Untrusted input from a saved chain: every length is checked against what is actually there
    // before it is used.
    if (!data || len < 8 || !mComponent)
        return false;

    auto readLen = [data](size_t at) {
        return static_cast<int32_t>(static_cast<uint32_t>(data[at]) |
                                    (static_cast<uint32_t>(data[at + 1]) << 8) |
                                    (static_cast<uint32_t>(data[at + 2]) << 16) |
                                    (static_cast<uint32_t>(data[at + 3]) << 24));
    };

    const int32_t componentLen = readLen(0);
    if (componentLen < 0 || static_cast<size_t>(componentLen) + 8 > len)
        return false;

    const size_t controllerLenAt = 4 + static_cast<size_t>(componentLen);
    const int32_t controllerLen = readLen(controllerLenAt);
    if (controllerLen < 0 || controllerLenAt + 4 + static_cast<size_t>(controllerLen) > len)
        return false;

    // MemoryStream takes a non-const pointer but only reads from it here; the const_cast is
    // confined to this call and the buffer is never written through it.
    MemoryStream componentState(const_cast<void *>(reinterpret_cast<const void *>(data + 4)),
                                componentLen);
    componentState.seek(0, IBStream::kIBSeekSet, nullptr);
    if (mComponent->setState(&componentState) != kResultTrue)
        return false;

    if (mController) {
        // The controller has to see the component state too, or its parameter values disagree with
        // what is actually processing.
        componentState.seek(0, IBStream::kIBSeekSet, nullptr);
        mController->setComponentState(&componentState);

        if (controllerLen > 0) {
            MemoryStream controllerState(
                const_cast<void *>(reinterpret_cast<const void *>(data + controllerLenAt + 4)),
                controllerLen);
            controllerState.seek(0, IBStream::kIBSeekSet, nullptr);
            mController->setState(&controllerState);
        }
    }
    return true;
}

//------------------------------------------------------------------------
EditorKind Vst3Backend::editorKind() const
{
    return mController ? EditorKind::Vst3PlugView : EditorKind::NoEditor;
}

//------------------------------------------------------------------------
bool Vst3Backend::editorOpen(const EditorOpenRequest &request, EditorSurface &out)
{
    out = EditorSurface{};
    if (!mController)
        return false;

    if (!mView) {
        mView = owned(mController->createView(Vst::ViewType::kEditor));
        if (!mView)
            return false;
    }

    if (mView->isPlatformTypeSupported(kHostPlatformType) != kResultTrue) {
        mView = nullptr;
        return false; // caller falls back to the generic panel
    }

    ViewRect rect = {};
    if (mView->getSize(&rect) != kResultTrue)
        return false;

    if (request.plugFrame)
        mView->setFrame(static_cast<IPlugFrame *>(request.plugFrame));

    if (mView->attached(reinterpret_cast<void *>(request.parentWindow), kHostPlatformType) !=
        kResultTrue) {
        mView->setFrame(nullptr);
        mView = nullptr;
        return false;
    }

    out.kind = EditorKind::Vst3PlugView;
    out.plugView = mView.get();
    out.width = rect.getWidth();
    out.height = rect.getHeight();
    out.resizable = mView->canResize() == kResultTrue;
    return true;
}

//------------------------------------------------------------------------
// Run loop, once per UI tick while this plug-in's own editor is open.
void Vst3Backend::editorIdle()
{
    // Bounded, so a plug-in publishing a meter on every block cannot own the timer. Anything left
    // is taken on the next tick; the values are absolute rather than incremental, so arriving a
    // frame late costs nothing and arriving out of order is not possible.
    //
    // The generic panel runs the identical loop for the nodes it is showing instead, and the two
    // never overlap: a node has one window or the other, never both.
    //
    // A node with NO window open drains nowhere, and that is deliberate rather than overlooked. The
    // ring drops rather than growing, the processor's own state — which is what a preset saves — is
    // unaffected, and the only thing that can go stale is a display that is not on screen. It
    // catches up on the first tick after a window opens.
    for (int guard = 0; guard < 32; ++guard) {
        uint32_t index = 0;
        double normalized = 0.0;
        if (!paramPollFromRt(index, normalized))
            break;
    }
}

//------------------------------------------------------------------------
bool Vst3Backend::editorTakeResizeRequest(int32_t &w, int32_t &h)
{
    if (!mResizePending.exchange(false, std::memory_order_acquire))
        return false;
    w = mResizeW.load(std::memory_order_relaxed);
    h = mResizeH.load(std::memory_order_relaxed);
    return true;
}

//------------------------------------------------------------------------
// A view is allowed to leave the rect untouched, so the caller's values are the fallback rather
// than something read back unconditionally.
bool Vst3Backend::editorCheckSize(int32_t &w, int32_t &h) const
{
    if (!mView)
        return false;
    ViewRect rect = {0, 0, w, h};
    if (mView->checkSizeConstraint(&rect) != kResultTrue)
        return false;
    if (rect.getWidth() > 0 && rect.getHeight() > 0) {
        w = rect.getWidth();
        h = rect.getHeight();
    }
    return true;
}

//------------------------------------------------------------------------
void Vst3Backend::editorSetSize(int32_t w, int32_t h)
{
    if (!mView)
        return;
    ViewRect rect = {0, 0, w, h};
    mView->onSize(&rect);
}

//------------------------------------------------------------------------
void Vst3Backend::editorClose()
{
    if (!mView)
        return;
    // Order matters: the view unregisters its own run-loop handlers from inside removed(), so that
    // has to happen before the frame goes away and before the window is destroyed.
    mView->removed();
    mView->setFrame(nullptr);
    mView = nullptr;
}

//------------------------------------------------------------------------
bool Vst3Backend::hasFileLoader() const
{
    if (!mController)
        return false;
    return FUnknownPtr<NAMp::INampFileLoader>(mController).getInterface() != nullptr;
}

//------------------------------------------------------------------------
bool Vst3Backend::fileGet(int32_t which, char *buf, int32_t bufLen) const
{
    if (!buf || bufLen <= 0)
        return false;
    buf[0] = 0;
    if (!mController)
        return false;

    FUnknownPtr<NAMp::INampFileLoader> loader(mController);
    if (!loader)
        return false;

    // The getters fill a caller-supplied buffer and report kResultFalse when it is too small, so
    // the size travels straight through.
    switch (which) {
        case 0:
            return loader->getModelFile(buf, bufLen) == kResultTrue;
        case 1:
            return loader->getBankDirectory(buf, bufLen) == kResultTrue;
        case 2:
            return loader->getIrFile(buf, bufLen) == kResultTrue;
        default:
            return false;
    }
}

//------------------------------------------------------------------------
bool Vst3Backend::fileSet(int32_t which, const char *path)
{
    if (!mController || !path)
        return false;

    FUnknownPtr<NAMp::INampFileLoader> loader(mController);
    if (!loader)
        return false;

    switch (which) {
        case 0:
            return loader->setModelFile(path) == kResultTrue;
        case 1:
            return loader->setBankDirectory(path) == kResultTrue;
        case 2:
            return loader->setIrFile(path) == kResultTrue;
        default:
            return false;
    }
}

//------------------------------------------------------------------------
int64_t Vst3Backend::diagTakeMaxMicros()
{
    return mMaxMicros.exchange(0, std::memory_order_relaxed);
}

} // namespace NAMp::host
