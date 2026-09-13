// SPDX-License-Identifier: MIT
//
// rations_lv2check — what a host actually sees in the INSTALLED rations.lv2, and whether it runs.
//
// Every check here is one the ordinary build cannot make, because the questions are about a
// bundle rather than about a binary: whether the Turtle parses, whether the manifest offers the
// UI, whether the port declarations are the ones the code expects, and whether a real discovery
// library can load and run the result. lilv is the reference implementation of all of that, so
// asking lilv is as close to asking a host as this machine gets without opening one.
//
// It is ported from the author's own lvtuner tools/lv2check.c and grown into a gate rather than a
// printout, because the failures it is looking for are all silent at runtime:
//
//   * a UI present in the plug-in description and missing from the MANIFEST, which most hosts
//     never offer;
//   * a missing ui:portNotification, without which the meters, the bank progress bar and the
//     capture readout are permanently dead while everything else works;
//   * a port table that has drifted from the code's own — caught at the TTL by rations_ttlgen,
//     and caught HERE the other way round, through whatever the RDF actually says;
//   * a bundle that describes itself perfectly and whose binary will not load.
//
// It runs headless, needs no captures and needs no audio server. The optional UI check needs an X
// display and skips itself without one.
//
// Usage: rations_lv2check [bundle directory]
// With no argument it searches the host's ordinary LV2 path, which is what a DAW does.

#include "lv2/lv2message.h"
#include "lv2/rationslv2.h"

#include <lilv/lilv.h>

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(RATIONS_LV2CHECK_NO_X11)
#include <X11/Xlib.h>
#endif

using namespace Steinberg;
using namespace Rations;
using namespace Rations::lv2;

namespace
{

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const char *what)
{
    ++gChecks;
    if (ok)
        return;
    ++gFailures;
    printf("  FAIL  %s\n", what);
}

