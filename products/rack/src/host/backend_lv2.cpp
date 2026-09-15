// Lv2Backend implementation. See backend_lv2.h for the five ways LV2 differs from VST3 and how each
// one is handled here.

#include "backend_lv2.h"
#include "lv2urid.h"
#include "lv2world.h"
#include "diagnostics.h"

#include "../../deps/jalv/lv2_evbuf.h"

#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/parameters/parameters.h>
#include <lv2/presets/presets.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/state/state.h>
#include <lv2/worker/worker.h>

#include <suil/suil.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace NAMp::host
{

namespace
{

// An atom port smaller than this is not worth having: a single patch:Set carrying a path can be a
// few hundred bytes, and a plug-in that declares nothing gets a buffer that will not embarrass it.
constexpr uint32_t kMinAtomBufferBytes = 4096;
// And a bound, because rsz:minimumSize comes out of a file on disk.
constexpr uint32_t kMaxAtomBufferBytes = 1u << 20;

// Copy a string into a fixed buffer, always terminated. The unbounded forms — strcpy, sprintf,
// strcat — are banned outright here, and every string in this file comes from a plug-in's own
// metadata.
void copyField(char *dst, size_t cap, const char *src)
{
    if (!cap)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    // snprintf rather than strncpy: strncpy's contract is "copy at most n and do not necessarily
    // terminate", which is exactly the trap the ban above exists to close. snprintf truncates and
    // always terminates, and unlike a measure-then-copy it does not make the compiler reason about
    // reading past a shorter source buffer.
    std::snprintf(dst, cap, "%s", src);
}

float clampToRange(float value, float lo, float hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

} // namespace

//------------------------------------------------------------------------
Lv2Backend::~Lv2Backend()
{
    teardown();
}

//------------------------------------------------------------------------
void Lv2Backend::teardown()
{
    editorClose();
    teardownVoices();

    if (mUis) {
        lilv_uis_free(mUis);
        mUis = nullptr;
    }
    mUi = nullptr;
    mUiType = nullptr;
    mPlugin = nullptr;
}

//------------------------------------------------------------------------
void Lv2Backend::teardownVoices()
{
    for (Voice &voice : mVoices) {
        // The worker thread calls into the plug-in, so it stops BEFORE the instance is freed. Doing
        // it the other way round is a use-after-free with third-party code on the stack.
        if (voice.worker)
            voice.worker->stop();

        if (voice.instance) {
            if (mActive)
                lilv_instance_deactivate(voice.instance);
            lilv_instance_free(voice.instance);
            voice.instance = nullptr;
        }
        for (LV2_Evbuf *buf : voice.atoms) {
            if (buf)
                lv2_evbuf_free(buf);
        }
        voice.atoms.clear();
        voice.values.clear();
        voice.worker.reset();
    }
    mVoices.clear();
    mActive = false;
    mPrepared = false;
}

//------------------------------------------------------------------------
Lv2Backend *Lv2Backend::load(const PluginRef &ref, std::string &error)
{
    error.clear();

    Lv2World *world = Lv2World::instance();
    if (!world) {
        error = "LV2 support is unavailable (lilv could not be initialised)";
        return nullptr;
    }

    const LilvPlugin *plugin = world->pluginByUri(ref.key);
    if (!plugin) {
        error = "no LV2 plug-in with URI " + ref.key + " is installed";
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(world->lock());

    const std::string reason = world->unsupportedReason(plugin);
    if (!reason.empty()) {
        error = ref.key + " " + reason;
        return nullptr;
    }

    auto backend = std::unique_ptr<Lv2Backend>(new Lv2Backend());
    backend->mKey = ref.key;
    backend->mPlugin = plugin;

    if (LilvNode *name = lilv_plugin_get_name(plugin)) {
        const char *text = lilv_node_as_string(name);
        backend->mName = text ? text : ref.key;
        lilv_node_free(name);
    }
    if (backend->mName.empty())
        backend->mName = ref.key;

    if (const LilvPluginClass *cls = lilv_plugin_get_class(plugin)) {
        if (const LilvNode *label = lilv_plugin_class_get_label(cls)) {
            if (const char *text = lilv_node_as_string(label))
                backend->mCategory = text;
        }
    }

    // lv2:inPlaceBroken is a promise the plug-in makes about aliasing, and LV2 is one of the few
    // formats that states it. Believing it is what lets most LV2 nodes take a single chain slot.
    if (LilvNodes *features = lilv_plugin_get_required_features(plugin)) {
        backend->mInPlaceOk = !lv2NodesContain(features, world->nodes().inPlaceBroken);
        lilv_nodes_free(features);
    }
    if (backend->mInPlaceOk) {
        // It may also appear as a plain property rather than a required feature.
        if (LilvNodes *values = lilv_plugin_get_value(plugin, world->nodes().inPlaceBroken)) {
            backend->mInPlaceOk = (lilv_nodes_size(values) == 0);
            lilv_nodes_free(values);
        }
    }

    if (!backend->readPorts(error))
        return nullptr;

    // An escape hatch, and the only way to exercise the generic-panel path on a machine where every
    // installed plug-in happens to have a usable native UI — which, measured across the 111
    // hostable LV2 plug-ins here, is all of them (72 ui:X11UI, 39 ui:showInterface, none without).
    // It is also the answer for a user whose plug-in UI crashes or misbehaves: the parameters are
    // still there.
    const char *const suppress = std::getenv("NAMPRACK_LV2_NO_NATIVE_UI");
    const bool useNativeUi = !(suppress && suppress[0] && std::strcmp(suppress, "0") != 0);

    // The UI list is read now and held: the LilvUI pointers below belong to it, so freeing it early
    // would dangle them. It is released in teardown().
    backend->mUis = useNativeUi ? lilv_plugin_get_uis(plugin) : nullptr;
    if (backend->mUis) {
        const Lv2Nodes &n = world->nodes();
        LILV_FOREACH(uis, i, backend->mUis)
        {
            const LilvUI *ui = lilv_uis_get(backend->mUis, i);
            if (!ui)
                continue;

            // Native X11 first: suil embeds an X11 UI into an X11 parent with no wrapper module at
            // all, so nothing pulls in a second toolkit. That is the only kind this host can embed.
            if (lilv_ui_is_a(ui, n.uiX11)) {
                backend->mUi = ui;
                backend->mUiType = n.uiX11;
                backend->mEditorKind = EditorKind::Lv2X11Embed;
                break;
            }
        }

        if (!backend->mUi) {
            // Second chance: a UI of any toolkit that implements ui:showInterface opens its OWN
            // window and only needs us to drive idle(). That is the escape hatch the extension was
            // designed for, and it works without linking that toolkit.
            LILV_FOREACH(uis, i, backend->mUis)
            {
                const LilvUI *ui = lilv_uis_get(backend->mUis, i);
                if (!ui)
                    continue;
                const LilvNode *uiUri = lilv_ui_get_uri(ui);
                LilvNode *extensionData = lilv_new_uri(world->world(), LV2_CORE__extensionData);
                const bool shows =
                    uiUri && extensionData &&
                    lilv_world_ask(world->world(), uiUri, extensionData, n.uiShowInterface);
                if (extensionData)
                    lilv_node_free(extensionData);
                if (shows) {
                    backend->mUi = ui;
                    backend->mUiType = lilv_ui_get_uri(ui);
                    backend->mEditorKind = EditorKind::Lv2ShowInterface;
                    break;
                }
            }
        }

        if (!backend->mUi && lilv_uis_size(backend->mUis) > 0) {
            // A UI exists but it is for a toolkit we cannot host — in practice a GtkUI, and suil
            // has no gtk-in-x11 wrapper, so no host without GTK linked can ever embed it. One
            // warning, and the generic panel takes over. This is the documented, permanent
            // limitation in the plan's risk list, not a bug to be fixed later.
            std::fprintf(
                stderr,
                "namp-rack: %s has a plug-in UI this host cannot embed (no ui:X11UI and no "
                "ui:showInterface); using the generic parameter panel instead\n",
                backend->mName.c_str());
        }
    }

    if (backend->mUi)
        backend->mUiFixedSize = false;

    return backend.release();
}

//------------------------------------------------------------------------
// Reads the port layout once, so nothing after this ever has to ask lilv a question. Called with
// the world's lock already held.
bool Lv2Backend::readPorts(std::string &error)
{
    Lv2World *world = Lv2World::instance();
    const Lv2Nodes &n = world->nodes();

    const uint32_t count = lilv_plugin_get_num_ports(mPlugin);
    if (count == 0 || count > 4096) {
        error = "implausible port count";
        return false;
    }

    // lilv fills three parallel arrays, any of which may be left as NAN for a port that declares no
    // such value. Asking for all three in one call is much cheaper than three queries per port.
    std::vector<float> minimums(count, 0.0f);
    std::vector<float> maximums(count, 0.0f);
    std::vector<float> defaults(count, 0.0f);
    lilv_plugin_get_port_ranges_float(mPlugin, minimums.data(), maximums.data(), defaults.data());

    const LilvPort *latencyPort =
        lilv_plugin_get_port_by_designation(mPlugin, n.outputPort, n.latency);

    mPorts.assign(count, Port{});

    for (uint32_t i = 0; i < count; ++i) {
        const LilvPort *port = lilv_plugin_get_port_by_index(mPlugin, i);
        if (!port) {
            error = "lilv could not describe a port";
            return false;
        }

        Port &out = mPorts[i];
        out.index = i;
        out.isInput = lilv_port_is_a(mPlugin, port, n.inputPort);

        if (const LilvNode *symbol = lilv_port_get_symbol(mPlugin, port))
            copyField(out.symbol, sizeof(out.symbol), lilv_node_as_string(symbol));
        if (LilvNode *name = lilv_port_get_name(mPlugin, port)) {
            copyField(out.name, sizeof(out.name), lilv_node_as_string(name));
            lilv_node_free(name);
        }
        if (!out.name[0])
            copyField(out.name, sizeof(out.name), out.symbol);

        if (lilv_port_is_a(mPlugin, port, n.audioPort)) {
            out.kind = Port::Kind::Audio;
            (out.isInput ? mAudioIn : mAudioOut).push_back(i);
            continue;
        }

        if (lilv_port_is_a(mPlugin, port, n.atomPort)) {
            out.kind = Port::Kind::Atom;

            uint32_t size = kMinAtomBufferBytes;
            if (LilvNodes *values = lilv_port_get_value(mPlugin, port, n.rszMinimumSize)) {
                if (const LilvNode *first = lilv_nodes_get_first(values)) {
                    if (lilv_node_is_int(first)) {
                        const int declared = lilv_node_as_int(first);
                        if (declared > 0)
                            size = std::max(size, static_cast<uint32_t>(declared));
                    }
                }
                lilv_nodes_free(values);
            }
            out.bufferSize = std::min(size, kMaxAtomBufferBytes);

            // A port that supports only MIDI is dead weight in a chain that carries no MIDI, but it
            // still needs a valid buffer — a plug-in reading an unconnected atom port is a crash.
            // The flag exists so the UI transfer path can skip it, not so the buffer can be
            // skipped.
            if (LilvNodes *supports = lilv_port_get_value(mPlugin, port, n.atomSupports)) {
                const bool midi = lv2NodesContain(supports, n.midiEvent);
                out.carriesMidiOnly =
                    lilv_nodes_size(supports) > 0 && midi && lilv_nodes_size(supports) == 1;
                // SEPARATE FROM carriesMidiOnly, and it has to be. That flag asks "is this port
                // ONLY MIDI", which is a question about whether the UI transfer path can skip it.
                // This one asks "will this port take MIDI", and the common shape in the wild is a
                // port that supports MIDI *and* patch messages *and* time position — one that
                // carriesMidiOnly is false for and that a footswitch must still reach.
                out.acceptsMidi = midi;
                lilv_nodes_free(supports);
            }
            continue;
        }

        if (lilv_port_is_a(mPlugin, port, n.controlPort)) {
            out.kind = Port::Kind::Control;
            out.isLatency =
                (latencyPort != nullptr) &&
                lilv_port_get_index(mPlugin, port) == lilv_port_get_index(mPlugin, latencyPort);

            const float lo = std::isfinite(minimums[i]) ? minimums[i] : 0.0f;
            const float hi = std::isfinite(maximums[i]) ? maximums[i] : 1.0f;
            out.minimum = lo;
            // A zero-width range makes every normalisation a division by zero. Widening it by one
            // is arbitrary but harmless: the control has exactly one legal value either way.
            out.maximum = (hi > lo) ? hi : lo + 1.0f;
            out.defaultValue = std::isfinite(defaults[i])
                                   ? clampToRange(defaults[i], out.minimum, out.maximum)
                                   : out.minimum;

            out.scaledBySampleRate = lilv_port_has_property(mPlugin, port, n.sampleRate);
            out.isToggled = lilv_port_has_property(mPlugin, port, n.toggled);
            out.isEnumeration = lilv_port_has_property(mPlugin, port, n.enumeration);
            out.notOnGui = lilv_port_has_property(mPlugin, port, n.notOnGui);

            if (out.isToggled) {
                out.stepCount = 2;
            } else if (out.isEnumeration || lilv_port_has_property(mPlugin, port, n.integer)) {
                if (LilvScalePoints *points = lilv_port_get_scale_points(mPlugin, port)) {
                    const unsigned size = lilv_scale_points_size(points);
                    if (size > 1)
                        out.stepCount = static_cast<int32_t>(size);
                    lilv_scale_points_free(points);
                }
                if (out.stepCount == 0) {
                    const double span = static_cast<double>(out.maximum) - out.minimum;
                    if (span > 0.0 && span < 4096.0)
                        out.stepCount = static_cast<int32_t>(span) + 1;
                }
            }

            // Input control ports that are not explicitly hidden are what the generic panel draws.
            if (out.isInput && !out.notOnGui)
                mParams.push_back(i);
            continue;
        }

        out.kind = Port::Kind::Ignored;
    }

    mAudioInPorts = static_cast<int32_t>(mAudioIn.size());
    mAudioOutPorts = static_cast<int32_t>(mAudioOut.size());
    mEffectiveIn = mAudioInPorts;
    mEffectiveOut = mAudioOutPorts;
    if (mAudioInPorts == 0 || mAudioOutPorts == 0) {
        error = "not an audio effect";
        return false;
    }

    mShadow.assign(mParams.size(), 0.0f);
    mUiSent.assign(mParams.size(), std::numeric_limits<float>::quiet_NaN());
    mOutputShadow.assign(mPorts.size(), 0.0f);

    for (size_t p = 0; p < mParams.size(); ++p)
        mShadow[p] = mPorts[mParams[p]].defaultValue;

    return true;
}

//------------------------------------------------------------------------
void Lv2Backend::buildFeatures()
{
    Lv2UridMap &urids = Lv2UridMap::instance();

    mUridAtomChunk = urids.map(LV2_ATOM__Chunk);
    mUridAtomSequence = urids.map(LV2_ATOM__Sequence);
    mUridAtomEventTransfer = urids.map(LV2_ATOM__eventTransfer);
    mUridMidiEvent = urids.map(LV2_MIDI__MidiEvent);
    mUridFloat = urids.map(LV2_ATOM__Float);

    mOptMinBlock = mConfig.maxBlock;
    mOptMaxBlock = mConfig.maxBlock;
    mOptNominalBlock = mConfig.maxBlock;
    mOptSequenceSize = static_cast<int32_t>(kMaxAtomBufferBytes);
    mOptSampleRate = static_cast<float>(mConfig.sampleRate);

    const uint32_t uridInt = urids.map(LV2_ATOM__Int);
    const uint32_t uridFloat = urids.map(LV2_ATOM__Float);

    // minBlockLength == maxBlockLength is the literal truth here and the reason
    // bufsz:fixedBlockLength can be offered: ChainEngine drives every node at exactly one size.
    mOptions.clear();
    mOptions.push_back({LV2_OPTIONS_INSTANCE, 0, urids.map(LV2_BUF_SIZE__minBlockLength),
                        sizeof(int32_t), uridInt, &mOptMinBlock});
    mOptions.push_back({LV2_OPTIONS_INSTANCE, 0, urids.map(LV2_BUF_SIZE__maxBlockLength),
                        sizeof(int32_t), uridInt, &mOptMaxBlock});
    mOptions.push_back({LV2_OPTIONS_INSTANCE, 0, urids.map(LV2_BUF_SIZE__nominalBlockLength),
                        sizeof(int32_t), uridInt, &mOptNominalBlock});
    mOptions.push_back({LV2_OPTIONS_INSTANCE, 0, urids.map(LV2_BUF_SIZE__sequenceSize),
                        sizeof(int32_t), uridInt, &mOptSequenceSize});
    mOptions.push_back({LV2_OPTIONS_INSTANCE, 0, urids.map(LV2_PARAMETERS__sampleRate),
                        sizeof(float), uridFloat, &mOptSampleRate});
    // The array is NULL-terminated by a zeroed entry, which is how the extension defines its end.
    mOptions.push_back({LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr});

    mMapFeature = {LV2_URID__map, urids.mapFeature()};
    mUnmapFeature = {LV2_URID__unmap, urids.unmapFeature()};
    mOptionsFeature = {LV2_OPTIONS__options, mOptions.data()};
    mBoundedBlockFeature = {LV2_BUF_SIZE__boundedBlockLength, nullptr};
    mFixedBlockFeature = {LV2_BUF_SIZE__fixedBlockLength, nullptr};
    mPowerOf2BlockFeature = {LV2_BUF_SIZE__powerOf2BlockLength, nullptr};
    mThreadSafeRestoreFeature = {LV2_STATE__threadSafeRestore, nullptr};

    // mWorkerFeature's data is per-voice and is filled in by buildVoice(); the list is otherwise
    // shared, so each voice gets its own copy of the array at instantiation time.
}

//------------------------------------------------------------------------
bool Lv2Backend::buildVoice(Voice &voice, int32_t channelOffset, std::string &error)
{
    (void)channelOffset;

    Lv2World *world = Lv2World::instance();
    const Lv2Nodes &n = world->nodes();

    // The worker exists before the instance does: schedule_work() may be called from the plug-in's
    // own instantiate(), so the feature has to be valid by then.
    voice.worker = std::make_unique<Lv2Worker>();

    LV2_Feature workerFeature = {LV2_WORKER__schedule, voice.worker->scheduleFeature()};
    const LV2_Feature *features[] = {&mMapFeature,
                                     &mUnmapFeature,
                                     &workerFeature,
                                     &mOptionsFeature,
                                     &mBoundedBlockFeature,
                                     &mFixedBlockFeature,
                                     &mPowerOf2BlockFeature,
                                     &mThreadSafeRestoreFeature,
                                     nullptr};

    voice.instance = lilv_plugin_instantiate(mPlugin, mConfig.sampleRate, features);
    if (!voice.instance) {
        error =
            "the plug-in refused to instantiate at " + std::to_string(mConfig.sampleRate) + " Hz";
        return false;
    }

    // Only now can the worker interface be asked for; before instantiation there is no handle.
    const LV2_Worker_Interface *workerIface = nullptr;
    if (LilvNodes *extData = lilv_plugin_get_extension_data(mPlugin)) {
        if (lv2NodesContain(extData, n.workerInterface)) {
            workerIface = static_cast<const LV2_Worker_Interface *>(
                lilv_instance_get_extension_data(voice.instance, LV2_WORKER__interface));
        }
        lilv_nodes_free(extData);
    }
    voice.worker->start(workerIface, lilv_instance_get_handle(voice.instance), true);

    // Every port gets storage, indexed by port number so nothing needs a second lookup table.
    voice.values.assign(mPorts.size(), 0.0f);
    voice.atoms.assign(mPorts.size(), nullptr);

    for (const Port &port : mPorts) {
        switch (port.kind) {
            case Port::Kind::Control: {
                float value = port.defaultValue;
                // lv2:sampleRate says the declared numbers are in units of the sample rate, so the
                // default has to be scaled before the plug-in ever reads it.
                if (port.scaledBySampleRate)
                    value *= static_cast<float>(mConfig.sampleRate);
                voice.values[port.index] = value;
                lilv_instance_connect_port(voice.instance, port.index, &voice.values[port.index]);
                break;
            }

            case Port::Kind::Atom: {
                LV2_Evbuf *buf = lv2_evbuf_new(port.bufferSize, mUridAtomChunk, mUridAtomSequence);
                if (!buf) {
                    error = "could not allocate an atom buffer";
                    return false;
                }
                lv2_evbuf_reset(buf, port.isInput);
                voice.atoms[port.index] = buf;
                lilv_instance_connect_port(voice.instance, port.index, lv2_evbuf_get_buffer(buf));
                break;
            }

            case Port::Kind::Audio:
                // Connected per block, in connectAudio(). A null here is not a hazard: the plug-in
                // is not run until activate() and prepare() has pointed every audio port somewhere
                // by then.
                lilv_instance_connect_port(voice.instance, port.index, nullptr);
                break;

            case Port::Kind::Ignored:
                // A port we do not understand but that lilv let through as connection-optional. LV2
                // requires it to be connected to null explicitly rather than left untouched.
                lilv_instance_connect_port(voice.instance, port.index, nullptr);
                break;
        }
    }

    return true;
}

//------------------------------------------------------------------------
bool Lv2Backend::prepare(const ProcessConfig &config)
{
    if (!mPlugin)
        return false;

    Lv2World *world = Lv2World::instance();
    if (!world)
        return false;

    // Re-preparing means new instances: an LV2 plug-in's sample rate is fixed at instantiation and
    // there is no equivalent of setupProcessing to change it.
    teardownVoices();

    mConfig = config;
    buildFeatures();

    // How many instantiations the chain's channel count needs. A stereo plug-in in a stereo section
    // is one; a mono plug-in in a stereo section is two, each with its own state — see decision 3
    // in the header. A stereo plug-in in the mono pre-section is one, fed a duplicated input.
    const int32_t chainChannels = std::max<int32_t>(1, config.channels);
    int32_t voiceCount = 1;
    if (mAudioInPorts == 1 && mAudioOutPorts == 1 && chainChannels > 1)
        voiceCount = chainChannels;

    // The scratch path is needed when one voice's port count does not line up with the chain's
    // channels — a stereo-only plug-in in the mono section, most commonly.
    mNeedsScratch =
        (voiceCount == 1) && (mAudioInPorts != chainChannels || mAudioOutPorts != chainChannels);

    // Dual-mono makes the node stereo even though each instance is mono, and the scratch path makes
    // it match the chain by copying. Either way the NODE's channel count is the chain's, and that
    // is what every caller wants to hear about.
    mEffectiveIn = (voiceCount > 1 || mNeedsScratch) ? chainChannels : mAudioInPorts;
    mEffectiveOut = (voiceCount > 1 || mNeedsScratch) ? chainChannels : mAudioOutPorts;

    std::lock_guard<std::mutex> guard(world->lock());

    mVoices.resize(static_cast<size_t>(voiceCount));
    for (int32_t v = 0; v < voiceCount; ++v) {
        std::string error;
        if (!buildVoice(mVoices[static_cast<size_t>(v)], v, error)) {
            std::fprintf(stderr, "namp-rack: %s could not be prepared: %s\n", mName.c_str(),
                         error.c_str());
            teardownVoices();
            return false;
        }
    }

    if (mNeedsScratch) {
        // One block for every port the plug-in has, on both sides. Allocated here, never on the
        // audio path.
        const size_t inChannels = static_cast<size_t>(mAudioInPorts);
        const size_t outChannels = static_cast<size_t>(mAudioOutPorts);
        mScratch.assign((inChannels + outChannels) * static_cast<size_t>(config.maxBlock), 0.0f);
        mScratchIn.resize(inChannels);
        mScratchOut.resize(outChannels);
        for (size_t c = 0; c < inChannels; ++c)
            mScratchIn[c] = mScratch.data() + c * static_cast<size_t>(config.maxBlock);
        for (size_t c = 0; c < outChannels; ++c)
            mScratchOut[c] =
                mScratch.data() + (inChannels + c) * static_cast<size_t>(config.maxBlock);
    } else {
        mScratch.clear();
        mScratchIn.clear();
        mScratchOut.clear();
    }

    // Push whatever the UI already believes into the fresh instances, so re-preparing after a
    // sample rate change does not silently reset every knob to its default.
    for (size_t p = 0; p < mParams.size(); ++p) {
        const Port &port = mPorts[mParams[p]];
        float value = mShadow[p];
        if (port.scaledBySampleRate)
            value *= static_cast<float>(config.sampleRate);
        for (Voice &voice : mVoices)
            voice.values[port.index] = value;
    }

    mToRt.clear();
    mUiToRtAtoms.clear();
    mRtToUiAtoms.clear();
    mPrepared = true;
    return true;
}

//------------------------------------------------------------------------
void Lv2Backend::activate()
{
    if (!mPrepared || mActive)
        return;
    for (Voice &voice : mVoices) {
        if (voice.instance)
            lilv_instance_activate(voice.instance);
    }
    mActive = true;
}

//------------------------------------------------------------------------
void Lv2Backend::deactivate()
{
    if (!mActive)
        return;
    for (Voice &voice : mVoices) {
        if (voice.instance)
            lilv_instance_deactivate(voice.instance);
    }
    mActive = false;
}

//------------------------------------------------------------------------
void Lv2Backend::reset()
{
    // LV2 has no reset. deactivate/activate is the specified way to clear a plug-in's tail, and is
    // what every host does; it is not RT-safe and is never called from the audio thread.
    if (!mActive)
        return;
    deactivate();
    activate();
}

//------------------------------------------------------------------------
void Lv2Backend::connectAudio(Voice &voice, int32_t channelOffset, float *const *in,
                              float *const *out, int32_t channels) noexcept
{
    for (size_t c = 0; c < mAudioIn.size(); ++c) {
        const int32_t source = std::min(channelOffset + static_cast<int32_t>(c), channels - 1);
        lilv_instance_connect_port(voice.instance, mAudioIn[c], in[source]);
    }
    for (size_t c = 0; c < mAudioOut.size(); ++c) {
        const int32_t sink = std::min(channelOffset + static_cast<int32_t>(c), channels - 1);
        lilv_instance_connect_port(voice.instance, mAudioOut[c], out[sink]);
    }
}

//------------------------------------------------------------------------
// AUDIO THREAD. Nothing below allocates, locks, logs or destroys.
void Lv2Backend::process(const AudioBlock &block) noexcept
{
    if (!mActive || mVoices.empty() || block.frames <= 0)
        return;

    const bool timing = diagArmed();
    const auto started =
        timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    // 1. Drain parameter edits. Once per block, before any instance runs, so every voice sees the
    //    same value at the same block boundary and the two channels of a dual-mono pair cannot
    //    diverge.
    {
        ParamEdit edit;
        uint32_t size = 0;
        while (mToRt.read(&edit, sizeof(edit), size)) {
            if (size != sizeof(edit) || edit.param >= mParams.size())
                continue;
            const Port &port = mPorts[mParams[edit.param]];
            for (Voice &voice : mVoices)
                voice.values[port.index] = edit.value;
        }
    }

    // 2. Reset every atom buffer. An input becomes an empty sequence, an output an empty chunk of
    //    the full capacity — that is what tells the plug-in how much room it has to write.
    for (Voice &voice : mVoices) {
        for (const Port &port : mPorts) {
            if (port.kind == Port::Kind::Atom && voice.atoms[port.index])
                lv2_evbuf_reset(voice.atoms[port.index], port.isInput);
        }
    }

    // 3. Deliver anything the UI sent since the last block. Voice 0 only: a dual-mono pair is one
    //    plug-in from the user's point of view and its UI addresses one of them.
    if (mUiToRtPending.load(std::memory_order_acquire)) {
        uint8_t staging[kUiAtomStagingBytes];
        uint32_t size = 0;
        while (mUiToRtAtoms.read(staging, sizeof(staging), size)) {
            if (size < sizeof(uint32_t) + sizeof(LV2_Atom))
                continue;
            uint32_t portIndex = 0;
            std::memcpy(&portIndex, staging, sizeof(portIndex));
            if (portIndex >= mPorts.size())
                continue;
            LV2_Evbuf *buf = mVoices[0].atoms[portIndex];
            if (!buf)
                continue;

            const LV2_Atom *atom = reinterpret_cast<const LV2_Atom *>(staging + sizeof(uint32_t));
            LV2_Evbuf_Iterator iter = lv2_evbuf_end(buf);
            lv2_evbuf_write(&iter, 0, 0, atom->type, atom->size,
                            reinterpret_cast<const uint8_t *>(atom) + sizeof(LV2_Atom));
        }
        mUiToRtPending.store(false, std::memory_order_release);
    }

    // 3b. Deliver this block's MIDI. LV2 has no separate event queue and no parameter door for a
    //     controller: a MIDI message is three bytes written into an atom sequence as an event of
    //     type midi:MidiEvent, timed in frames, which is the whole protocol. Every input port that
    //     accepts MIDI gets every message, and every voice does — a dual-mono pair is two instances
    //     of one pedal and a footswitch means both of them.
    //
    //     lv2_evbuf_write appends and reports failure when the buffer is full; a full buffer drops
    //     the message rather than growing, which is the same bargain the VST3 event list makes.
    if (block.midiCount > 0 && !mPorts.empty()) {
        for (Voice &voice : mVoices) {
            for (const Port &port : mPorts) {
                if (port.kind != Port::Kind::Atom || !port.isInput || !port.acceptsMidi)
                    continue;
                LV2_Evbuf *buf = voice.atoms[port.index];
                if (!buf)
                    continue;
                LV2_Evbuf_Iterator iter = lv2_evbuf_end(buf);
                for (int32_t i = 0; i < block.midiCount; ++i) {
                    const RtMidiEvent &m = block.midi[i];
                    // Running status never reaches here — JACK delivers whole messages — so the
                    // length is decided by the status byte alone. Program Change and Channel
                    // Pressure are two bytes; everything else this host forwards is three.
                    const uint8_t high = m.status & 0xf0;
                    const uint32_t size = (high == 0xc0 || high == 0xd0) ? 2u : 3u;
                    const uint8_t bytes[3] = {m.status, m.data1, m.data2};
                    if (!lv2_evbuf_write(&iter, static_cast<uint32_t>(m.frame), 0, mUridMidiEvent,
                                         size, bytes))
                        break; // full: the rest of this block's messages are dropped
                }
            }
        }
    }

    // 4. Point the audio ports at this block and run.
    if (mNeedsScratch) {
        // The plug-in's channel count does not match the chain's. Feed every input port from the
        // channels we have (duplicating when the plug-in wants more) and take back only as many as
        // the chain carries. This costs what the parent project pays for EVERY plug-in; here it is
        // the exception, and it is why the mono/stereo split in chainmodel.h exists.
        for (size_t c = 0; c < mScratchIn.size(); ++c) {
            const int32_t source = std::min(static_cast<int32_t>(c), block.channels - 1);
            std::memcpy(mScratchIn[c], block.in[source],
                        static_cast<size_t>(block.frames) * sizeof(float));
        }
        connectAudio(mVoices[0], 0, mScratchIn.data(), mScratchOut.data(),
                     static_cast<int32_t>(mScratchIn.size()));
        lilv_instance_run(mVoices[0].instance, static_cast<uint32_t>(block.frames));
        for (int32_t c = 0; c < block.channels; ++c) {
            const size_t source = std::min<size_t>(static_cast<size_t>(c), mScratchOut.size() - 1);
            std::memcpy(block.out[c], mScratchOut[source],
                        static_cast<size_t>(block.frames) * sizeof(float));
        }
    } else {
        for (size_t v = 0; v < mVoices.size(); ++v) {
            connectAudio(mVoices[v], static_cast<int32_t>(v), block.in, block.out, block.channels);
            lilv_instance_run(mVoices[v].instance, static_cast<uint32_t>(block.frames));
        }
    }

    // 5. Close the worker cycle for every voice, in the order the specification requires.
    for (Voice &voice : mVoices) {
        voice.worker->emitResponses();
        voice.worker->endRun();
    }

    // 6. Forward the plug-in's own notifications to its editor, but only while one is open —
    //    otherwise every plug-in with a meter would pay for a ring nobody reads.
    if (mEditorOpen.load(std::memory_order_relaxed)) {
        for (const Port &port : mPorts) {
            if (port.kind != Port::Kind::Atom || port.isInput || port.carriesMidiOnly)
                continue;
            LV2_Evbuf *buf = mVoices[0].atoms[port.index];
            if (!buf)
                continue;
            for (LV2_Evbuf_Iterator iter = lv2_evbuf_begin(buf); lv2_evbuf_is_valid(iter);
                 iter = lv2_evbuf_next(iter)) {
                uint32_t frames = 0, subframes = 0, type = 0, size = 0;
                void *data = nullptr;
                if (!lv2_evbuf_get(iter, &frames, &subframes, &type, &size, &data))
                    break;
                if (sizeof(uint32_t) + sizeof(LV2_Atom) + size > kUiAtomStagingBytes)
                    continue;

                uint8_t staging[kUiAtomStagingBytes];
                std::memcpy(staging, &port.index, sizeof(uint32_t));
                LV2_Atom header = {size, type};
                std::memcpy(staging + sizeof(uint32_t), &header, sizeof(header));
                std::memcpy(staging + sizeof(uint32_t) + sizeof(header), data, size);
                // A full ring drops the notification. That is correct: a UI update is not worth
                // stalling the audio thread for, and the next one supersedes it anyway.
                mRtToUiAtoms.write(staging,
                                   static_cast<uint32_t>(sizeof(uint32_t) + sizeof(header) + size));
            }
        }
    }

    applyLatency();

    if (timing) {
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        int64_t previous = mMaxMicros.load(std::memory_order_relaxed);
        while (micros > previous &&
               !mMaxMicros.compare_exchange_weak(previous, micros, std::memory_order_relaxed)) {
        }
    }
}

//------------------------------------------------------------------------
// AUDIO THREAD. A plug-in reports its latency through an output control port, so it can change at
// any block; the chain reads the atomic and republishes when it moves.
void Lv2Backend::applyLatency() noexcept
{
    for (const Port &port : mPorts) {
        if (!port.isLatency)
            continue;
        const float value = mVoices[0].values[port.index];
        if (std::isfinite(value) && value >= 0.0f && value < 1.0e7f)
            mLatency.store(static_cast<uint32_t>(value), std::memory_order_relaxed);
        return;
    }
}

//------------------------------------------------------------------------
bool Lv2Backend::paramInfo(uint32_t index, ParamInfo &out) const
{
    if (index >= mParams.size())
        return false;
    const Port &port = mPorts[mParams[index]];

    out = ParamInfo{};
    copyField(out.name, sizeof(out.name), port.name);
    out.stepCount = port.stepCount;
    out.defaultNormalized = (port.defaultValue - port.minimum) / (port.maximum - port.minimum);
    // LV2 marks a bypass control with lv2:enabled, whose sense is inverted (1 = running). The rack
    // has its own per-node enable that costs nothing on the audio thread, so this is reported for
    // display only and is not wired to it.
    out.isBypass = false;
    out.isReadOnly = false;
    return true;
}

//------------------------------------------------------------------------
double Lv2Backend::paramGet(uint32_t index) const
{
    if (index >= mParams.size())
        return 0.0;
    const Port &port = mPorts[mParams[index]];
    return (mShadow[index] - port.minimum) / (port.maximum - port.minimum);
}

//------------------------------------------------------------------------
void Lv2Backend::paramSetFromUi(uint32_t index, double normalized)
{
    if (index >= mParams.size())
        return;
    const Port &port = mPorts[mParams[index]];

    const double clamped = normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized);
    float value = static_cast<float>(port.minimum + clamped * (port.maximum - port.minimum));
    if (port.stepCount > 1) {
        const float step = (port.maximum - port.minimum) / static_cast<float>(port.stepCount - 1);
        value = port.minimum + std::round((value - port.minimum) / step) * step;
    }
    mShadow[index] = value;

    if (port.scaledBySampleRate)
        value *= static_cast<float>(mConfig.sampleRate);

    // Enqueued, never applied here. See the threading note in pluginbackend.h.
    const ParamEdit edit{index, value};
    mToRt.write(&edit, sizeof(edit));
}

//------------------------------------------------------------------------
bool Lv2Backend::paramDisplay(uint32_t index, double normalized, char *buf, int32_t bufLen) const
{
    if (index >= mParams.size() || !buf || bufLen <= 0)
        return false;
    const Port &port = mPorts[mParams[index]];

    const double clamped = normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized);
    const float value = static_cast<float>(port.minimum + clamped * (port.maximum - port.minimum));

    if (port.isToggled) {
        std::snprintf(buf, static_cast<size_t>(bufLen), "%s", value >= 0.5f ? "on" : "off");
        return true;
    }

    // An enumeration's scale point label is the only meaningful display for it, and asking lilv is
    // the only way to get one. This is a UI-thread call, so the world lock is fine here.
    if (port.isEnumeration) {
        if (Lv2World *world = Lv2World::instance()) {
            std::lock_guard<std::mutex> guard(world->lock());
            const LilvPort *lilvPort = lilv_plugin_get_port_by_index(mPlugin, port.index);
            if (LilvScalePoints *points = lilv_port_get_scale_points(mPlugin, lilvPort)) {
                float bestDistance = std::numeric_limits<float>::infinity();
                std::string bestLabel;
                LILV_FOREACH(scale_points, i, points)
                {
                    const LilvScalePoint *point = lilv_scale_points_get(points, i);
                    const LilvNode *pv = lilv_scale_point_get_value(point);
                    const LilvNode *pl = lilv_scale_point_get_label(point);
                    if (!pv || !pl || !lilv_node_is_float(pv))
                        continue;
                    const float distance = std::fabs(lilv_node_as_float(pv) - value);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        if (const char *text = lilv_node_as_string(pl))
                            bestLabel = text;
                    }
                }
                lilv_scale_points_free(points);
                if (!bestLabel.empty()) {
                    std::snprintf(buf, static_cast<size_t>(bufLen), "%s", bestLabel.c_str());
                    return true;
                }
            }
        }
    }

    const float span = port.maximum - port.minimum;
    const char *format = (span >= 100.0f) ? "%.0f" : (span >= 10.0f ? "%.1f" : "%.3f");
    std::snprintf(buf, static_cast<size_t>(bufLen), format, static_cast<double>(value));
    return true;
}

