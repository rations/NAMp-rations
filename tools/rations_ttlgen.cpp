// SPDX-License-Identifier: MIT
//
// rations_ttlgen — writes the LV2 bundle's manifest.ttl and rations.ttl, and refuses to write
// either one if the port table disagrees with the plug-in.
//
// WHY THIS IS A TOOL AND NOT A FILE. An LV2 bundle's TTL states every parameter's range, default,
// step count and name. Those already exist, once, in src/rationscontroller.cpp and src/rationsids.h
// — so a hand-written TTL would be a second copy of forty-eight parameters, kept in step by
// nobody. This links the real controller, asks it, and writes down what it says.
//
// WHAT IT CHECKS BEFORE IT WRITES, and each of these is a way the LV2 build could otherwise go
// quietly wrong:
//
//   * every port in lv2/rationslv2.h's table is a parameter the controller actually declares;
//   * every parameter the controller declares outside the MIDI block (1000..) and the read-only
//     feedback block (200..) has a port — so a control added to the plug-in and forgotten here
//     fails the build instead of becoming something no LV2 host can reach;
//   * each port's range, step count and default match the controller's own, to a tolerance far
//     tighter than any of them are specified to;
//   * the CC and Program Change routes the DSP wrapper hard-codes are the routes the controller's
//     IMidiMapping and program list actually name — the one place that wrapper states a fact about
//     the plug-in rather than asking it;
//   * every lv2:symbol is unique, since a repeat is a bundle a host silently mis-reads.
//
// Run by the build, into the staged bundle. It needs no X server and no captures.

#include "lv2/rationslv2.h"

#include "rationscontroller.h"
#include "rationsids.h"

#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstunits.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Rations;
using namespace Rations::lv2;

namespace
{

int gFailures = 0;

void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fail(const char *fmt, ...)
{
    fputs("rations_ttlgen: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    ++gFailures;
}

// A VST3 String128 as ASCII. Every title, unit and scale-point string in this plug-in is ASCII by
// construction; anything outside it becomes '?' rather than a mangled byte in a TTL a parser will
// then reject.
std::string ascii(const Vst::TChar *s)
{
    std::string out;
    for (int i = 0; s && s[i] && i < 128; ++i)
        out.push_back(s[i] < 0x80 ? static_cast<char>(s[i]) : '?');
    return out;
}

// An lv2:symbol: a name a host may use as an identifier, and the key it stores automation under.
// Derived from the title so it reads as the control it is, with everything a symbol may not
// contain folded to '_'.
std::string symbolOf(const std::string &title)
{
    std::string out;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        else if (!out.empty() && out.back() != '_')
            out.push_back('_');
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty())
        out = "control";
    if (std::isdigit(static_cast<unsigned char>(out[0])))
        out.insert(out.begin(), 'p');
    return out;
}

// Turtle wants a decimal point in a number that is to be read as a double, and "%g" will happily
// write "1" or "1e-05". Neither is what a strict parser takes as an xsd:decimal.
std::string number(double v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f", v);
    // Trim the trailing zeros but never the point itself.
    std::string s(buf);
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last == dot)
            ++last;
        s.erase(last + 1);
    }
    return s;
}

