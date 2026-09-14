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
#include "version.h"

#include <lilv/lilv.h>

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
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
// WHAT A HOST PUTS ON THE SCREEN, read back out of the bundle and compared with the VST3's own
// macros in src/version.h. This is the half a user reads, and the two formats are one product
// there — so every value here is checked against the SAME definition the VST3 factory is built
// from rather than against a copy of it.
//
// It is a gate rather than a printout because the failure is silent and cosmetic-looking, and was
// neither. The bundle named no maintainer, so a host's "Name (vendor)" plug-in list had nothing
// to put in the brackets beside an LV2 entry while the VST3 entry in the same list read
// correctly. And it declared no version at all: lv2core.ttl's own documentation says releases
// "MUST be explicitly versioned" and that minor version zero — which is what a host reads when
// there is none — marks a development build that hosts "SHOULD NOT expose to users by default".
void checkIdentity(LilvWorld *world, const LilvPlugin *plugin)
{
    LilvNode *name = lilv_plugin_get_name(plugin);
    checkf(name && std::strcmp(lilv_node_as_string(name), stringPluginName) == 0,
           "the bundle calls the plug-in \"%s\"; the VST3 calls it \"%s\"",
           name ? lilv_node_as_string(name) : "(nothing)", stringPluginName);
    lilv_node_free(name);

    // The vendor. lilv resolves doap:maintainer -> foaf:name, which is what a host displays.
    LilvNode *author = lilv_plugin_get_author_name(plugin);
    checkf(author != nullptr,
           "the bundle names no author: a host that lists plug-ins as \"Name (vendor)\" has "
           "nothing to show beside this one, while the VST3 shows \"%s\"",
           stringCompanyName);
    checkf(!author || std::strcmp(lilv_node_as_string(author), stringCompanyName) == 0,
           "the bundle's author is \"%s\"; the VST3's vendor is \"%s\"",
           author ? lilv_node_as_string(author) : "", stringCompanyName);
    lilv_node_free(author);

    LilvNode *email = lilv_plugin_get_author_email(plugin);
    checkf(email && std::strcmp(lilv_node_as_string(email), stringCompanyEmail) == 0,
           "the bundle's author email is \"%s\"; the VST3's is \"%s\"",
           email ? lilv_node_as_string(email) : "(nothing)", stringCompanyEmail);
    lilv_node_free(email);

    LilvNode *homepage = lilv_plugin_get_author_homepage(plugin);
    checkf(homepage && std::strcmp(lilv_node_as_string(homepage), stringCompanyWeb) == 0,
           "the bundle's author homepage is \"%s\"; the VST3's is \"%s\"",
           homepage ? lilv_node_as_string(homepage) : "(nothing)", stringCompanyWeb);
    lilv_node_free(homepage);

    // The version, as two integers with a stability claim attached to each.
    const auto readInt = [&](const char *predicate, int &out) {
        LilvNode *p = lilv_new_uri(world, predicate);
        LilvNodes *values = lilv_plugin_get_value(plugin, p);
        const bool found = values && lilv_nodes_size(values) > 0;
        if (found)
            out = lilv_node_as_int(lilv_nodes_get_first(values));
        lilv_nodes_free(values);
        lilv_node_free(p);
        return found;
    };
    int minor = -1;
    int micro = -1;
    const bool hasMinor = readInt(LV2_CORE__minorVersion, minor);
    const bool hasMicro = readInt(LV2_CORE__microVersion, micro);
    check(hasMinor && hasMicro,
          "the bundle declares no lv2:minorVersion / lv2:microVersion. A release MUST be "
          "versioned, and a host reads the absence as 0.0 - a development build it is told not "
          "to show by default");
    checkf(!hasMinor || minor == kLv2MinorVersion, "lv2:minorVersion is %d; the code says %d",
           minor, kLv2MinorVersion);
    checkf(!hasMicro || micro == kLv2MicroVersion, "lv2:microVersion is %d; the code says %d",
           micro, kLv2MicroVersion);
    checkf(!hasMinor || minor != 0,
           "lv2:minorVersion 0 marks a pre-release plug-in that hosts are told not to show by "
           "default");
    checkf(!hasMinor || !hasMicro || (minor % 2 == 0 && micro % 2 == 0),
           "lv2:minorVersion %d / lv2:microVersion %d: an odd number in either marks a "
           "development build",
           minor, micro);

    // The category the host files it under, which is the VST3's subcategory in LV2's vocabulary.
    checkf(nodeIsA(world, lilv_plugin_get_uri(plugin), LILV_NS_RDF "type", kLv2PluginClassUri),
           "the bundle does not declare %s, so a host files it somewhere other than the VST3's %s",
           kLv2PluginClass, stringSubCategory);
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

// One MIDI message into the input sequence, the way a host delivers a footswitch. Under VST3 a CC
// arrives as a parameter change and a Program Change as a program parameter; under LV2 both arrive
// here as real MIDI, and turning them back into what the processor expects is the DSP wrapper's
// job. Nothing else in this tree exercises that conversion.
bool sendMidi(Sequences &seq, LV2_Atom_Forge &forge, LV2_URID midiEvent, const uint8_t *bytes,
              uint32_t size)
{
    seq.clearInput();
    LV2_Atom_Sequence *sequence = seq.input();
    uint8_t *tail = reinterpret_cast<uint8_t *>(sequence) + lv2_atom_total_size(&sequence->atom);
    const size_t room = seq.in.size() - static_cast<size_t>(tail - seq.in.data());
    lv2_atom_forge_set_buffer(&forge, tail, static_cast<uint32_t>(room));

    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_sequence_head(&forge, &frame, 0);
    if (!lv2_atom_forge_frame_time(&forge, 0))
        return false;
    if (!lv2_atom_forge_atom(&forge, size, midiEvent))
        return false;
    if (!lv2_atom_forge_write(&forge, bytes, size))
        return false;
    lv2_atom_forge_pop(&forge, &frame);

    const auto *forged = reinterpret_cast<const LV2_Atom_Sequence *>(tail);
    sequence->atom.size = static_cast<uint32_t>(sizeof(LV2_Atom_Sequence_Body) + forged->atom.size -
                                                sizeof(LV2_Atom_Sequence_Body));
    return true;
}

//------------------------------------------------------------------------
// The footswitch, end to end, and it needs no captures: a learn is armed with a message, taught
// with a MIDI CC, and stomped with the same CC, and what comes back is the echo that tells the
// editor its own panel has moved.
//
// That echo is the piece LV2 does not give away. A VST3 host reports a parameter the plug-in
// changed by itself back into IEditController::setParamNormalized, and the panel follows; here a
// control INPUT port belongs to the host, the plug-in may not write one, and without a message of
// its own the bat switch sits still while the sound changes — which is exactly the fault the JACK
// standalone was found to have with a real footswitch.
void checkMidiLearn(const LilvPlugin *plugin)
{
    UridMap map;
    LV2_URID_Map mapFeature = {&map, UridMap::mapUri};
    LV2_Feature mapItem = {LV2_URID__map, &mapFeature};
    const LV2_Feature *features[] = {&mapItem, nullptr};

    MessageUris uris;
    uris.map(&mapFeature);
    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &mapFeature);
    const LV2_URID midiEvent = UridMap::mapUri(&map, LV2_MIDI__MidiEvent);

    LilvInstance *instance = lilv_plugin_instantiate(plugin, 48000.0, features);
    check(instance != nullptr, "the plug-in did not instantiate for the footswitch test");
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

    using namespace std::chrono_literals;
    // Everything the plug-in has said since the last time this was called.
    const auto drain = [&](Message **wanted, const char *wantedId) {
        LV2_ATOM_SEQUENCE_FOREACH(seq.output(), ev)
        {
            const LV2_Atom *atom = &ev->body;
            if (!lv2_atom_forge_is_object_type(&forge, atom->type))
                continue;
            Message *reply = parseMessage(reinterpret_cast<const LV2_Atom_Object *>(atom), uris);
            if (!reply)
                continue;
            if (wanted && !*wanted && reply->id() == wantedId) {
                *wanted = reply;
                continue; // kept; the caller releases it
            }
            reply->release();
        }
    };
    const auto run = [&](int blocks) {
        for (int block = 0; block < blocks; ++block) {
            seq.resetOutput();
            lilv_instance_run(instance, kBlock);
            seq.clearInput();
            drain(nullptr, nullptr);
        }
    };

    // Arm the Crunch row. The message thread is what performs this, so it is given time rather
    // than assumed to have happened by the next block.
    constexpr int kRow = 1; // rows 0..3 are the channels, in channel order
    Message arm;
    arm.setMessageID(kMsgMidiLearn);
    arm.attributes().setInt(kMidiRowAttr, kRow);
    check(sendMessage(seq, forge, uris, arm), "could not forge the learn message");
    seq.resetOutput();
    lilv_instance_run(instance, kBlock);
    seq.clearInput();
    std::this_thread::sleep_for(60ms);
    run(4);

    // Teach it: one press of CC 20. A teaching press must NOT also perform, which is the thing
    // rations_midicheck asserts about the VST3 build and which the conversion here could break on
    // its own.
    const uint8_t press[3] = {0xB0, 20, 127};
    check(sendMidi(seq, forge, midiEvent, press, sizeof(press)), "could not forge the teaching CC");
    seq.resetOutput();
    lilv_instance_run(instance, kBlock);
    seq.clearInput();
    run(4);
    checkf(gPortValues[kPortFeedbackFirst + 4] == 0.0f,
           "the press that taught the row also performed it: the active channel moved to %.3f",
           static_cast<double>(gPortValues[kPortFeedbackFirst + 4]));
    std::this_thread::sleep_for(60ms);
    run(4);

    // Stomp it. Nothing is loaded, so the rack holds the switch and the SOUNDING channel cannot
    // move — but the parameter does, and the echo that says so is what this test is for.
    Message *echo = nullptr;
    check(sendMidi(seq, forge, midiEvent, press, sizeof(press)), "could not forge the stomp");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!echo && std::chrono::steady_clock::now() < deadline) {
        seq.resetOutput();
        lilv_instance_run(instance, kBlock);
        seq.clearInput();
        drain(&echo, kMsgLv2ParamEcho);
        std::this_thread::sleep_for(2ms);
    }

    checkf(echo != nullptr,
           "a footswitch stomp produced no %s: the editor's bat switch would sit still while the "
           "sound changed, and the host's automation lane would never learn",
           kMsgLv2ParamEcho);
    if (echo) {
        int64_t id = 0;
        double value = -1.0;
        check(echo->getAttributes()->getInt(kLv2EchoIdAttr, id) == kResultOk,
              "the parameter echo carries no id");
        check(echo->getAttributes()->getFloat(kLv2EchoValueAttr, value) == kResultOk,
              "the parameter echo carries no value");
        checkf(static_cast<Vst::ParamID>(id) == kChannelId,
               "the echo names parameter %lld; the Crunch row performs the channel switch",
               static_cast<long long>(id));
        checkf(std::fabs(value - normFromChannel(static_cast<Channel>(kRow))) < 1e-9,
               "the echo carries %.6f; Crunch is %.6f", value,
               normFromChannel(static_cast<Channel>(kRow)));
        echo->release();
    }

    lilv_instance_deactivate(instance);
    lilv_instance_free(instance);
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

    // ALL FOUR banks, each as a DIRECTORY. One channel would say nothing about the other three,
    // and a directory is the case that carries the product: a bank a dial sweeps rather than a
    // single capture. The wrapper mirrors the isDir flag into its own state as the message goes
    // past, and the capability report says what each bank turned out to be, so loading four
    // folders and reading four counts back is also the check that the message tunnel keys its
    // attributes by channel rather than by position.
    for (int c = 0; c < kChannelCount; ++c) {
        const std::string bank = captureDir + "/" + kChannelDefaultName[c];
        Message load;
        load.setMessageID(kMsgLoadCapture[c]);
        load.attributes().setBinary(kMsgPathAttr, bank.data(), static_cast<uint32_t>(bank.size()));
        load.attributes().setInt(kMsgIsDirAttr, 1);
        checkf(sendMessage(seq, forge, uris, load), "could not forge the load for %s",
               kChannelDefaultName[c]);
        seq.resetOutput();
        lilv_instance_run(instance, kBlock);
        seq.clearInput();
    }

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
    int entries[kChannelCount] = {};
    int reportedIsDir[kChannelCount] = {};
    // What each bank's captures STATE about their own levels. These are what the editor greys the
    // output modes and the input-calibration pair on, so they have to follow the captures a
    // channel loaded and nothing else - see the reload at the end of this function.
    int hasLoudness[kChannelCount] = {};
    int hasInLevel[kChannelCount] = {};
    int hasOutLevel[kChannelCount] = {};
    int slimmable[kChannelCount] = {};
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
                for (int c = 0; c < kChannelCount; ++c) {
                    const auto read = [&](const char *prefix) {
                        const std::string attr = std::string(prefix) + kChannelDefaultName[c];
                        int64_t value = 0;
                        return reply->getAttributes()->getInt(attr.c_str(), value) == kResultOk
                                   ? static_cast<int>(value)
                                   : 0;
                    };
                    entries[c] = read(kCapsEntryCountAttr);
                    reportedIsDir[c] = read(kCapsIsDirAttr);
                    hasLoudness[c] = read(kCapsHasLoudnessAttr);
                    hasInLevel[c] = read(kCapsHasInLevelAttr);
                    hasOutLevel[c] = read(kCapsHasOutLevelAttr);
                    slimmable[c] = read(kCapsSlimmableAttr);
                }
                reportedEntries = entries[0];
            }
            reply->release();
        }
        built = true;
        for (int c = 0; c < kChannelCount; ++c)
            built = built && entries[c] > 0;
        std::this_thread::sleep_for(5ms);
    }

    checkf(built,
           "the bank did not finish building within 60 s (progress %.3f, %d captures "
           "reported over %d replies)",
           static_cast<double>(gPortValues[kPortFeedbackFirst + 2]), reportedEntries, capsReplies);
    checkf(capsReplies > 0, "the plug-in sent no capability report, so an editor would draw an "
                            "empty bank; the notify port is not carrying messages");
    for (int c = 0; c < kChannelCount; ++c) {
        checkf(entries[c] > 0, "the capability report names %d captures in %s", entries[c],
               kChannelDefaultName[c]);
        // A bank loaded as a FOLDER has to come back as one. If this flag is lost anywhere
        // between the editor and the processor the channel plays a single capture and its dial
        // sweeps nothing, which is the shape the whole product depends on.
        checkf(reportedIsDir[c] == 1, "%s was loaded as a directory and reports isDir %d",
               kChannelDefaultName[c], reportedIsDir[c]);
    }
    (void)reportedEntries;

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

    // --- the channel switch, through the port a host writes -----------------------------------
    //
    // Four banks are the product; being able to change between them with a foot is the feature.
    // The check is the sounding channel and not the requested one, because a switch is HELD until
    // the incoming channel is exact — so what is asserted is that the audio actually arrived
    // there, which is the only claim worth making.
    //
    // It is given a generous deadline rather than a block count, and that is a fact about this
    // harness rather than about the plug-in: offline the blocks run far faster than real time, so
    // the prime worker is never warm and every switch falls back to the full on-thread catch-up
    // that the worker exists to avoid. In a live host the same switch is milliseconds.
    const int channelPort = static_cast<int>(kPortControlFirst) + 3;
    check(controlPortParam(3) == kChannelId,
          "the fourth control port is no longer the channel switch; this check names it by "
          "position");
    const auto soundingChannel = [&]() {
        return static_cast<int>(std::lround(
            static_cast<double>(gPortValues[kPortFeedbackFirst + 4]) * (kChannelCount - 1)));
    };
    double channelRms[kChannelCount] = {};
    for (int c = 0; c < kChannelCount; ++c) {
        gPortValues[channelPort] = static_cast<float>(c);
        const auto switchDeadline = std::chrono::steady_clock::now() + 60s;
        while (soundingChannel() != c && std::chrono::steady_clock::now() < switchDeadline) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                audioIn[i] = static_cast<float>(0.25 * std::sin(phase));
                phase += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
            }
            seq.resetOutput();
            lilv_instance_run(instance, kBlock);
        }
        checkf(soundingChannel() == c,
               "writing %d to the channel control port left %s sounding: the switch never "
               "arrived",
               c, kChannelDefaultName[soundingChannel()]);

        double sum = 0.0;
        int counted = 0;
        for (int block = 0; block < 64; ++block) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                audioIn[i] = static_cast<float>(0.25 * std::sin(phase));
                phase += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
            }
            seq.resetOutput();
            lilv_instance_run(instance, kBlock);
            if (block < 48)
                continue;
            for (uint32_t i = 0; i < kBlock; ++i) {
                sum += static_cast<double>(outL[i]) * outL[i];
                ++counted;
            }
        }
        channelRms[c] = counted > 0 ? std::sqrt(sum / counted) : 0.0;
    }
    // Four different amps through four different banks do not measure the same. Identical figures
    // would mean the switch moved the readout and not the audio.
    for (int c = 1; c < kChannelCount; ++c)
        checkf(std::fabs(channelRms[c] - channelRms[0]) > 1e-6,
               "%s and %s produced the same output (%.6f): the channel switch moved the readout "
               "but not the sound",
               kChannelDefaultName[0], kChannelDefaultName[c], channelRms[c]);

    // --- the dial sweeps the BANK, which is what a folder of captures is for -------------------
    //
    // The active-capture readout is the index the sounding channel is sitting on, normalized over
    // its own bank. Moving that channel's gain port has to move it; a channel whose dial does
    // nothing is a bank that plays one capture, whatever the capability report says it holds.
    const int sounding = soundingChannel();
    int gainPort = -1;
    for (int i = 0; i < kControlInCount; ++i)
        if (controlPortParam(i) == kChannelGainId[sounding])
            gainPort = static_cast<int>(kPortControlFirst) + i;
    checkf(gainPort >= 0, "no control port carries %s's gain dial", kChannelDefaultName[sounding]);
    if (gainPort >= 0 && entries[sounding] > 1) {
        const auto settle = [&](int blocks) {
            for (int block = 0; block < blocks; ++block) {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    audioIn[i] = static_cast<float>(0.25 * std::sin(phase));
                    phase += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
                }
                seq.resetOutput();
                lilv_instance_run(instance, kBlock);
            }
        };
        gPortValues[gainPort] = 0.0f;
        settle(64);
        const float indexLow = gPortValues[kPortFeedbackFirst + 3];
        gPortValues[gainPort] = 1.0f;
        // Long enough for the auto-detent to collapse onto the top capture, which is what makes
        // the readout a whole index rather than a position between two.
        settle(400);
        const float indexHigh = gPortValues[kPortFeedbackFirst + 3];
        checkf(indexHigh > indexLow + 0.5f,
               "sweeping %s's gain dial from 0 to 1 moved the active capture from %.3f to %.3f "
               "over a bank of %d: the dial is not sweeping the bank",
               kChannelDefaultName[sounding], indexLow, indexHigh, entries[sounding]);
        gPortValues[gainPort] = 0.0f;
    }

    // --- the FOOTSWITCH, with a bank under it and the switch required to STAY -----------------
    //
    // checkMidiLearn above stomps a row and reads the echo back, and that is not enough: it runs
    // with nothing loaded, so the rack holds every switch and the sounding channel cannot move,
    // and it looks at one block. The build that shipped passed it while a real footswitch did not
    // change channel at all.
    //
    // What it missed is that a control INPUT port belongs to the host. When the learn table moves
    // one of the plug-in's own parameters, the port still holds the pre-stomp value, and a wrapper
    // that treats that as a change publishes it straight back — so the stomp is undone in the
    // following block. Asserting the switch ARRIVED would not catch it either, because with an
    // editor attached the editor's own write eventually lands and the channel gets there in the
    // end, late and after a reversed switch. So the assertion is that it arrives AND is still
    // there a long way later, with nothing writing the port in between.
    {
        const LV2_URID midiEvent = UridMap::mapUri(&map, LV2_MIDI__MidiEvent);
        const auto spin = [&](int blocks) {
            for (int block = 0; block < blocks; ++block) {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    audioIn[i] = static_cast<float>(0.25 * std::sin(phase));
                    phase += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
                }
                seq.resetOutput();
                lilv_instance_run(instance, kBlock);
                seq.clearInput();
            }
        };
        // Whichever channel is NOT sounding, so that an arrival is a real change.
        const int from = soundingChannel();
        const int to = (from + 1) % kChannelCount;
        const uint8_t press[3] = {0xB0, 20, 127};

        Message arm;
        arm.setMessageID(kMsgMidiLearn);
        arm.attributes().setInt(kMidiRowAttr, to); // rows 0..3 are the channels, in channel order
        check(sendMessage(seq, forge, uris, arm), "could not forge the learn message");
        seq.resetOutput();
        lilv_instance_run(instance, kBlock);
        seq.clearInput();
        std::this_thread::sleep_for(60ms);
        spin(4);

        check(sendMidi(seq, forge, midiEvent, press, sizeof(press)), "could not forge the teach");
        seq.resetOutput();
        lilv_instance_run(instance, kBlock);
        seq.clearInput();
        spin(8);
        checkf(soundingChannel() == from,
               "the press that taught the row also performed it: %s is sounding rather than %s",
               kChannelDefaultName[soundingChannel()], kChannelDefaultName[from]);
        std::this_thread::sleep_for(60ms);
        spin(4);

        check(sendMidi(seq, forge, midiEvent, press, sizeof(press)), "could not forge the stomp");
        seq.resetOutput();
        lilv_instance_run(instance, kBlock);
        seq.clearInput();
        // The same generous deadline the port-driven switch above is given, and for the same
        // reason: offline the prime worker is never warm, so every switch takes the full
        // on-thread catch-up.
        const auto stompDeadline = std::chrono::steady_clock::now() + 60s;
        while (soundingChannel() != to && std::chrono::steady_clock::now() < stompDeadline)
            spin(1);
        checkf(soundingChannel() == to,
               "a footswitch stomp on a learned CC left %s sounding rather than %s: the channel "
               "port still holds the pre-stomp value and the wrapper is publishing it back",
               kChannelDefaultName[soundingChannel()], kChannelDefaultName[to]);

        // And it has to STAY. Nothing writes the channel port here, which is a player with the
        // editor closed — the case MIDI learn exists for.
        spin(600);
        checkf(soundingChannel() == to,
               "the stomp reached %s and the plug-in then went back to %s on its own: a "
               "footswitch that works for one block",
               kChannelDefaultName[to], kChannelDefaultName[soundingChannel()]);
    }

    // --- a capability belongs to the CAPTURES, never to the channel ---------------------------
    //
    // The editor greys the output modes and the input-calibration pair on what the loaded captures
    // state about their own levels, so a user who loads a bank carrying input_level_dbu into any
    // of the four channels has to get a live calibration control there. The four channels ship
    // with default NAMES only — Clean, Crunch, OD1, OD2 are what an empty instance calls them
    // until someone loads their own captures — and nothing may key off which one it is.
    //
    // Proved by moving a bank rather than by reading the code: whatever the LAST channel reported,
    // the FIRST channel must report exactly the same once it is given that channel's folder. If
    // any of it were keyed to the channel, these two rows would disagree.
    constexpr int kFrom = kChannelCount - 1;
    constexpr int kTo = 0;
    if (entries[kFrom] > 0) {
        const std::string moved = captureDir + "/" + kChannelDefaultName[kFrom];
        Message load;
        load.setMessageID(kMsgLoadCapture[kTo]);
        load.attributes().setBinary(kMsgPathAttr, moved.data(),
                                    static_cast<uint32_t>(moved.size()));
        load.attributes().setInt(kMsgIsDirAttr, 1);
        check(sendMessage(seq, forge, uris, load), "could not forge the moved-bank load");
        seq.resetOutput();
        lilv_instance_run(instance, kBlock);
        seq.clearInput();

        const int wantEntries = entries[kFrom];
        const int wantLoudness = hasLoudness[kFrom];
        const int wantInLevel = hasInLevel[kFrom];
        const int wantOutLevel = hasOutLevel[kFrom];
        const int wantSlimmable = slimmable[kFrom];
        entries[kTo] = 0;
        const auto movedDeadline = std::chrono::steady_clock::now() + 60s;
        auto movedPoll = std::chrono::steady_clock::now();
        while (entries[kTo] != wantEntries && std::chrono::steady_clock::now() < movedDeadline) {
            if (std::chrono::steady_clock::now() >= movedPoll) {
                Message poll;
                poll.setMessageID(kMsgRequestCaps);
                sendMessage(seq, forge, uris, poll);
                movedPoll = std::chrono::steady_clock::now() + 100ms;
            }
            seq.resetOutput();
            lilv_instance_run(instance, kBlock);
            seq.clearInput();
            LV2_ATOM_SEQUENCE_FOREACH(seq.output(), ev)
            {
                const LV2_Atom *atom = &ev->body;
                if (!lv2_atom_forge_is_object_type(&forge, atom->type))
                    continue;
                Message *reply =
                    parseMessage(reinterpret_cast<const LV2_Atom_Object *>(atom), uris);
                if (!reply)
                    continue;
                if (reply->id() == kMsgModelCaps) {
                    const auto read = [&](const char *prefix) {
                        const std::string attr = std::string(prefix) + kChannelDefaultName[kTo];
                        int64_t value = 0;
                        return reply->getAttributes()->getInt(attr.c_str(), value) == kResultOk
                                   ? static_cast<int>(value)
                                   : 0;
                    };
                    entries[kTo] = read(kCapsEntryCountAttr);
                    hasLoudness[kTo] = read(kCapsHasLoudnessAttr);
                    hasInLevel[kTo] = read(kCapsHasInLevelAttr);
                    hasOutLevel[kTo] = read(kCapsHasOutLevelAttr);
                    slimmable[kTo] = read(kCapsSlimmableAttr);
                }
                reply->release();
            }
            std::this_thread::sleep_for(5ms);
        }

        checkf(entries[kTo] == wantEntries,
               "%s's bank loaded into %s reports %d captures rather than %d",
               kChannelDefaultName[kFrom], kChannelDefaultName[kTo], entries[kTo], wantEntries);
        checkf(hasInLevel[kTo] == wantInLevel,
               "the same captures report hasInLevel %d in %s and %d in %s: the input-calibration "
               "control would be live on one channel and grey on another for one bank",
               wantInLevel, kChannelDefaultName[kFrom], hasInLevel[kTo], kChannelDefaultName[kTo]);
        checkf(hasOutLevel[kTo] == wantOutLevel,
               "the same captures report hasOutLevel %d in %s and %d in %s", wantOutLevel,
               kChannelDefaultName[kFrom], hasOutLevel[kTo], kChannelDefaultName[kTo]);
        checkf(hasLoudness[kTo] == wantLoudness,
               "the same captures report hasLoudness %d in %s and %d in %s", wantLoudness,
               kChannelDefaultName[kFrom], hasLoudness[kTo], kChannelDefaultName[kTo]);
        checkf(slimmable[kTo] == wantSlimmable,
               "the same captures report slimmable %d in %s and %d in %s", wantSlimmable,
               kChannelDefaultName[kFrom], slimmable[kTo], kChannelDefaultName[kTo]);
    }

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

    // --- do the control ports do ANYTHING? ---------------------------------------------------
    //
    // This is the cheapest check in the file and for a while it was the missing one. Every other
    // thing here can pass while all 48 control inputs are inert: the plug-in loads, the TTL
    // describes them, lilv reads them back, a bank builds, audio comes out and both meters move,
    // because the processor's own defaults already match the ports and nothing here had ever
    // MOVED one. That is exactly what shipped — the wrapper remembered each port's last value and
    // used a NaN to mean "nothing seen yet", in a translation unit built with -ffast-math, where
    // `x == NaN` comes out TRUE and so every port was skipped on the first block and for the life
    // of the instance. Four banks that could not be switched between, a dial that could not sweep
    // its bank, and no control on the panel that did anything.
    //
    // Bypass is the one that needs no captures. With nothing loaded the amp is at its
    // ramped-silence gate, so bypass OUT is silence and bypass IN is the dry input — a difference
    // no default can produce, reached only by the host writing a port.
    const int bypassPort = static_cast<int>(kPortControlFirst); // kBypassId is the first control
    check(controlPortParam(0) == kBypassId, "the first control port is no longer Bypass; this "
                                            "check names it by position");
    const auto runSine = [&](int blocks, double &rms) {
        double sum = 0.0;
        int counted = 0;
        double phase = 0.0;
        for (int block = 0; block < blocks; ++block) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                audioIn[i] = static_cast<float>(0.25 * std::sin(phase));
                phase += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
            }
            seqOut->atom.size = static_cast<uint32_t>(atomOut.size() - sizeof(LV2_Atom));
            lilv_instance_run(instance, kBlock);
            // The last few blocks only: the bypass crossfade is a ramp, so the first ones after a
            // change are neither state.
            if (block < blocks - 8)
                continue;
            for (uint32_t i = 0; i < kBlock; ++i) {
                sum += static_cast<double>(audioOutL[i]) * audioOutL[i];
                ++counted;
            }
        }
        rms = counted > 0 ? std::sqrt(sum / counted) : 0.0;
    };

    double rmsBypassOut = 0.0;
    double rmsBypassIn = 0.0;
    gPortValues[bypassPort] = 0.0f;
    runSine(64, rmsBypassOut);
    gPortValues[bypassPort] = 1.0f;
    runSine(64, rmsBypassIn);
    gPortValues[bypassPort] = 0.0f;

    checkf(rmsBypassIn > 0.1,
           "writing 1.0 to the bypass control port did not pass the dry signal through "
           "(output rms %.6f against an input rms of about 0.177): the host's control ports are "
           "not reaching the plug-in at all",
           rmsBypassIn);
    checkf(rmsBypassIn > rmsBypassOut * 100.0 + 1e-6,
           "the bypass control port made no difference (out %.6f, in %.6f)", rmsBypassOut,
           rmsBypassIn);
    // And back again, so what is being measured is the port and not a one-way latch.
    double rmsAgain = 0.0;
    runSine(64, rmsAgain);
    checkf(rmsAgain < rmsBypassIn * 0.5,
           "bypass could be switched on and not off again (on %.6f, off again %.6f)", rmsBypassIn,
           rmsAgain);

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
// Everything the UI writes, kept so the test can say what it asked for. A real host routes these
// to the plug-in; this one routes them AND records them, because the UI's own messages are
// otherwise invisible from outside the shared object.
struct UiWrite {
    uint32_t port = 0;
    uint32_t protocol = 0;
    std::vector<uint8_t> bytes;
};
std::vector<UiWrite> gUiWrites;