//------------------------------------------------------------------------
// Output control ports are plain floats the audio thread writes and nobody synchronises. That is a
// benign race by construction — a float store is atomic on every architecture this builds for, and
// a meter reading one block late is a meter reading one block late. Reporting one change per call
// keeps the caller's loop bounded.
bool Lv2Backend::paramPollFromRt(uint32_t &index, double &normalized)
{
    if (mVoices.empty())
        return false;

    const size_t count = mPorts.size();
    for (size_t scanned = 0; scanned < count; ++scanned) {
        const size_t i = (mPollCursor + scanned) % count;
        const Port &port = mPorts[i];
        if (port.kind != Port::Kind::Control || port.isInput || port.isLatency)
            continue;

        const float value = mVoices[0].values[port.index];
        if (value == mOutputShadow[i])
            continue;
        mOutputShadow[i] = value;
        mPollCursor = static_cast<uint32_t>((i + 1) % count);

        // Output ports are not in mParams — they are not editable — so they are reported by port
        // index with the high bit set, which is what tells the caller not to treat it as a knob.
        index = static_cast<uint32_t>(i) | 0x80000000u;
        normalized = (value - port.minimum) / (port.maximum - port.minimum);
        return true;
    }
    return false;
}

//------------------------------------------------------------------------
int64_t Lv2Backend::diagTakeMaxMicros()
{
    return mMaxMicros.exchange(0, std::memory_order_relaxed);
}