bool near(double a, double b)
{
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

struct PortInfo {
    Vst::ParamID id = 0;
    std::string title;
    std::string symbol;
    std::string unit;
    ControlSpec spec = {};
    std::vector<std::string> scalePoints; // list parameters only
};

// The standard unit for a VST3 unit string, or an empty string when there is no standard one and
// the caller has to spell the unit out. dBu is the interesting case: LV2's units vocabulary has
// db, hz, ms, pc and a dozen more, and no dBu — so that one is written as a units:Unit of its own
// rather than mislabelled as dB, which is a different quantity by 0.775 volts.
std::string standardUnit(const std::string &vst3Unit)
{
    if (vst3Unit == "dB")
        return "units:db";
    if (vst3Unit == "Hz")
        return "units:hz";
    if (vst3Unit == "ms")
        return "units:ms";
    if (vst3Unit == "%")
        return "units:pc";
    return std::string();
}

//------------------------------------------------------------------------
// The checks.
//------------------------------------------------------------------------
void checkAgainstController(RationsController &controller, const std::vector<PortInfo> &ports)
{
    std::set<Vst::ParamID> ported;
    for (const PortInfo &p : ports)
        ported.insert(p.id);

    // Every parameter that ought to have a port, has one.
    const int32 count = controller.getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info = {};
        if (controller.getParameterInfo(i, info) != kResultOk)
            continue;
        if (info.id >= kMidiCcBaseId)
            continue; // the MIDI block: real MIDI on the atom port under LV2
        bool feedback = false;
        for (int f = 0; f < kFeedbackCount; ++f)
            feedback = feedback || kFeedbackIds[f] == info.id;
        if (feedback)
            continue;
        if (ported.count(info.id) == 0)
            fail("parameter %u (%s) has no LV2 port; add it to kFixedControlIds", info.id,
                 ascii(info.title).c_str());
    }

    // Every port is a real parameter, and its range is the parameter's own.
    for (const PortInfo &p : ports) {
        Vst::Parameter *param = controller.getParameterObject(p.id);
        if (!param) {
            fail("port %s names parameter %u, which the controller does not declare",
                 p.symbol.c_str(), p.id);
            continue;
        }
        const Vst::ParameterInfo &info = param->getInfo();
        if (!near(param->toPlain(0.0), p.spec.min) || !near(param->toPlain(1.0), p.spec.max))
            fail("%s: the table says [%s, %s] and the controller says [%s, %s]", p.symbol.c_str(),
                 number(p.spec.min).c_str(), number(p.spec.max).c_str(),
                 number(param->toPlain(0.0)).c_str(), number(param->toPlain(1.0)).c_str());
        if (info.stepCount != p.spec.steps)
            fail("%s: the table says %d steps and the controller says %d", p.symbol.c_str(),
                 p.spec.steps, info.stepCount);
        const double controllerDefault = param->toPlain(info.defaultNormalizedValue);
        if (!near(controllerDefault, p.spec.def))
            fail("%s: the table's default is %s and the controller's is %s", p.symbol.c_str(),
                 number(p.spec.def).c_str(), number(controllerDefault).c_str());
    }

    // Unique symbols. A repeat is not a parse error — a host simply takes one of them and stores
    // the other's automation under it.
    std::set<std::string> symbols;
    for (const PortInfo &p : ports)
        if (!symbols.insert(p.symbol).second)
            fail("two ports share the symbol \"%s\"", p.symbol.c_str());

    // The MIDI routes the DSP wrapper spells out. It is inside this source tree, so it names what
    // rationsids.h defines rather than discovering it the way standalone/midiroute.cpp has to —
    // and this is what keeps "names" and "is" the same thing.
    // One controller number is enough to pin the rule, and CC 7 is as good as any: the claim under
    // test is the ARITHMETIC the LV2 wrapper does, not a table of 128 answers.
    const int probeCc = static_cast<int>(Vst::kCtrlVolume);
    const auto expectedCcParam =
        static_cast<Vst::ParamID>(static_cast<int>(kMidiCcBaseId) + probeCc);
    Vst::ParamID ccParam = Vst::kNoParamId;
    if (controller.getMidiControllerAssignment(0, 0, static_cast<Vst::CtrlNumber>(probeCc),
                                               ccParam) != kResultTrue ||
        ccParam != expectedCcParam) {
        fail("the controller maps CC %d to %u; the LV2 wrapper assumes kMidiCcBaseId + cc (%u)",
             probeCc, ccParam, expectedCcParam);
    }
    Vst::UnitID unitId = Vst::kRootUnitId;
    if (controller.getUnitByBus(Vst::kEvent, Vst::kInput, 0, 0, unitId) != kResultTrue ||
        unitId != kMidiUnitId) {
        fail("the event bus resolves to unit %d rather than the MIDI unit (%d)", unitId,
             kMidiUnitId);
    }
    Vst::Parameter *pc = controller.getParameterObject(kMidiProgramChangeId);
    if (!pc || (pc->getInfo().flags & Vst::ParameterInfo::kIsProgramChange) == 0)
        fail("no kIsProgramChange parameter at %u", static_cast<unsigned>(kMidiProgramChangeId));
    else if (pc->getInfo().stepCount != kMidiProgramCount - 1)
        fail("the program list has %d steps; the LV2 wrapper divides a program number by %d",
             pc->getInfo().stepCount, kMidiProgramCount - 1);
}