void checkf(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void checkf(bool ok, const char *fmt, ...)
{
    ++gChecks;
    if (ok)
        return;
    ++gFailures;
    fputs("  FAIL  ", stdout);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputc('\n', stdout);
}

//------------------------------------------------------------------------
// The smallest urid:map a plug-in can be given: an array of strings and a linear scan. This is a
// test rig, not a host, so nothing here has to be fast — and a map that is obviously correct is
// worth more in a gate than one that is clever.
struct UridMap {
    std::vector<std::string> uris;

    static LV2_URID mapUri(LV2_URID_Map_Handle handle, const char *uri)
    {
        auto *self = static_cast<UridMap *>(handle);
        for (size_t i = 0; i < self->uris.size(); ++i)
            if (self->uris[i] == uri)
                return static_cast<LV2_URID>(i + 1);
        self->uris.emplace_back(uri);
        return static_cast<LV2_URID>(self->uris.size());
    }
};

//------------------------------------------------------------------------
bool nodeIsA(LilvWorld *world, const LilvNode *subject, const char *predicate, const char *object)
{
    LilvNode *p = lilv_new_uri(world, predicate);
    LilvNode *o = lilv_new_uri(world, object);
    const bool found = lilv_world_ask(world, subject, p, o);
    lilv_node_free(o);
    lilv_node_free(p);
    return found;
}

//------------------------------------------------------------------------
// The port declarations, read back out of the RDF and compared with the table the code compiles
// against. rations_ttlgen checks the code against the controller; this checks the BUNDLE against
// the code, so a stale TTL beside a fresh binary — the commonest way an LV2 bundle goes wrong —
// cannot pass.
void checkPorts(LilvWorld *world, const LilvPlugin *plugin)
{
    LilvNode *classAudio = lilv_new_uri(world, LV2_CORE__AudioPort);
    LilvNode *classControl = lilv_new_uri(world, LV2_CORE__ControlPort);
    LilvNode *classAtom = lilv_new_uri(world, LV2_ATOM__AtomPort);
    LilvNode *classInput = lilv_new_uri(world, LV2_CORE__InputPort);

    const uint32_t count = lilv_plugin_get_num_ports(plugin);
    checkf(count == kPortCount, "the bundle declares %u ports; the code expects %u", count,
           static_cast<unsigned>(kPortCount));

    for (uint32_t i = 0; i < count && i < kPortCount; ++i) {
        const LilvPort *port = lilv_plugin_get_port_by_index(plugin, i);
        if (!port) {
            checkf(false, "port %u is missing", i);
            continue;
        }
        const bool isInput = lilv_port_is_a(plugin, port, classInput);

        if (i <= kPortAudioOutR) {
            checkf(lilv_port_is_a(plugin, port, classAudio), "port %u should be audio", i);
            checkf(isInput == (i == kPortAudioIn), "port %u has the wrong direction", i);
            continue;
        }
        if (i == kPortAtomIn || i == kPortAtomOut) {
            checkf(lilv_port_is_a(plugin, port, classAtom), "port %u should be an atom port", i);
            checkf(isInput == (i == kPortAtomIn), "port %u has the wrong direction", i);
            continue;
        }
        if (i >= kPortControlFirst && i < kPortFeedbackFirst) {
            const int index = static_cast<int>(i - kPortControlFirst);
            const ControlSpec spec = controlSpec(index);
            checkf(lilv_port_is_a(plugin, port, classControl), "port %u should be a control port",
                   i);
            checkf(isInput, "control port %u should be an input", i);

            LilvNode *def = nullptr;
            LilvNode *min = nullptr;
            LilvNode *max = nullptr;
            lilv_port_get_range(plugin, port, &def, &min, &max);
            const double tolerance = 1e-4; // the TTL is written to six decimal places
            checkf(min && std::fabs(lilv_node_as_float(min) - spec.min) < tolerance,
                   "port %u minimum is %g; the code says %g", i,
                   min ? lilv_node_as_float(min) : 0.0, spec.min);
            checkf(max && std::fabs(lilv_node_as_float(max) - spec.max) < tolerance,
                   "port %u maximum is %g; the code says %g", i,
                   max ? lilv_node_as_float(max) : 0.0, spec.max);
            checkf(def && std::fabs(lilv_node_as_float(def) - spec.def) < tolerance,
                   "port %u default is %g; the code says %g", i,
                   def ? lilv_node_as_float(def) : 0.0, spec.def);
            lilv_node_free(def);
            lilv_node_free(min);
            lilv_node_free(max);
            continue;
        }
        // The feedback block and the latency port.
        checkf(lilv_port_is_a(plugin, port, classControl), "port %u should be a control port", i);
        checkf(!isInput, "port %u should be an output", i);
    }

    lilv_node_free(classInput);
    lilv_node_free(classAtom);
    lilv_node_free(classControl);
    lilv_node_free(classAudio);
}

//------------------------------------------------------------------------
// The UI, and above all its portNotification blocks. This is the single most likely silent failure
// in an LV2 UI project: without them a host sends the UI nothing for an output control port, and
// the meters and the capture readout never move while the plug-in loads, plays and otherwise works
// perfectly.
void checkUi(LilvWorld *world, const LilvPlugin *plugin)
{
    LilvUIs *uis = lilv_plugin_get_uis(plugin);
    const unsigned uiCount = uis ? lilv_uis_size(uis) : 0;
    checkf(uiCount == 1, "the bundle offers %u UIs; it should offer exactly one", uiCount);
    if (!uis || uiCount == 0) {
        if (uis)
            lilv_uis_free(uis);
        return;
    }

    const LilvUI *ui = lilv_uis_get(uis, lilv_uis_begin(uis));
    const LilvNode *uiUri = lilv_ui_get_uri(ui);
    checkf(uiUri && std::strcmp(lilv_node_as_string(uiUri), kUiUri) == 0,
           "the UI URI is %s; the code expects %s", uiUri ? lilv_node_as_string(uiUri) : "?",
           kUiUri);

    LilvNode *x11 = lilv_new_uri(world, LV2_UI__X11UI);
    check(lilv_ui_is_a(ui, x11), "the UI is not declared as a ui:X11UI");
    lilv_node_free(x11);

    check(lilv_ui_get_binary_uri(ui) != nullptr, "the UI declares no binary");

    // Both directions, and each one on its own: a UI with the feature and not the extension data
    // never runs, and one with the extension data and not the feature is never driven.
    check(nodeIsA(world, uiUri, LV2_CORE__requiredFeature, LV2_UI__idleInterface),
          "ui:idleInterface is not a required FEATURE of the UI");
    check(nodeIsA(world, uiUri, LV2_CORE__extensionData, LV2_UI__idleInterface),
          "ui:idleInterface is not declared as EXTENSION DATA of the UI");

    // Every port the UI has to be told about.
    LilvNode *notify = lilv_new_uri(world, LV2_UI__portNotification);
    LilvNode *indexPred = lilv_new_uri(world, LV2_UI__portIndex);
    LilvNodes *blocks = lilv_world_find_nodes(world, uiUri, notify, nullptr);
    std::vector<int> notified;
    if (blocks) {
        LILV_FOREACH(nodes, n, blocks)
        {
            const LilvNode *block = lilv_nodes_get(blocks, n);
            LilvNodes *idx = lilv_world_find_nodes(world, block, indexPred, nullptr);
            if (idx && lilv_nodes_size(idx) > 0)
                notified.push_back(lilv_node_as_int(lilv_nodes_get_first(idx)));
            lilv_nodes_free(idx);
        }
        lilv_nodes_free(blocks);
    }
    lilv_node_free(indexPred);
    lilv_node_free(notify);

    const auto isNotified = [&notified](int index) {
        return std::find(notified.begin(), notified.end(), index) != notified.end();
    };
    checkf(isNotified(static_cast<int>(kPortAtomOut)),
           "no ui:portNotification for the notify port (%u): the capture names, the MIDI table and "
           "the plug-in's state would never reach the editor",
           static_cast<unsigned>(kPortAtomOut));
    for (int f = 0; f < kFeedbackCount; ++f) {
        const int index = static_cast<int>(kPortFeedbackFirst) + f;
        checkf(isNotified(index),
               "no ui:portNotification for output port %d: that readout would be permanently dead",
               index);
    }

    lilv_uis_free(uis);
}

//------------------------------------------------------------------------
// Getting and setting port values for the state round trip. The host owns control port values, so
// a saved state records them; these two are how lilv asks.
float gPortValues[kPortCount] = {};

const void *getPortValue(const char *symbol, void *user, uint32_t *size, uint32_t *type)
{
    auto *floatUrid = static_cast<LV2_URID *>(user);
    (void)symbol;
    *size = sizeof(float);
    *type = *floatUrid;
    // One value for every port is enough for a round trip; which port it came from does not
    // matter here, because what is under test is the plug-in's own state:interface and not lilv's
    // port bookkeeping.
    static float value = 0.0f;
    return &value;
}

//------------------------------------------------------------------------
// The atom sequence buffers, and the two things a host does with them every block: reset the
// output port's size field to its CAPACITY, and leave the input's holding only what it put there.
struct Sequences {
    std::vector<uint8_t> in = std::vector<uint8_t>(8192, 0);
    std::vector<uint8_t> out = std::vector<uint8_t>(65536, 0);
    LV2_URID sequenceType = 0;

    LV2_Atom_Sequence *input()
    {
        return reinterpret_cast<LV2_Atom_Sequence *>(in.data());
    }
    LV2_Atom_Sequence *output()
    {
        return reinterpret_cast<LV2_Atom_Sequence *>(out.data());
    }

    void clearInput()
    {
        LV2_Atom_Sequence *seq = input();
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        seq->atom.type = sequenceType;
        seq->body.unit = 0;
        seq->body.pad = 0;
    }
    void resetOutput()
    {
        output()->atom.size = static_cast<uint32_t>(out.size() - sizeof(LV2_Atom));
    }
};

// Write one VST3 message into the input sequence, the way the UI half does. The codec is the
// plug-in's OWN — lv2/lv2message.cpp, linked here rather than restated — so this exercises the
// wire format rather than a second description of it.
bool sendMessage(Sequences &seq, LV2_Atom_Forge &forge, const MessageUris &uris, Message &message)
{
    seq.clearInput();
    LV2_Atom_Sequence *sequence = seq.input();
    // Forge straight past the sequence header that clearInput just wrote.
    uint8_t *tail = reinterpret_cast<uint8_t *>(sequence) + lv2_atom_total_size(&sequence->atom);
    const size_t room = seq.in.size() - static_cast<size_t>(tail - seq.in.data());
    lv2_atom_forge_set_buffer(&forge, tail, static_cast<uint32_t>(room));

    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_sequence_head(&forge, &frame, 0);
    if (!lv2_atom_forge_frame_time(&forge, 0))
        return false;
    if (!forgeMessage(forge, uris, &message, message.attributes()))
        return false;
    lv2_atom_forge_pop(&forge, &frame);

    // The events the forge just wrote sit immediately after our own header, so the sequence's
    // size is its body plus theirs, less the header the forge wrote for its own sequence.
    const auto *forged = reinterpret_cast<const LV2_Atom_Sequence *>(tail);
    sequence->atom.size = static_cast<uint32_t>(sizeof(LV2_Atom_Sequence_Body) + forged->atom.size -
                                                sizeof(LV2_Atom_Sequence_Body));
    return true;
}

//------------------------------------------------------------------------
// Load a real bank into channel Clean, wait for it to build, and prove the LV2 build makes sound.
//
// This is the one check that exercises the whole path end to end: an editor message goes in as an
// atom, the wrapper's message thread hands it to the processor, four worker threads build the
// bank, the processor answers with its capability report, that comes back out of the notify port
// as an atom, and audio comes out of the audio ports. Nothing short of it would show that any of
// those seams is joined.
//
// It needs captures, which this plug-in does not ship (they are not ours to distribute), so it is
// asked for explicitly and skipped with a line rather than silently passing.
void checkCaptures(const LilvPlugin *plugin, const std::string &captureDir)
{
    UridMap map;
    LV2_URID_Map mapFeature = {&map, UridMap::mapUri};
    LV2_Feature mapItem = {LV2_URID__map, &mapFeature};
    const LV2_Feature *features[] = {&mapItem, nullptr};

    MessageUris uris;
    uris.map(&mapFeature);
    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &mapFeature);

    LilvInstance *instance = lilv_plugin_instantiate(plugin, 48000.0, features);
    check(instance != nullptr, "the plug-in did not instantiate for the capture test");
    if (!instance)
        return;

    constexpr uint32_t kBlock = 128;
    std::vector<float> audioIn(kBlock, 0.0f);
    std::vector<float> outL(kBlock, 0.0f);
    std::vector<float> outR(kBlock, 0.0f);
    Sequences seq;
    seq.sequenceType = UridMap::mapUri(&map, LV2_ATOM__Sequence);
    seq.clearInput();
    seq.resetOutput();

    lilv_instance_connect_port(instance, kPortAudioIn, audioIn.data());
    lilv_instance_connect_port(instance, kPortAudioOutL, outL.data());
    lilv_instance_connect_port(instance, kPortAudioOutR, outR.data());
    lilv_instance_connect_port(instance, kPortAtomIn, seq.input());
    lilv_instance_connect_port(instance, kPortAtomOut, seq.output());
    for (int i = 0; i < kControlInCount; ++i) {
        gPortValues[kPortControlFirst + i] = static_cast<float>(controlSpec(i).def);
        lilv_instance_connect_port(instance, kPortControlFirst + i,
                                   &gPortValues[kPortControlFirst + i]);
    }
    for (uint32_t i = kPortFeedbackFirst; i < kPortCount; ++i)
        lilv_instance_connect_port(instance, i, &gPortValues[i]);
    lilv_instance_activate(instance);

    const std::string bank = captureDir + "/Clean";
    Message load;
    load.setMessageID(kMsgLoadCapture[0]);
    load.attributes().setBinary(kMsgPathAttr, bank.data(), static_cast<uint32_t>(bank.size()));
    load.attributes().setInt(kMsgIsDirAttr, 1);
    check(sendMessage(seq, forge, uris, load), "could not forge the capture load message");

    seq.resetOutput();
    lilv_instance_run(instance, kBlock);
    seq.clearInput();

    // Wait for the bank, by ASKING repeatedly rather than by watching the progress port.
    //
    // Two reasons, and the second is the interesting one. The capability report a load sends by
    // itself necessarily says the bank is empty — the workers have built nothing at the moment the
    // load is acknowledged — so the report that carries the real count is the reply to a REQUEST,
    // which is exactly why kMsgRequestCaps exists and exactly what the editor's own pollCaps does.
    // And bank progress is a fraction of work done, which for a rack with nothing loaded is 1.0
    // already: it says a build has finished, never that one happened.
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    bool built = false;
    int capsReplies = 0;
    int reportedEntries = 0;
    auto nextPoll = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline && !built) {
        if (std::chrono::steady_clock::now() >= nextPoll) {
            Message poll;
            poll.setMessageID(kMsgRequestCaps);
            sendMessage(seq, forge, uris, poll);
            nextPoll = std::chrono::steady_clock::now() + 100ms;
        }
        seq.resetOutput();
        lilv_instance_run(instance, kBlock);
        seq.clearInput();

        // Anything the plug-in sent back. kMsgModelCaps is the one that matters: it is what tells
        // an editor how many captures the dial has to sweep and what they are called.
        LV2_ATOM_SEQUENCE_FOREACH(seq.output(), ev)
        {
            const LV2_Atom *atom = &ev->body;
            if (!lv2_atom_forge_is_object_type(&forge, atom->type))
                continue;
            Message *reply = parseMessage(reinterpret_cast<const LV2_Atom_Object *>(atom), uris);
            if (!reply)
                continue;
            if (reply->id() == kMsgModelCaps) {
                ++capsReplies;
                const std::string attr = std::string(kCapsEntryCountAttr) + kChannelDefaultName[0];
                int64_t entries = 0;
                if (reply->getAttributes()->getInt(attr.c_str(), entries) == kResultOk)
                    reportedEntries = static_cast<int>(entries);
            }
            reply->release();
        }
        built = reportedEntries > 0;
        std::this_thread::sleep_for(5ms);
    }

    checkf(built,
           "the bank did not finish building within 60 s (progress %.3f, %d captures "
           "reported over %d replies)",
           static_cast<double>(gPortValues[kPortFeedbackFirst + 2]), reportedEntries, capsReplies);
    checkf(capsReplies > 0, "the plug-in sent no capability report, so an editor would draw an "
                            "empty bank; the notify port is not carrying messages");
    checkf(reportedEntries > 0, "the capability report names %d captures in Clean",
           reportedEntries);

    // And now the point of all of it: sound. A quiet sine in, and the output must be finite and
    // not silent. The amp is at its default settings with one bank loaded.
    bool nonSilent = false;
    bool finite = true;
    double phase = 0.0;
    for (int block = 0; block < 200; ++block) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            audioIn[i] = static_cast<float>(0.1 * std::sin(phase));
            phase += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
        }
        seq.resetOutput();
        lilv_instance_run(instance, kBlock);
        for (uint32_t i = 0; i < kBlock; ++i) {
            nonSilent = nonSilent || std::fabs(outL[i]) > 1e-6f;
            finite = finite && std::isfinite(outL[i]) && std::isfinite(outR[i]);
        }
    }
    check(nonSilent, "a loaded amp produced silence");
    check(finite, "the output was not finite");
    // The meters are the other half of what an editor draws, and they come back the same way the
    // progress did.
    check(gPortValues[kPortFeedbackFirst + 0] > 0.0f, "the input meter never moved");
    check(gPortValues[kPortFeedbackFirst + 1] > 0.0f, "the output meter never moved");

    lilv_instance_deactivate(instance);
    lilv_instance_free(instance);
}