//========================================================================
// State
//
// lilv_state_* is the whole implementation. It collects the plug-in's own state:interface blob if
// it has one, plus every control port value, and serialises the lot to Turtle — which is a text
// format, so a saved rack is readable and diffable rather than a wall of base64. The pair of
// callbacks below is how lilv asks us for port values, because it has no idea where we keep them.
//
// STATE:MAPPATH IS SUPPLIED BY LILV, NOT BY US, AND ONLY WHEN IT HAS A DIRECTORY. Read
// lilv/src/state.c: lilv_state_new_from_instance() builds its own LV2_State_Map_Path (and, when
// save_dir is given, LV2_State_Make_Path) and appends it to whatever feature array it is handed,
// and lilv_state_restore() does the same. So the whole of this host's job is to give lilv a
// directory:
// with copy_dir set, a path the plug-in stores is copied into that directory and abstracted to a
// name relative to it, which is what makes a preset portable to another machine instead of
// carrying an absolute path to a file that is not there.
//
// The asymmetry to know about is on the way back in. lilv_state_new_from_STRING has no directory
// and cannot be given one — state->dir stays null, so a relative path stays relative and the
// plug-in is handed something it cannot open. lilv_state_new_from_FILE derives state->dir from the
// file's parent. Verified in two independent lilv generations: 0.28.0's state.c takes the parent
// path in the non-directory branch of lilv_state_new_from_file, and the older lilv bundled in
// JUCE's LV2_SDK calls lilv_path_parent() unconditionally at the same place. That is why
// stateLoad() below writes the blob to <dir>/state.ttl and reads it back: it is the only lilv entry
// point that sets the directory, and the file it writes is inside a directory this host created
// for exactly this node.
//========================================================================