//------------------------------------------------------------------------
std::vector<PortInfo> collectPorts(RationsController &controller)
{
    std::vector<PortInfo> ports;
    ports.reserve(kControlInCount);
    for (int i = 0; i < kControlInCount; ++i) {
        PortInfo p;
        p.spec = controlSpec(i);
        p.id = p.spec.id;
        Vst::Parameter *param = controller.getParameterObject(p.id);
        if (param) {
            p.title = ascii(param->getInfo().title);
            p.unit = ascii(param->getInfo().units);
            if (p.spec.steps > 0 && (param->getInfo().flags & Vst::ParameterInfo::kIsList) != 0) {
                for (int step = 0; step <= p.spec.steps; ++step) {
                    Vst::String128 text = {};
                    const Vst::ParamValue norm =
                        static_cast<double>(step) / static_cast<double>(p.spec.steps);
                    if (controller.getParamStringByValue(p.id, norm, text) == kResultOk)
                        p.scalePoints.push_back(ascii(text));
                }
            }
        }
        if (p.title.empty())
            p.title = "Control";
        p.symbol = symbolOf(p.title);
        ports.push_back(std::move(p));
    }
    return ports;
}

//------------------------------------------------------------------------
void writeControlPort(FILE *f, const PortInfo &p, std::uint32_t index)
{
    fprintf(f, "[\n\t\ta lv2:InputPort ,\n\t\t\tlv2:ControlPort ;\n");
    fprintf(f, "\t\tlv2:index %u ;\n", index);
    fprintf(f, "\t\tlv2:symbol \"%s\" ;\n", p.symbol.c_str());
    fprintf(f, "\t\tlv2:name \"%s\" ;\n", p.title.c_str());
    fprintf(f, "\t\tlv2:default %s ;\n", number(p.spec.def).c_str());
    fprintf(f, "\t\tlv2:minimum %s ;\n", number(p.spec.min).c_str());
    fprintf(f, "\t\tlv2:maximum %s", number(p.spec.max).c_str());

    if (p.spec.steps == 1) {
        fprintf(f, " ;\n\t\tlv2:portProperty lv2:toggled");
    } else if (p.spec.steps > 1) {
        fprintf(f, " ;\n\t\tlv2:portProperty lv2:integer ,\n\t\t\tlv2:enumeration");
        for (size_t s = 0; s < p.scalePoints.size(); ++s) {
            fprintf(f,
                    " ;\n\t\tlv2:scalePoint [\n\t\t\trdfs:label \"%s\" ;\n\t\t\trdf:value %s\n"
                    "\t\t]",
                    p.scalePoints[s].c_str(), number(static_cast<double>(s)).c_str());
        }
    }

    if (!p.unit.empty()) {
        const std::string standard = standardUnit(p.unit);
        if (!standard.empty())
            fprintf(f, " ;\n\t\tunits:unit %s", standard.c_str());
        else
            fprintf(f,
                    " ;\n\t\tunits:unit [\n\t\t\ta units:Unit ;\n\t\t\tunits:name \"%s\" ;\n"
                    "\t\t\tunits:symbol \"%s\" ;\n\t\t\tunits:render \"%%f %s\"\n\t\t]",
                    p.unit.c_str(), p.unit.c_str(), p.unit.c_str());
    }
    fprintf(f, "\n\t]");
}