void recordWrite(LV2UI_Controller, uint32_t port, uint32_t size, uint32_t protocol,
                 const void *buffer)
{
    UiWrite write;
    write.port = port;
    write.protocol = protocol;
    const auto *bytes = static_cast<const uint8_t *>(buffer);
    if (bytes && size > 0)
        write.bytes.assign(bytes, bytes + size);
    gUiWrites.push_back(std::move(write));
}

// The editor's own request, handed to a real instance, and the reply read back off the notify
// port. Both halves of the tunnel in one pass, using the bytes the EDITOR produced rather than
// any this file forged — so an encoder and a decoder that agree with each other and with nothing
// else cannot pass it.
void checkStateReply(const LilvPlugin *plugin, const std::vector<uint8_t> &request)
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
    check(instance != nullptr, "the plug-in did not instantiate for the editor's state request");
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

    // Drop the editor's atom into the input sequence exactly as a host would.
    seq.clearInput();
    LV2_Atom_Sequence *sequence = seq.input();
    uint8_t *tail = reinterpret_cast<uint8_t *>(sequence) + lv2_atom_total_size(&sequence->atom);
    const size_t room = seq.in.size() - static_cast<size_t>(tail - seq.in.data());
    bool staged = request.size() + sizeof(LV2_Atom_Event) <= room;
    if (staged) {
        lv2_atom_forge_set_buffer(&forge, tail, static_cast<uint32_t>(room));
        LV2_Atom_Forge_Frame frame;
        lv2_atom_forge_sequence_head(&forge, &frame, 0);
        staged = lv2_atom_forge_frame_time(&forge, 0) != 0 &&
                 lv2_atom_forge_write(&forge, request.data(),
                                      static_cast<uint32_t>(request.size())) != 0;
        lv2_atom_forge_pop(&forge, &frame);
        const auto *forged = reinterpret_cast<const LV2_Atom_Sequence *>(tail);
        sequence->atom.size = static_cast<uint32_t>(
            sizeof(LV2_Atom_Sequence_Body) + forged->atom.size - sizeof(LV2_Atom_Sequence_Body));
    }
    check(staged, "the editor's state request did not fit in an input sequence");

    using namespace std::chrono_literals;
    bool answered = false;
    uint32_t blobSize = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!answered && std::chrono::steady_clock::now() < deadline) {
        seq.resetOutput();
        lilv_instance_run(instance, kBlock);
        seq.clearInput();
        LV2_ATOM_SEQUENCE_FOREACH(seq.output(), ev)
        {
            const LV2_Atom *atom = &ev->body;
            if (!lv2_atom_forge_is_object_type(&forge, atom->type))
                continue;
            Message *reply = parseMessage(reinterpret_cast<const LV2_Atom_Object *>(atom), uris);
            if (!reply)
                continue;
            if (reply->id() == kMsgLv2State) {
                const void *data = nullptr;
                uint32_t size = 0;
                if (reply->getAttributes()->getBinary(kLv2StateAttr, data, size) == kResultOk &&
                    data && size > 0) {
                    answered = true;
                    blobSize = size;
                }
            }
            reply->release();
        }
        std::this_thread::sleep_for(2ms);
    }

    checkf(answered,
           "the plug-in never answered the editor's %s with a %s carrying a state blob: the "
           "editor's capture rows and channel names would stay empty however much is loaded",
           kMsgLv2RequestState, kMsgLv2State);
    checkf(!answered || blobSize > 8, "the state blob the editor was sent is %u bytes", blobSize);

    lilv_instance_deactivate(instance);
    lilv_instance_free(instance);
}