namespace
{

struct PortValueContext {
    const Lv2Backend *backend = nullptr;
    // Scratch for one value, because the callback returns a pointer lilv reads immediately.
    float value = 0.0f;
};

// The subject every state blob this host writes is serialised under, and therefore the subject it
// must be read back under. It has to be given explicitly on the way in: told no subject,
// lilv_state_new_from_file() guesses, and the two lilv generations guess DIFFERENTLY — 0.28 looks
// for a lone pset:Preset or lv2:Plugin in the file, while the older one uses the file's own URI,
// which is never this. Naming it makes the read version-independent instead of accidentally
// correct on one of them.
constexpr char kStateSubjectUri[] = "http://namp/state";

// lilv_state_to_string() writes CURIEs — "a pset:Preset", "lv2:port" — and writes NO @prefix lines
// with them, because lilv_state_new_from_string() sets the same prefixes on its reader and the two
// are designed as a matched pair. Reading that text as a FILE goes through a plain reader with no
// prefixes at all, and serd stops at "failed to expand CURIE `pset:Preset'".
//
// So the staged file gets the header the string form leaves implicit. The list is lilv's own
// set_prefixes() in state.c, and the four LV2 ones are taken from the LV2 headers rather than
// spelled out, so they cannot drift from what the extension actually says.
constexpr char kStatePrefixes[] = "@prefix atom: <" LV2_ATOM_PREFIX "> .\n"
                                  "@prefix lv2: <" LV2_CORE_PREFIX "> .\n"
                                  "@prefix pset: <" LV2_PRESETS_PREFIX "> .\n"
                                  "@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n"
                                  "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                                  "@prefix state: <" LV2_STATE_PREFIX "> .\n"
                                  "@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .\n";

} // namespace