//------------------------------------------------------------------------
bool writePluginTtl(const std::string &path, const std::vector<PortInfo> &ports)
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        fail("cannot write %s", path.c_str());
        return false;
    }

    fprintf(f,
            "# GENERATED by tools/rations_ttlgen.cpp. Do not edit.\n"
            "#\n"
            "# Every range, default, step count and name below is read out of the plug-in's own\n"
            "# RationsController at build time, so this file cannot drift from the parameters it\n"
            "# describes. To change a control, change src/rationscontroller.cpp and rebuild.\n\n");
    fprintf(f, "@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .\n"
               "@prefix bufsz: <http://lv2plug.in/ns/ext/buf-size#> .\n"
               "@prefix doap:  <http://usefulinc.com/ns/doap#> .\n"
               "@prefix foaf:  <http://xmlns.com/foaf/0.1/> .\n"
               "@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .\n"
               "@prefix midi:  <http://lv2plug.in/ns/ext/midi#> .\n"
               "@prefix opts:  <http://lv2plug.in/ns/ext/options#> .\n"
               "@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n"
               "@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .\n"
               "@prefix rsz:   <http://lv2plug.in/ns/ext/resize-port#> .\n"
               "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n"
               "@prefix time:  <http://lv2plug.in/ns/ext/time#> .\n"
               "@prefix ui:    <http://lv2plug.in/ns/extensions/ui#> .\n"
               "@prefix units: <http://lv2plug.in/ns/extensions/units#> .\n"
               "@prefix urid:  <http://lv2plug.in/ns/ext/urid#> .\n\n");

    // --- the UI ----------------------------------------------------------
    fprintf(
        f,
        "# The editor is an X11 UI: the LV2UI_Widget it hands back is an X11 Window ID, not a\n"
        "# pointer, created as a child of the window passed through ui:parent.\n"
        "#\n"
        "# ui:idleInterface is required in BOTH directions and neither is optional. As a\n"
        "# FEATURE, because the UI owns no thread and no timer — the plug-in's X11 editor\n"
        "# borrows its host's run loop, and idle() is the run loop here, so without the host\n"
        "# calling it nothing drains X events or repaints. As EXTENSION DATA, because that is\n"
        "# how the host obtains the callback. One without the other gives a UI that either\n"
        "# never runs or that the host cannot drive.\n"
        "#\n"
        "# ui:portNotification is load-bearing rather than decorative. The UI extension says a\n"
        "# host calls port_event() for control port INPUTS by default. Ports %u..%u are\n"
        "# OUTPUTS — the two meters, the bank build progress, which capture is sounding and\n"
        "# which channel is — so without these declarations every one of those readouts is\n"
        "# permanently dead while the plug-in builds, loads and otherwise works perfectly. The\n"
        "# same is true of the notify port at %u, which carries the capture names, the MIDI\n"
        "# learn table and the plug-in's state to the editor.\n"
        "#\n"
        "# ui:resize is deprecated in the specification and is still the only way a UI can\n"
        "# state its size, so it is declared OPTIONAL: the editor asks once at startup and\n"
        "# again on every page change, and carries on without it. ui:noUserResize is NOT\n"
        "# declared — this editor is resizable, and its limits reach a host through the X size\n"
        "# hints it sets on its own window, which is what suil reads before resizing it.\n",
        kPortFeedbackFirst, kPortFeedbackFirst + kFeedbackCount - 1, kPortAtomOut);
    fprintf(f,
            "<%s>\n\ta ui:X11UI ;\n\tlv2:binary <rations_ui.so> ;\n\tui:binary <rations_ui.so> "
            ";\n"
            "\tlv2:requiredFeature ui:idleInterface ,\n\t\turid:map ;\n"
            "\tlv2:optionalFeature ui:resize ;\n"
            "\tlv2:extensionData ui:idleInterface ;\n",
            kUiUri);
    fprintf(f,
            "\tui:portNotification [\n\t\tui:plugin <%s> ;\n\t\tui:portIndex %u ;\n"
            "\t\tui:protocol atom:eventTransfer ;\n\t\tui:notifyType atom:Object\n\t]",
            kPluginUri, kPortAtomOut);
    for (int i = 0; i < kFeedbackCount; ++i) {
        fprintf(f,
                " , [\n\t\tui:plugin <%s> ;\n\t\tui:portIndex %u ;\n"
                "\t\tui:protocol ui:floatProtocol\n\t]",
                kPluginUri, kPortFeedbackFirst + static_cast<std::uint32_t>(i));
    }
    fprintf(f, " .\n\n");

    // --- the plug-in -----------------------------------------------------
    fprintf(f, "<%s>\n\ta lv2:Plugin ,\n\t\tlv2:SimulatorPlugin ;\n", kPluginUri);
    fprintf(f,
            "\tdoap:name \"NAMp Rations\" ;\n"
            "\tdoap:license <http://opensource.org/licenses/MIT> ;\n"
            "\tlv2:project [\n\t\tdoap:name \"NAMp Rations\" ;\n"
            "\t\tdoap:homepage <%s>\n\t] ;\n",
            kPluginUri);
    fprintf(f, "\tlv2:requiredFeature urid:map ;\n"
               "\tlv2:optionalFeature lv2:hardRTCapable ,\n"
               "\t\tbufsz:boundedBlockLength ,\n"
               "\t\topts:options ;\n"
               "\tlv2:extensionData state:interface ;\n");
    fprintf(f, "\tui:ui <%s> ;\n", kUiUri);
    fprintf(f, "\tlv2:port\n");

    fprintf(f,
            "\t[\n\t\ta lv2:AudioPort ,\n\t\t\tlv2:InputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tlv2:symbol \"in\" ;\n\t\tlv2:name \"In\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAudioIn));
    fprintf(f,
            "[\n\t\ta lv2:AudioPort ,\n\t\t\tlv2:OutputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tlv2:symbol \"out_l\" ;\n\t\tlv2:name \"Out Left\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAudioOutL));
    fprintf(f,
            "[\n\t\ta lv2:AudioPort ,\n\t\t\tlv2:OutputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tlv2:symbol \"out_r\" ;\n\t\tlv2:name \"Out Right\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAudioOutR));

    // The control port carries MIDI (the footswitch, which is the reason the channel switch
    // exists) and the editor's own messages. time:Position is what the Delay's sync divisions
    // read; a host that sends none leaves the Delay free-running, which is its own fallback.
    fprintf(f,
            "[\n\t\ta atom:AtomPort ,\n\t\t\tlv2:InputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tatom:bufferType atom:Sequence ;\n"
            "\t\tatom:supports midi:MidiEvent ,\n\t\t\ttime:Position ,\n\t\t\tatom:Object ;\n"
            "\t\tlv2:designation lv2:control ;\n"
            "\t\tlv2:symbol \"control\" ;\n\t\tlv2:name \"Control\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAtomIn));
    // The notify port carries what the editor cannot get any other way: the capture names each
    // dial is sitting on, the MIDI learn table, and the plug-in's state. rsz:minimumSize because
    // a bank of thirty-five capture names does not fit in a host's default atom buffer.
    fprintf(f,
            "[\n\t\ta atom:AtomPort ,\n\t\t\tlv2:OutputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tatom:bufferType atom:Sequence ;\n"
            "\t\tatom:supports atom:Object ;\n"
            "\t\trsz:minimumSize 65536 ;\n"
            "\t\tlv2:symbol \"notify\" ;\n\t\tlv2:name \"Notify\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAtomOut));

    for (int i = 0; i < kControlInCount; ++i) {
        writeControlPort(f, ports[static_cast<size_t>(i)],
                         kPortControlFirst + static_cast<std::uint32_t>(i));
        fprintf(f, " , ");
    }

    static const char *kFeedbackNames[kFeedbackCount] = {
        "Input Meter", "Output Meter", "Bank Progress", "Active Capture", "Active Channel",
    };
    static const char *kFeedbackSymbols[kFeedbackCount] = {
        "input_meter", "output_meter", "bank_progress", "active_capture", "active_channel",
    };
    for (int i = 0; i < kFeedbackCount; ++i) {
        fprintf(f,
                "[\n\t\ta lv2:OutputPort ,\n\t\t\tlv2:ControlPort ;\n\t\tlv2:index %u ;\n"
                "\t\tlv2:symbol \"%s\" ;\n\t\tlv2:name \"%s\" ;\n"
                "\t\tlv2:default 0.0 ;\n\t\tlv2:minimum 0.0 ;\n\t\tlv2:maximum 1.0\n\t] , ",
                kPortFeedbackFirst + static_cast<std::uint32_t>(i), kFeedbackSymbols[i],
                kFeedbackNames[i]);
    }

    // Latency. The VST3 build reports it through getLatencySamples(); LV2's way of saying the same
    // thing is this port, and the wrapper copies the same figure into it every block.
    fprintf(f,
            "[\n\t\ta lv2:OutputPort ,\n\t\t\tlv2:ControlPort ;\n\t\tlv2:index %u ;\n"
            "\t\tlv2:symbol \"latency\" ;\n\t\tlv2:name \"Latency\" ;\n"
            "\t\tlv2:designation lv2:latency ;\n"
            "\t\tlv2:portProperty lv2:reportsLatency ,\n\t\t\tlv2:integer ;\n"
            "\t\tlv2:minimum 0 ;\n\t\tlv2:maximum 65536\n\t] .\n",
            static_cast<std::uint32_t>(kPortLatency));

    fclose(f);
    return true;
}

//------------------------------------------------------------------------
bool writeManifest(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        fail("cannot write %s", path.c_str());
        return false;
    }
    fprintf(f, "# GENERATED by tools/rations_ttlgen.cpp. Do not edit.\n\n");
    fprintf(f, "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
               "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
               "@prefix ui:   <http://lv2plug.in/ns/extensions/ui#> .\n\n");
    fprintf(f,
            "<%s>\n\ta lv2:Plugin ;\n\tlv2:binary <rations.so> ;\n\trdfs:seeAlso <rations.ttl> "
            ".\n\n",
            kPluginUri);
    fprintf(
        f,
        "# The UI is declared HERE as well as in rations.ttl, because hosts discover UIs while\n"
        "# scanning manifests, before any plug-in's full description is read: one declared only\n"
        "# in the plug-in's own file is invisible to most of them.\n"
        "#\n"
        "# Both lv2:binary and ui:binary are given. ui:binary is deprecated in favour of\n"
        "# lv2:binary and the specification still requires hosts to support it, and loaders\n"
        "# differ in which they look for — lilv reads lv2:binary and falls back to ui:binary,\n"
        "# while other hosts only ever learned ui:binary.\n");
    fprintf(f,
            "<%s>\n\ta ui:X11UI ;\n\tlv2:binary <rations_ui.so> ;\n\tui:binary <rations_ui.so> "
            ";\n\trdfs:seeAlso <rations.ttl> .\n",
            kUiUri);
    fclose(f);
    return true;
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: rations_ttlgen <bundle directory>\n");
        return 2;
    }
    const std::string dir = argv[1];

    // The controller is instantiated exactly as a host does, minus the host context: nothing here
    // sends a message, and initialize() is what declares every parameter this file reads.
    IPtr<RationsController> controller = owned(new RationsController());
    if (controller->initialize(nullptr) != kResultOk) {
        fprintf(stderr, "rations_ttlgen: the controller refused to initialise\n");
        return 1;
    }

    const std::vector<PortInfo> ports = collectPorts(*controller);
    checkAgainstController(*controller, ports);
    if (gFailures > 0) {
        fprintf(stderr, "rations_ttlgen: %d problem(s); no TTL written\n", gFailures);
        return 1;
    }

    if (!writeManifest(dir + "/manifest.ttl") || !writePluginTtl(dir + "/rations.ttl", ports))
        return 1;

    printf("rations_ttlgen: %d control ports, %d feedback ports, %u ports in all -> %s\n",
           kControlInCount, kFeedbackCount, static_cast<unsigned>(kPortCount), dir.c_str());
    controller->terminate();
    return 0;
}