//------------------------------------------------------------------------
// Load the plug-in and run it. The strongest thing this tool can say without a DAW: the binary
// resolves, the ports connect, run() survives a few blocks of silence, and the state interface
// saves and restores without complaint.
void checkInstantiate(LilvWorld *world, const LilvPlugin *plugin)
{
    UridMap map;
    LV2_URID_Map mapFeature = {&map, UridMap::mapUri};
    LV2_Feature mapItem = {LV2_URID__map, &mapFeature};
    const LV2_Feature *features[] = {&mapItem, nullptr};

    LilvInstance *instance = lilv_plugin_instantiate(plugin, 48000.0, features);
    check(instance != nullptr, "the plug-in did not instantiate");
    if (!instance)
        return;

    constexpr uint32_t kBlock = 128;
    std::vector<float> audioIn(kBlock, 0.0f);
    std::vector<float> audioOutL(kBlock, 0.0f);
    std::vector<float> audioOutR(kBlock, 0.0f);
    // An atom sequence buffer of each kind. The input's size field is the CONTENT size and the
    // output's is the CAPACITY, which is the convention the plug-in's own run() reads it by.
    std::vector<uint8_t> atomIn(8192, 0);
    std::vector<uint8_t> atomOut(65536, 0);
    auto *seqIn = reinterpret_cast<LV2_Atom_Sequence *>(atomIn.data());
    seqIn->atom.size = sizeof(LV2_Atom_Sequence_Body);
    seqIn->atom.type = UridMap::mapUri(&map, LV2_ATOM__Sequence);
    seqIn->body.unit = 0;
    seqIn->body.pad = 0;
    auto *seqOut = reinterpret_cast<LV2_Atom_Sequence *>(atomOut.data());
    seqOut->atom.size = static_cast<uint32_t>(atomOut.size() - sizeof(LV2_Atom));
    seqOut->atom.type = UridMap::mapUri(&map, LV2_ATOM__Chunk);

    lilv_instance_connect_port(instance, kPortAudioIn, audioIn.data());
    lilv_instance_connect_port(instance, kPortAudioOutL, audioOutL.data());
    lilv_instance_connect_port(instance, kPortAudioOutR, audioOutR.data());
    lilv_instance_connect_port(instance, kPortAtomIn, seqIn);
    lilv_instance_connect_port(instance, kPortAtomOut, seqOut);
    for (int i = 0; i < kControlInCount; ++i) {
        gPortValues[kPortControlFirst + i] = static_cast<float>(controlSpec(i).def);
        lilv_instance_connect_port(instance, kPortControlFirst + i,
                                   &gPortValues[kPortControlFirst + i]);
    }
    for (uint32_t i = kPortFeedbackFirst; i < kPortCount; ++i)
        lilv_instance_connect_port(instance, i, &gPortValues[i]);

    lilv_instance_activate(instance);
    for (int block = 0; block < 32; ++block) {
        // The output port's size field is the capacity every block, exactly as a host resets it.
        seqOut->atom.size = static_cast<uint32_t>(atomOut.size() - sizeof(LV2_Atom));
        lilv_instance_run(instance, kBlock);
    }
    // Nothing is loaded, so the amp is at its ramped-silence gate and the output must be silent —
    // which is also the check that run() wrote the buffers at all rather than leaving them alone.
    bool silent = true;
    for (uint32_t i = 0; i < kBlock; ++i)
        silent = silent && audioOutL[i] == 0.0f && audioOutR[i] == 0.0f;
    check(silent, "an unloaded plug-in produced something other than silence");

    // The state round trip. A fresh instance with nothing loaded still has a full blob — the
    // shared controls, the trims, the pedalboard, the output section — so this exercises the same
    // reader a project does.
    const LV2_URID floatUrid = UridMap::mapUri(&map, LV2_ATOM__Float);
    LilvState *state = lilv_state_new_from_instance(
        plugin, instance, &mapFeature, nullptr, nullptr, nullptr, nullptr, getPortValue,
        const_cast<LV2_URID *>(&floatUrid), LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features);
    check(state != nullptr, "the plug-in's state:interface saved nothing");
    if (state) {
        const unsigned properties = lilv_state_get_num_properties(state);
        checkf(properties >= 2,
               "the saved state holds %u properties; it should hold at least the "
               "blob and the capture-directory mask",
               properties);
        lilv_state_restore(state, instance, nullptr, nullptr, 0, features);
        // Restoring starts a bank build for whatever the blob named — nothing, here — so the only
        // thing to assert is that the plug-in still runs afterwards.
        seqOut->atom.size = static_cast<uint32_t>(atomOut.size() - sizeof(LV2_Atom));
        lilv_instance_run(instance, kBlock);
        lilv_state_free(state);
    }

    lilv_instance_deactivate(instance);
    lilv_instance_free(instance);
    (void)world;
}