//------------------------------------------------------------------------
void Lv2Backend::paramFlushToPlugin()
{
    // Writing straight into the voice arrays is safe HERE and nowhere else, for the same reason
    // stateLoad()'s restore callback is: the caller guarantees this node is not in a published
    // chain, so no audio thread is reading them.
    //
    // The ring is drained first and then discarded. Its contents and the shadow cannot disagree —
    // paramSetFromUi writes both — so applying the shadow is applying the ring, and leaving stale
    // entries in it would re-apply them at the next block for no reason.
    {
        ParamEdit edit;
        uint32_t size = 0;
        while (mToRt.read(&edit, sizeof(edit), size)) {
        }
    }

    for (size_t p = 0; p < mParams.size(); ++p) {
        const Port &port = mPorts[mParams[p]];
        float applied = mShadow[p];
        if (port.scaledBySampleRate)
            applied *= static_cast<float>(mConfig.sampleRate);
        for (Voice &voice : mVoices)
            voice.values[port.index] = applied;
    }
}

//------------------------------------------------------------------------
void Lv2Backend::stateSetDirectory(const char *dir)
{
    mStateDir = (dir && *dir) ? dir : "";
}

//------------------------------------------------------------------------
bool Lv2Backend::stateSave(std::vector<uint8_t> &out) const
{
    out.clear();
    if (!mPlugin || mVoices.empty() || !mVoices[0].instance)
        return false;

    Lv2World *world = Lv2World::instance();
    if (!world)
        return false;

    Lv2UridMap &urids = Lv2UridMap::instance();

    // lilv calls this once per port symbol. Returning null for anything that is not a control port
    // is how a port is left out of the snapshot.
    //
    // THE SHADOW IS SAVED, NOT THE LIVE PORT VALUE, and the difference is two separate bugs. The
    // live value is whatever the audio thread last wrote, so a knob the user moved with the
    // transport stopped would not be in the file at all. And for an lv2:sampleRate port the live
    // value has ALREADY been multiplied by the sample rate, so saving it and restoring it — where
    // it is clamped to the port's un-scaled range and then multiplied again — turns a delay time
    // into its own maximum.
    auto getValue = [](const char *symbol, void *userData, uint32_t *size,
                       uint32_t *type) -> const void * {
        auto *ctx = static_cast<PortValueContext *>(userData);
        const Lv2Backend *self = ctx->backend;
        for (size_t p = 0; p < self->mParams.size(); ++p) {
            const Port &port = self->mPorts[self->mParams[p]];
            if (std::strcmp(port.symbol, symbol) != 0)
                continue;
            ctx->value = self->mShadow[p];
            *size = sizeof(float);
            *type = self->mUridFloat;
            return &ctx->value;
        }
        *size = 0;
        *type = 0;
        return nullptr;
    };

    PortValueContext ctx;
    ctx.backend = this;

    std::lock_guard<std::mutex> guard(world->lock());

    // copy_dir and save_dir, but deliberately NOT link_dir. A link is not a copy: a preset whose
    // impulse response is a symlink to somewhere in the user's home directory looks portable and
    // is not. scratch_dir stays null because nothing here is a recording session with temporary
    // files to be promoted.
    const char *dir = mStateDir.empty() ? nullptr : mStateDir.c_str();
    LilvState *state = lilv_state_new_from_instance(
        mPlugin, mVoices[0].instance, urids.mapFeature(),
        /* scratch_dir */ nullptr, /* copy_dir */ dir, /* link_dir */ nullptr,
        /* save_dir */ dir, getValue, &ctx, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
        mFeatures.empty() ? nullptr : mFeatures.data());
    if (!state)
        return false;

    char *text = lilv_state_to_string(world->world(), urids.mapFeature(), urids.unmapFeature(),
                                      state, kStateSubjectUri, nullptr);
    lilv_state_free(state);
    if (!text)
        return false;

    const size_t len = std::strlen(text);
    out.assign(text, text + len);
    // lilv allocated it with its own allocator, so lilv frees it.
    lilv_free(text);
    return true;
}