//------------------------------------------------------------------------
void checkUiBinary(const std::string &bundleDir, const LilvPlugin *plugin)
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

    gUiWrites.clear();
    LV2UI_Widget widget = nullptr;
    LV2UI_Handle ui = descriptor->instantiate(descriptor, kPluginUri, bundleDir.c_str(),
                                              recordWrite, nullptr, &widget, features);
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

        // --- does the editor's own traffic go anywhere? ---------------------------------------
        //
        // The panel draws the capture paths and the channel names, and neither is a parameter:
        // under VST3 they arrive through setComponentState, which LV2 has no equivalent of, so the
        // editor asks for them the moment it opens. That request is the first thing this UI ever
        // writes, and it is the only outward sign from here that the editor's connection to the
        // processor is joined at all — a UI that wrote nothing would open, paint and never learn
        // what is loaded.
        const LV2_URID eventTransfer = UridMap::mapUri(&map, LV2_ATOM__eventTransfer);
        MessageUris uris;
        uris.map(&mapFeature);
        LV2_Atom_Forge forge;
        lv2_atom_forge_init(&forge, &mapFeature);

        std::vector<uint8_t> request;
        bool sawRequest = false;
        for (const UiWrite &write : gUiWrites) {
            if (write.port != kPortAtomIn || write.protocol != eventTransfer)
                continue;
            if (write.bytes.size() < sizeof(LV2_Atom))
                continue;
            const auto *atom = reinterpret_cast<const LV2_Atom *>(write.bytes.data());
            if (!lv2_atom_forge_is_object_type(&forge, atom->type))
                continue;
            Message *message = parseMessage(reinterpret_cast<const LV2_Atom_Object *>(atom), uris);
            if (!message)
                continue;
            if (message->id() == kMsgLv2RequestState) {
                sawRequest = true;
                request = write.bytes;
            }
            message->release();
        }
        checkf(sawRequest,
               "the editor wrote no %s to the plug-in's control port (%zu writes in all): it "
               "would open knowing nothing about what is loaded",
               kMsgLv2RequestState, gUiWrites.size());

        // And the answer has to come back. Hand the editor's own request to a real instance and
        // look for the state on the notify port: that is both directions of the tunnel, through
        // the shipped binaries, with nothing in between that this test wrote.
        if (sawRequest && plugin)
            checkStateReply(plugin, request);

        // Finally, feed the editor what a host feeds it — a feedback port and a control port —
        // and keep idling. Nothing here can see the panel; what it can say is that the editor
        // survives the traffic rather than faulting on the first meter reading.
        if (descriptor->port_event) {
            const float meter = 0.5f;
            descriptor->port_event(ui, kPortFeedbackFirst, sizeof(float), 0, &meter);
            const float channel = 2.0f;
            descriptor->port_event(ui, kPortControlFirst + 3, sizeof(float), 0, &channel);
            bool alive = true;
            for (int i = 0; i < 10 && alive; ++i)
                alive = idle && idle->idle ? idle->idle(ui) == 0 : true;
            check(alive, "the editor stopped idling after a port event");
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
    checkIdentity(world, plugin);
    checkPorts(world, plugin);
    checkUi(world, plugin);

    printf("\nloading and running\n");
    checkInstantiate(world, plugin);

    printf("\nthe footswitch\n");
    checkMidiLearn(plugin);

    printf("\ncaptures\n");
    if (captureDir.empty())
        printf("  skip  no --captures <dir> and no $RATIONS_TEST_CAPTURES: the plug-in ships no\n"
               "        captures, so loading a bank, building it and making a sound cannot be\n"
               "        exercised without being pointed at some\n");
    else
        checkCaptures(plugin, captureDir);

    printf("\nthe editor\n");
#if !defined(RATIONS_LV2CHECK_NO_X11)
    checkUiBinary(bundleDir, plugin);
#else
    printf("  skip  built without X11\n");
#endif

    printf("\n%d checks, %d failed\n", gChecks, gFailures);
    lilv_node_free(uri);
    lilv_world_free(world);
    return gFailures == 0 ? 0 : 1;
}