//------------------------------------------------------------------------
// The UI half: dlopen it as a host would, embed it in a window of our own, and drive idle(). The
// only automated proof that the editor's shared object loads, finds its art in the bundle and
// takes an X window. Skipped, loudly, when there is no display.
#if !defined(RATIONS_LV2CHECK_NO_X11)
void writeNothing(LV2UI_Controller, uint32_t, uint32_t, uint32_t, const void *)
{
}

void checkUiBinary(const std::string &bundleDir)
{
    const char *display = std::getenv("DISPLAY");
    if (!display || !*display) {
        printf("  skip  the UI binary check needs an X display\n");
        return;
    }
    ::Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        printf("  skip  cannot open DISPLAY=%s\n", display);
        return;
    }

    const std::string soPath = bundleDir + "/rations_ui.so";
    void *handle = dlopen(soPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    checkf(handle != nullptr, "cannot dlopen %s: %s", soPath.c_str(), dlerror());
    if (!handle) {
        XCloseDisplay(dpy);
        return;
    }

    auto descriptorFn =
        reinterpret_cast<LV2UI_DescriptorFunction>(dlsym(handle, "lv2ui_descriptor"));
    check(descriptorFn != nullptr, "the UI binary exports no lv2ui_descriptor");
    if (!descriptorFn) {
        dlclose(handle);
        XCloseDisplay(dpy);
        return;
    }
    const LV2UI_Descriptor *descriptor = descriptorFn(0);
    checkf(descriptor && std::strcmp(descriptor->URI, kUiUri) == 0,
           "the UI descriptor's URI is %s; the code expects %s",
           descriptor ? descriptor->URI : "(none)", kUiUri);
    if (!descriptor) {
        dlclose(handle);
        XCloseDisplay(dpy);
        return;
    }

    const int screen = DefaultScreen(dpy);
    ::Window parent = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, 1133, 403, 0,
                                          BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XFlush(dpy);

    UridMap map;
    LV2_URID_Map mapFeature = {&map, UridMap::mapUri};
    LV2_Feature mapItem = {LV2_URID__map, &mapFeature};
    LV2_Feature parentItem = {LV2_UI__parent, reinterpret_cast<void *>(parent)};
    const LV2_Feature *features[] = {&mapItem, &parentItem, nullptr};

    LV2UI_Widget widget = nullptr;
    LV2UI_Handle ui = descriptor->instantiate(descriptor, kPluginUri, bundleDir.c_str(),
                                              writeNothing, nullptr, &widget, features);
    check(ui != nullptr, "the UI did not instantiate");
    check(widget != nullptr, "the UI handed back no widget");
    if (ui) {
        const auto *idle = static_cast<const LV2UI_Idle_Interface *>(
            descriptor->extension_data(LV2_UI__idleInterface));
        check(idle != nullptr && idle->idle != nullptr,
              "the UI offers no idle interface, so no host could ever drive it");
        if (idle && idle->idle) {
            // Thirty turns is a second of the extension's promised 30 Hz: long enough for the
            // window to be mapped, the art to load and the first frames to be painted.
            bool alive = true;
            for (int i = 0; i < 30 && alive; ++i)
                alive = idle->idle(ui) == 0;
            check(alive, "the UI reported itself closed while idling");
        }
        descriptor->cleanup(ui);
    }

    XDestroyWindow(dpy, parent);
    XCloseDisplay(dpy);
    // Deliberately NOT dlclosed: the editor's FreeType faces are owned by cairo with a destroy
    // callback, and unloading the library under a cairo cache that still holds them is the exact
    // hazard the VST3 guidance warns about. A test process that is about to exit loses nothing by
    // leaving it mapped.
}
#endif

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    LilvWorld *world = lilv_world_new();
    if (!world) {
        fprintf(stderr, "rations_lv2check: cannot create a lilv world\n");
        return 1;
    }

    std::string bundleDir;
    std::string captureDir;
    if (const char *env = std::getenv("RATIONS_TEST_CAPTURES"))
        captureDir = env;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--captures" && i + 1 < argc)
            captureDir = argv[++i];
        else if (arg.rfind("--captures=", 0) == 0)
            captureDir = arg.substr(11);
        else
            positional.push_back(arg);
    }

    if (!positional.empty()) {
        bundleDir = positional.front();
        // A bundle URI must name a directory and must end in a slash, or lilv reads the parent.
        std::string uri = "file://" + bundleDir;
        if (uri.back() != '/')
            uri.push_back('/');
        LilvNode *node = lilv_new_uri(world, uri.c_str());
        lilv_world_load_bundle(world, node);
        lilv_node_free(node);
    } else {
        lilv_world_load_all(world);
    }

    LilvNode *uri = lilv_new_uri(world, kPluginUri);
    const LilvPlugins *plugins = lilv_world_get_all_plugins(world);
    const LilvPlugin *plugin = lilv_plugins_get_by_uri(plugins, uri);
    if (!plugin) {
        fprintf(stderr,
                "rations_lv2check: <%s> not found.\n"
                "  Either the bundle is not installed, or its Turtle does not parse.\n",
                kPluginUri);
        lilv_node_free(uri);
        lilv_world_free(world);
        return 1;
    }

    LilvNode *name = lilv_plugin_get_name(plugin);
    const LilvNode *bundle = lilv_plugin_get_bundle_uri(plugin);
    printf("plugin  <%s>\n", kPluginUri);
    printf("name    %s\n", name ? lilv_node_as_string(name) : "?");
    printf("bundle  %s\n", bundle ? lilv_node_as_string(bundle) : "?");
    lilv_node_free(name);

    if (bundleDir.empty() && bundle) {
        // Everything below wants a filesystem path rather than a URI.
        char *path = lilv_file_uri_parse(lilv_node_as_string(bundle), nullptr);
        if (path) {
            bundleDir = path;
            lilv_free(path);
        }
    }
    while (bundleDir.size() > 1 && bundleDir.back() == '/')
        bundleDir.pop_back();

    printf("\nports and UI\n");
    checkPorts(world, plugin);
    checkUi(world, plugin);

    printf("\nloading and running\n");
    checkInstantiate(world, plugin);

    printf("\ncaptures\n");
    if (captureDir.empty())
        printf("  skip  no --captures <dir> and no $RATIONS_TEST_CAPTURES: the plug-in ships no\n"
               "        captures, so loading a bank, building it and making a sound cannot be\n"
               "        exercised without being pointed at some\n");
    else
        checkCaptures(plugin, captureDir);

    printf("\nthe editor\n");
#if !defined(RATIONS_LV2CHECK_NO_X11)
    checkUiBinary(bundleDir);
#else
    printf("  skip  built without X11\n");
#endif

    printf("\n%d checks, %d failed\n", gChecks, gFailures);
    lilv_node_free(uri);
    lilv_world_free(world);
    return gFailures == 0 ? 0 : 1;
}