//------------------------------------------------------------------------
bool Lv2Backend::stateLoad(const uint8_t *data, size_t len)
{
    // A state blob comes out of a project file and is untrusted. The cap is generous for Turtle and
    // still bounds the string this builds.
    constexpr size_t kMaxStateBytes = 16u * 1024u * 1024u;
    if (!data || len == 0 || len > kMaxStateBytes)
        return false;
    if (!mPlugin || mVoices.empty() || !mVoices[0].instance)
        return false;

    Lv2World *world = Lv2World::instance();
    if (!world)
        return false;

    // lilv wants a C string; the blob is not required to carry a terminator.
    std::string text(reinterpret_cast<const char *>(data), len);
    if (text.find('\0') != std::string::npos)
        return false;

    Lv2UridMap &urids = Lv2UridMap::instance();

    // lilv hands each restored port value back through this. Writing straight into the voice arrays
    // is safe HERE and nowhere else: the chain builder only calls stateLoad while the node is not
    // in a published chain, so no audio thread is reading them.
    auto setValue = [](const char *symbol, void *userData, const void *value, uint32_t size,
                       uint32_t type) {
        auto *self = static_cast<Lv2Backend *>(userData);
        if (!value || size != sizeof(float) || type != self->mUridFloat)
            return;
        float incoming = 0.0f;
        std::memcpy(&incoming, value, sizeof(incoming));
        if (!std::isfinite(incoming))
            return;

        for (size_t p = 0; p < self->mParams.size(); ++p) {
            const Port &port = self->mPorts[self->mParams[p]];
            if (std::strcmp(port.symbol, symbol) != 0)
                continue;
            const float clamped = clampToRange(incoming, port.minimum, port.maximum);
            self->mShadow[p] = clamped;
            float applied = clamped;
            if (port.scaledBySampleRate)
                applied *= static_cast<float>(self->mConfig.sampleRate);
            for (Voice &voice : self->mVoices)
                voice.values[port.index] = applied;
            return;
        }
    };

    std::lock_guard<std::mutex> guard(world->lock());

    // With a state directory, go through a file: it is the only lilv entry point that sets
    // state->dir, and without state->dir a relative path in the blob stays relative and the plug-in
    // is handed a name it cannot open. See the section comment above for where that is verified.
    // Without a directory this is exactly the old path, so a rack held only in memory is unchanged.
    LilvState *state = nullptr;
    if (!mStateDir.empty()) {
        const std::string ttl = mStateDir + "/state.ttl";
        std::FILE *file = std::fopen(ttl.c_str(), "wb");
        if (file) {
            const size_t prefixLen = sizeof(kStatePrefixes) - 1;
            const bool wrote = std::fwrite(kStatePrefixes, 1, prefixLen, file) == prefixLen &&
                               std::fwrite(text.data(), 1, text.size(), file) == text.size();
            std::fclose(file);
            if (wrote) {
                LilvNode *subject = lilv_new_uri(world->world(), kStateSubjectUri);
                state = lilv_state_new_from_file(world->world(), urids.mapFeature(), subject,
                                                 ttl.c_str());
                lilv_node_free(subject);
            }
        }
        if (!state)
            std::fprintf(
                stderr,
                "namp-rack: %s: could not stage state through %s; any file its state refers to "
                "will be resolved as an absolute path\n",
                mName.c_str(), ttl.c_str());
    }
    if (!state)
        state = lilv_state_new_from_string(world->world(), urids.mapFeature(), text.c_str());
    if (!state) {
        std::fprintf(stderr, "namp-rack: %s state could not be parsed and was ignored\n",
                     mName.c_str());
        return false;
    }

    // Restoring can make the plug-in schedule work and expect the answer before it reports itself
    // restored, with no run() cycle in between. Switching the worker to synchronous for the
    // duration is what jalv does and is safe because this is never the audio thread — see
    // lv2worker.h.
    for (Voice &voice : mVoices) {
        if (!voice.instance)
            continue;
        const LV2_Worker_Interface *iface = static_cast<const LV2_Worker_Interface *>(
            lilv_instance_get_extension_data(voice.instance, LV2_WORKER__interface));
        voice.worker->stop();
        voice.worker->start(iface, lilv_instance_get_handle(voice.instance), false);

        lilv_state_restore(state, voice.instance, setValue, this, 0,
                           mFeatures.empty() ? nullptr : mFeatures.data());

        voice.worker->stop();
        voice.worker->start(iface, lilv_instance_get_handle(voice.instance), true);
    }

    lilv_state_free(state);
    return true;
}

//========================================================================
// Editor
//
// For an X11 UI, suil does no wrapping at all: its instance.c only loads a wrapper module when the
// container type and the UI type differ, so X11-in-X11 loads nothing and links no toolkit. What
// suil still does is resolve the binary, look up the descriptor, assemble the feature array and
// clean up afterwards — small, but it is the part that dlopens a stranger's shared object, and
// keeping that out of our code is worth the dependency.
//========================================================================

//------------------------------------------------------------------------
void Lv2Backend::uiWrite(void *controller, uint32_t portIndex, uint32_t bufferSize,
                         uint32_t protocol, const void *buffer)
{
    auto *self = static_cast<Lv2Backend *>(controller);
    if (!self || !buffer || portIndex >= self->mPorts.size())
        return;

    // Protocol 0 is the default: a bare float for a control port.
    if (protocol == 0) {
        if (bufferSize != sizeof(float))
            return;
        float value = 0.0f;
        std::memcpy(&value, buffer, sizeof(value));
        if (!std::isfinite(value))
            return;

        // Find which parameter this port is, so the edit takes the same path as one from the
        // generic panel — one ring, one drain point, no second way for a value to reach the
        // plug-in.
        for (size_t p = 0; p < self->mParams.size(); ++p) {
            if (self->mParams[p] != portIndex)
                continue;
            const Port &port = self->mPorts[portIndex];
            const double normalized =
                (value - port.minimum) / static_cast<double>(port.maximum - port.minimum);
            self->paramSetFromUi(static_cast<uint32_t>(p), normalized);
            return;
        }
        return;
    }

    if (protocol != self->mUridAtomEventTransfer)
        return;
    if (bufferSize < sizeof(LV2_Atom) || sizeof(uint32_t) + bufferSize > kUiAtomStagingBytes)
        return;

    uint8_t staging[kUiAtomStagingBytes];
    std::memcpy(staging, &portIndex, sizeof(portIndex));
    std::memcpy(staging + sizeof(portIndex), buffer, bufferSize);
    if (self->mUiToRtAtoms.write(staging, static_cast<uint32_t>(sizeof(portIndex) + bufferSize)))
        self->mUiToRtPending.store(true, std::memory_order_release);
}

//------------------------------------------------------------------------
uint32_t Lv2Backend::uiPortIndex(void *controller, const char *symbol)
{
    auto *self = static_cast<Lv2Backend *>(controller);
    if (!self || !symbol)
        return LV2UI_INVALID_PORT_INDEX;
    for (const Port &port : self->mPorts) {
        if (std::strcmp(port.symbol, symbol) == 0)
            return port.index;
    }
    return LV2UI_INVALID_PORT_INDEX;
}

//------------------------------------------------------------------------
void Lv2Backend::uiTouch(void *controller, uint32_t portIndex, bool grabbed)
{
    // Touch marks the start and end of a gesture, which matters to a host that records automation.
    // This one does not, so it is accepted and ignored rather than left unimplemented — a UI that
    // requires the feature would otherwise refuse to instantiate.
    (void)controller;
    (void)portIndex;
    (void)grabbed;
}

//------------------------------------------------------------------------
int Lv2Backend::uiResize(LV2UI_Feature_Handle handle, int width, int height)
{
    auto *self = static_cast<Lv2Backend *>(handle);
    if (!self || width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return 1;

    // LATCH ONLY. A UI may call this from its own thread, or from inside its instantiate() before
    // we even have a window on screen. Making an Xlib call from here would be a call on the wrong
    // thread at an arbitrary moment; the run loop picks the request up on its next tick.
    self->mResizeW.store(width, std::memory_order_relaxed);
    self->mResizeH.store(height, std::memory_order_relaxed);
    self->mResizePending.store(true, std::memory_order_release);
    return 0;
}

//------------------------------------------------------------------------
bool Lv2Backend::editorOpen(const EditorOpenRequest &request, EditorSurface &out)
{
    out = EditorSurface{};
    if (!mUi || mEditorKind == EditorKind::NoEditor || mVoices.empty())
        return false;
    if (mSuilInstance)
        return false; // already open; close it first

    Lv2World *world = Lv2World::instance();
    if (!world)
        return false;

    const LilvNode *uiUri = lilv_ui_get_uri(mUi);
    const LilvNode *bundleUri = lilv_ui_get_bundle_uri(mUi);
    const LilvNode *binaryUri = lilv_ui_get_binary_uri(mUi);
    if (!uiUri || !bundleUri || !binaryUri)
        return false;

    char *bundlePath = lilv_file_uri_parse(lilv_node_as_uri(bundleUri), nullptr);
    char *binaryPath = lilv_file_uri_parse(lilv_node_as_uri(binaryUri), nullptr);
    if (!bundlePath || !binaryPath) {
        lilv_free(bundlePath);
        lilv_free(binaryPath);
        return false;
    }

    mUiParentFeature = {LV2_UI__parent, reinterpret_cast<void *>(request.parentWindow)};
    mUiResize.handle = this;
    mUiResize.ui_resize = &Lv2Backend::uiResize;
    mUiResizeFeature = {LV2_UI__resize, &mUiResize};
    mUiIdleFeature = {LV2_UI__idleInterface, nullptr};

    mUiFeatures.clear();
    mUiFeatures.push_back(&mMapFeature);
    mUiFeatures.push_back(&mUnmapFeature);
    mUiFeatures.push_back(&mOptionsFeature);
    mUiFeatures.push_back(&mUiIdleFeature);
    mUiFeatures.push_back(&mUiResizeFeature);
    // A showInterface UI opens its own top-level window and must NOT be given a parent, or a
    // toolkit that honours the feature will reparent itself into a window it does not own.
    if (mEditorKind == EditorKind::Lv2X11Embed)
        mUiFeatures.push_back(&mUiParentFeature);
    mUiFeatures.push_back(nullptr);

    if (!mSuilHost) {
        mSuilHost = suil_host_new(&Lv2Backend::uiWrite, &Lv2Backend::uiPortIndex, nullptr, nullptr);
        if (!mSuilHost) {
            lilv_free(bundlePath);
            lilv_free(binaryPath);
            return false;
        }
        suil_host_set_touch_func(mSuilHost, &Lv2Backend::uiTouch);
    }

    const char *containerType =
        (mEditorKind == EditorKind::Lv2X11Embed) ? LV2_UI__X11UI : lilv_node_as_uri(mUiType);
    const char *uiType =
        (mEditorKind == EditorKind::Lv2X11Embed) ? LV2_UI__X11UI : lilv_node_as_uri(mUiType);

    {
        std::lock_guard<std::mutex> guard(world->lock());
        mSuilInstance = suil_instance_new(
            mSuilHost, this, containerType, lilv_node_as_uri(lilv_plugin_get_uri(mPlugin)),
            lilv_node_as_uri(uiUri), uiType, bundlePath, binaryPath, mUiFeatures.data());
    }

    lilv_free(bundlePath);
    lilv_free(binaryPath);

    if (!mSuilInstance) {
        std::fprintf(stderr,
                     "namp-rack: %s refused to open its own editor; use the generic panel\n",
                     mName.c_str());
        return false;
    }

    mUiIdle = static_cast<const LV2UI_Idle_Interface *>(
        suil_instance_extension_data(mSuilInstance, LV2_UI__idleInterface));
    mUiShow = static_cast<const LV2UI_Show_Interface *>(
        suil_instance_extension_data(mSuilInstance, LV2_UI__showInterface));

    if (mEditorKind == EditorKind::Lv2ShowInterface && !mUiShow) {
        // The Turtle claimed showInterface and the binary does not provide it. Refusing here is
        // better than leaving an invisible window the user cannot find.
        std::fprintf(stderr, "namp-rack: %s declares ui:showInterface but does not implement it\n",
                     mName.c_str());
        editorClose();
        return false;
    }

    out.kind = mEditorKind;
    if (mEditorKind == EditorKind::Lv2X11Embed) {
        out.childWindow = reinterpret_cast<uintptr_t>(suil_instance_get_widget(mSuilInstance));
        if (!out.childWindow) {
            editorClose();
            return false;
        }
    }
    // A UI that has not yet called ui:resize has no size to report; the caller falls back to its
    // own default and the latched request arrives on the next tick.
    if (mResizePending.load(std::memory_order_acquire)) {
        out.width = mResizeW.load(std::memory_order_relaxed);
        out.height = mResizeH.load(std::memory_order_relaxed);
        mResizePending.store(false, std::memory_order_release);
    }
    out.resizable = !mUiFixedSize;

    // Tell the UI where every control currently stands. Without this a freshly opened editor shows
    // its own defaults rather than the values the plug-in is actually running.
    std::fill(mUiSent.begin(), mUiSent.end(), std::numeric_limits<float>::quiet_NaN());
    mEditorOpen.store(true, std::memory_order_release);
    editorIdle();
    return true;
}

//------------------------------------------------------------------------
void Lv2Backend::editorIdle()
{
    if (!mSuilInstance)
        return;

    // 1. Push any control value that has moved since the last tick. The comparison is against what
    //    the UI was last told, not against a previous poll, so a value that changes and changes
    //    back between ticks correctly sends nothing.
    for (size_t p = 0; p < mParams.size(); ++p) {
        const float value = mShadow[p];
        if (value == mUiSent[p])
            continue;
        mUiSent[p] = value;
        suil_instance_port_event(mSuilInstance, mParams[p], sizeof(float), 0, &value);
    }

    // 2. Deliver whatever the plug-in sent its UI. Bounded, because this runs on the run loop and a
    //    chatty plug-in must not be able to stall the interface.
    for (int guard = 0; guard < 64; ++guard) {
        uint8_t staging[kUiAtomStagingBytes];
        uint32_t size = 0;
        if (!mRtToUiAtoms.read(staging, sizeof(staging), size))
            break;
        if (size < sizeof(uint32_t) + sizeof(LV2_Atom))
            continue;
        uint32_t portIndex = 0;
        std::memcpy(&portIndex, staging, sizeof(portIndex));
        const uint32_t atomBytes = size - static_cast<uint32_t>(sizeof(uint32_t));
        suil_instance_port_event(mSuilInstance, portIndex, atomBytes, mUridAtomEventTransfer,
                                 staging + sizeof(uint32_t));
    }

    // 3. A UI with an idle interface must be driven, and a showInterface UI that returns non-zero
    // is
    //    telling us the user closed its window.
    if (mUiIdle && mUiIdle->idle) {
        const int wantsClose = mUiIdle->idle(suil_instance_get_handle(mSuilInstance));
        if (wantsClose != 0 && mEditorKind == EditorKind::Lv2ShowInterface)
            editorHide();
    }
}

//------------------------------------------------------------------------
bool Lv2Backend::editorTakeResizeRequest(int32_t &w, int32_t &h)
{
    if (!mResizePending.exchange(false, std::memory_order_acq_rel))
        return false;
    w = mResizeW.load(std::memory_order_relaxed);
    h = mResizeH.load(std::memory_order_relaxed);
    return w > 0 && h > 0;
}

//------------------------------------------------------------------------
bool Lv2Backend::editorCheckSize(int32_t &w, int32_t &h) const
{
    // LV2 has no equivalent of IPlugView::checkSizeConstraint: a UI states ui:noUserResize or it
    // does not, and there is no way to ask it about a particular rectangle. Reporting no opinion is
    // therefore the truthful answer, and the fixed-size case is handled by the window manager hints
    // the caller sets from EditorSurface::resizable.
    (void)w;
    (void)h;
    return false;
}

//------------------------------------------------------------------------
void Lv2Backend::editorSetSize(int32_t w, int32_t h)
{
    // Only meaningful for an embedded UI, and only as a courtesy: the widget is a child of a window
    // the caller already resized, so this tells the UI to re-lay out inside it.
    if (!mSuilInstance || mEditorKind != EditorKind::Lv2X11Embed || w <= 0 || h <= 0)
        return;
    if (const LV2UI_Resize *resize = static_cast<const LV2UI_Resize *>(
            suil_instance_extension_data(mSuilInstance, LV2_UI__resize))) {
        if (resize->ui_resize)
            resize->ui_resize(resize->handle, w, h);
    }
}

//------------------------------------------------------------------------
void Lv2Backend::editorShow()
{
    if (mSuilInstance && mUiShow && mUiShow->show)
        mUiShow->show(suil_instance_get_handle(mSuilInstance));
}

//------------------------------------------------------------------------
void Lv2Backend::editorHide()
{
    if (mSuilInstance && mUiShow && mUiShow->hide)
        mUiShow->hide(suil_instance_get_handle(mSuilInstance));
}

//------------------------------------------------------------------------
void Lv2Backend::editorClose()
{
    // Stop the RT -> UI copy BEFORE the instance goes, so the audio thread cannot be filling a ring
    // for a UI that is being freed. The ring itself is fine either way — it is just bytes — but the
    // flag is what makes the ordering explicit rather than accidental.
    mEditorOpen.store(false, std::memory_order_release);

    if (mSuilInstance) {
        if (mEditorKind == EditorKind::Lv2ShowInterface)
            editorHide();
        suil_instance_free(mSuilInstance);
        mSuilInstance = nullptr;
    }
    if (mSuilHost) {
        suil_host_free(mSuilHost);
        mSuilHost = nullptr;
    }
    mUiIdle = nullptr;
    mUiShow = nullptr;
    mRtToUiAtoms.clear();
    mResizePending.store(false, std::memory_order_relaxed);
}

} // namespace NAMp::host
