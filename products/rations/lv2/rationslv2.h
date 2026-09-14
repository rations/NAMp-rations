// SPDX-License-Identifier: MIT
//
// The LV2 build's shared vocabulary: the URIs, the port table, and the two wrapper-level messages
// that exist only because LV2 has no setComponentState.
//
// WHAT THE LV2 BUILD IS. It is not a second plug-in. rations.so instantiates the SAME
// RationsProcessor a VST3 host does and drives it through IAudioProcessor; rations_ui.so
// instantiates the SAME RationsController and RationsEditorView. Everything this directory
// contains is the adapter between LV2's four callbacks and the VST3 objects underneath — a host,
// in other words, of exactly the shape standalone/ already is, minus JACK and plus LV2.
//
// That is a deliberate choice over porting the DSP and the panel a second time, and it is the
// project's own rule rather than a shortcut: "port, do not re-invent", and "a file that began life
// in NAMp/src should still be recognisable as that file". A parallel LV2 processor would be a
// second copy of the chain order, the channel switch, the pedal placement and the state format,
// and every measured figure this project has recorded would then be a claim about one of the two
// copies. This
// way there is one DSP path and one editor, and the LV2 build inherits every proof they carry.
//
// THE PORT TABLE. The control ports are the plug-in's own parameters minus two blocks: the MIDI
// block at 1000.. (which exists only so a footswitch's CC and Program Change have somewhere to
// land — under LV2 those arrive as real MIDI on the atom port instead, and are converted back
// into parameter points by the DSP wrapper) and the feedback block at 200.. (which is
// processor -> editor and becomes five control OUTPUT ports). The list below is checked against
// the controller's own declarations by tools/rations_ttlgen.cpp, which refuses to write a TTL if
// the two disagree — so the two cannot drift even though only one of them is a list.

#pragma once

#include "rationsids.h"
#include "version.h"

#include <cstdint>

namespace Rations
{
namespace lv2
{

// The plug-in's identity. The UI is a fragment of it, which is the convention the author's own
// lvtuner uses and which keeps one repository URL answering for both.
inline constexpr const char *kPluginUri = "https://github.com/rations/NAMp-rations";
inline constexpr const char *kUiUri = "https://github.com/rations/NAMp-rations#ui";

// --- what a host puts on the screen -----------------------------------------------------------
//
// The name, the vendor, the version and the category are the VST3's, taken from src/version.h
// rather than restated: this is the half of the bundle a user reads, and the two formats are one
// product there. Hand-written literals were what made them diverge — the VST3 factory names a
// vendor and the bundle named nobody, so a host's "Name (vendor)" list had nothing to put in the
// brackets for the LV2 entry and printed something of its own beside a VST3 entry that reads
// correctly in the same list.
//
// THE VERSION IS NOT COSMETIC. lv2core.ttl's own documentation is explicit: "Releases of plugins
// and extensions MUST be explicitly versioned", and "an odd minor or micro version, OR MINOR
// VERSION ZERO, indicates that the resource is a development version... Where feasible, hosts
// SHOULD NOT expose such plugins to users by default." A bundle that declares neither is read as
// 0.0, which is precisely that case — so the plug-in was asking to be hidden.
//
// LV2 has no major version, only these two, so the mapping from a three-part project version is a
// decision rather than an arithmetic. It is made once here and CHECKED: the asserts below refuse
// to build rather than let a version be carried across that would claim something untrue.
//
// THE MAPPING IS DOUBLING, AND THE PROJECT'S OWN VERSION IS NOT CONSTRAINED BY IT. An earlier
// version of this file used the project's numbers directly, which quietly made LV2's convention
// into a rule about the product: 0.3.0 would not build, because 3 is odd. That is backwards. A
// project numbers its releases; LV2 numbers a BUNDLE, and what its numbers describe is port-table
// compatibility and release status, not a product. The two are different facts and only one of
// them is anybody's to choose.
//
// So the project version is whatever the project says, and the bundle's is derived from it by
// doubling: even by construction, monotonically increasing with the project's own numbering, and
// needing no second list for anyone to keep in step. Nothing is skipped and no release is renamed.
inline constexpr int kLv2MinorVersion = 2 * SUB_VERSION_INT;
inline constexpr int kLv2MicroVersion = 2 * RELEASE_NUMBER_INT;
static_assert(MAJOR_VERSION_INT == 0,
              "LV2 has no major version. Decide how the project's MAJOR maps onto "
              "lv2:minorVersion before releasing 1.0, and keep lv2:minorVersion monotonically "
              "increasing across releases - a host loads only the highest it can see.");
static_assert(kLv2MinorVersion != 0,
              "lv2:minorVersion 0 declares a pre-release plug-in that hosts are told not to show "
              "by default. Give this release an even, non-zero minor version.");
static_assert(kLv2MinorVersion % 2 == 0 && kLv2MicroVersion % 2 == 0,
              "LV2 reads an odd lv2:minorVersion or lv2:microVersion as a development build that "
              "hosts SHOULD NOT expose by default. The doubling above makes that impossible, so "
              "this fires only if the mapping itself was changed - which is a decision to make "
              "deliberately, not a number to adjust.");

// The VST3 files the plug-in under stringSubCategory. LV2's taxonomy is a class rather than a
// string, and this is that category's name in it (verified against lv2core.ttl, where
// lv2:DistortionPlugin is "A plugin that adds distortion to its input"). The pair is written
// together so that changing one without the other is visible; rations_lv2check reads the class
// back out of the installed bundle and compares it with this.
inline constexpr const char *kLv2PluginClass = "lv2:DistortionPlugin";
inline constexpr const char *kLv2PluginClassUri = "http://lv2plug.in/ns/lv2core#DistortionPlugin";

// --- the wrapper's own atom vocabulary --------------------------------------------------------
//
// One object type carries every VST3 IMessage in both directions. It is a TUNNEL rather than a set
// of lv2:Parameters reached by patch:Set, and that is a decision with a reason on each side.
//
// Against patch: the seven message types are entirely internal traffic between two halves of one
// plug-in that share a source tree, so the only thing idiomatic patch parameters would add is
// HOST introspection — a generic UI able to set a capture path with no editor open. This plug-in
// has already declined exactly that feature on the VST3 side, and for the reason that applies
// here too: src/rationscontroller.h records that the paths are deliberately not a published COM
// interface because "that is a permanent commitment to a UID, and nothing has asked for it here
// yet". An lv2:Parameter URI is the same permanent commitment. Adding them later is purely
// additive and breaks nothing, which is why this is a cheap decision to revisit.
//
// For the tunnel: it is generic over whatever the two halves put in a message, so an eighth
// message type costs nothing and cannot be forgotten on one side. Every attribute the sender
// wrote is carried, in order, with its type.
inline constexpr const char *kAtomMessageUri = "https://github.com/rations/NAMp-rations#message";
// The message's VST3 id string, e.g. "RationsLoadCaptureClean".
inline constexpr const char *kAtomMessageIdUri = "https://github.com/rations/NAMp-rations#msgId";
// A Tuple of alternating key (String) and value (Int / Double / String / Chunk) atoms. A Tuple
// rather than an Object because VST3 attribute keys are arbitrary strings rather than URIs, and
// inventing a URI per attribute would put the drift back that the tunnel exists to remove.
inline constexpr const char *kAtomMessageBodyUri =
    "https://github.com/rations/NAMp-rations#msgBody";

// --- the two messages that exist only in this build -------------------------------------------
//
// A VST3 host calls IEditController::setComponentState so the editor's mirror of the paths and
// the channel names agrees with the processor's. LV2 has no such call: the UI and the DSP are
// separate shared objects that a host may not even load into the same process. So the UI asks,
// and the DSP wrapper answers out of the processor's own getState — the same blob, read by the
// same reader, so there is no second state format and nothing to keep in step.
//
// Neither id ever reaches RationsProcessor::notify: they are served by the wrapper, which holds
// the processor pointer and needs nothing from it that IAudioProcessor does not already offer.
inline constexpr const char *kMsgLv2RequestState = "RationsLv2RequestState";
inline constexpr const char *kMsgLv2State = "RationsLv2State";
inline constexpr const char *kLv2StateAttr = "blob";

// A parameter the PLUG-IN moved by itself, on its way to the editor. There is exactly one thing
// that does that — the MIDI learn table, which stomps a channel or a pedal switch on the audio
// thread — and under VST3 the route is already there: the processor reports it through
// outputParameterChanges, the host turns that back into IEditController::setParamNormalized, and
// the panel follows. LV2 has no such route, because a control INPUT port belongs to the host and
// a plug-in may not write one; the standalone had to grow the same loop by hand for the same
// reason, and leaving it out there was a footswitch that changed the sound while the bat switch
// sat still.
//
// So the DSP half sends the echo to the UI, and the UI does two things with it: it tells its own
// controller, which repaints the panel, and it writes the control port — which is the only hand
// that CAN write it, and what makes the host's own automation lane agree with the audio.
inline constexpr const char *kMsgLv2ParamEcho = "RationsLv2ParamEcho";
inline constexpr const char *kLv2EchoIdAttr = "id";
inline constexpr const char *kLv2EchoValueAttr = "value";

// --- ports ------------------------------------------------------------------------------------

enum PortIndex : std::uint32_t {
    kPortAudioIn = 0,
    kPortAudioOutL = 1,
    kPortAudioOutR = 2,
    // MIDI in, and the UI's messages to the DSP.
    kPortAtomIn = 3,
    // The DSP's replies to the UI.
    kPortAtomOut = 4,
    kPortControlFirst = 5,
};

// The fixed half of the control inputs, in the controller's own declaration order so that a
// reader can hold this list and src/rationscontroller.cpp's initialize() side by side. The pedal
// half follows, walked out of kPedalParams, for the reason that table exists at all.
//
// NOT hand-counted and not trusted: tools/rations_ttlgen.cpp instantiates the controller and
// asserts that this list plus kPedalParams is exactly the set of parameters the controller
// declares outside the MIDI and feedback blocks. A parameter added to the plug-in and forgotten
// here fails the TTL build rather than becoming a control nobody can reach.
inline constexpr Steinberg::Vst::ParamID kFixedControlIds[] = {
    kBypassId,
    kInputGainId,
    kOutputGainId,
    kChannelId,

    kCleanGainId,
    kCrunchGainId,
    kOd1GainId,
    kOd2GainId,

    kCleanLevelId,
    kCrunchLevelId,
    kOd1LevelId,
    kOd2LevelId,

    kNoiseGateThresholdId,
    kBassId,
    kMiddleId,
    kTrebleId,

    kNoiseGateOnId,
    kToneStackOnId,
    kSlimId,
    kIrBlendId,

    kOutputModeId,
    kCalibrateInputId,
    kInputCalLevelId,
};
inline constexpr int kFixedControlCount =
    static_cast<int>(sizeof(kFixedControlIds) / sizeof(kFixedControlIds[0]));

// The feedback block, as control OUTPUT ports. Every one of these needs a ui:portNotification
// block in the TTL or it never reaches the UI: the UI extension says a host calls port_event()
// for control port INPUTS by default, so without those declarations the meters, the bank progress
// bar and the capture readout are permanently dead while the plug-in builds, loads and otherwise
// works perfectly.
inline constexpr Steinberg::Vst::ParamID kFeedbackIds[] = {
    kInputMeterId, kOutputMeterId, kBankProgressId, kActiveIndexId, kActiveChannelId,
};
inline constexpr int kFeedbackCount =
    static_cast<int>(sizeof(kFeedbackIds) / sizeof(kFeedbackIds[0]));

inline constexpr int kControlInCount = kFixedControlCount + kPedalParamCount;

inline constexpr std::uint32_t kPortFeedbackFirst = kPortControlFirst + kControlInCount;
// lv2:reportsLatency. The VST3 build reports latency through getLatencySamples(); LV2's way of
// saying the same thing is a control output port carrying the lv2:latency designation, so the
// figure is the same figure by construction — the wrapper copies it straight out of the processor.
inline constexpr std::uint32_t kPortLatency = kPortFeedbackFirst + kFeedbackCount;
inline constexpr std::uint32_t kPortCount = kPortLatency + 1;

// The ParamID a control-input port carries. Out of range returns 0, which is not a ParamID this
// plug-in uses.
inline constexpr Steinberg::Vst::ParamID controlPortParam(int index)
{
    if (index < 0 || index >= kControlInCount)
        return 0;
    if (index < kFixedControlCount)
        return kFixedControlIds[index];
    return kPedalParams[index - kFixedControlCount].id;
}

// --- what a control port carries ---------------------------------------------------------------
//
// PLAIN values, not normalized ones: an LV2 host draws its generic control from the port's own
// lv2:minimum / lv2:maximum and prints the number it holds, so a port carrying 0.63 would put
// "0.63" where the panel says "+12.0 dB". The VST3 build's automation lane says the real figure
// and this one has to as well.
//
// That means the wrapper needs each parameter's range, and this is the table that gives it. Every
// bound below is written as the SAME constant the controller declares the parameter from, so
// there is no second copy of a number — only a second statement of WHICH range each id uses. That
// statement is checked: tools/rations_ttlgen.cpp instantiates the controller and compares every
// row here against Parameter::toPlain at 0 and 1, its stepCount and its default, and refuses to
// write a TTL when they disagree.
struct ControlSpec {
    Steinberg::Vst::ParamID id;
    double min;
    double max;
    // VST3's stepCount: 0 for a continuous control, N for one with N+1 discrete values. A toggle
    // is 1, the four-way channel switch is 3, the twelve-way Delay sync is 11.
    int steps;
    double def; // plain
};

inline constexpr ControlSpec kFixedControlSpecs[kFixedControlCount] = {
    {kBypassId, 0.0, 1.0, 1, 0.0},
    {kInputGainId, ranges::kGainMin, ranges::kGainMax, 0, ranges::kGainDefault},
    {kOutputGainId, ranges::kGainMin, ranges::kGainMax, 0, ranges::kGainDefault},
    {kChannelId, 0.0, kChannelCount - 1, kChannelCount - 1, 0.0},

    {kCleanGainId, 0.0, 1.0, 0, 0.0},
    {kCrunchGainId, 0.0, 1.0, 0, 0.0},
    {kOd1GainId, 0.0, 1.0, 0, 0.0},
    {kOd2GainId, 0.0, 1.0, 0, 0.0},

    {kCleanLevelId, ranges::kLevelMin, ranges::kLevelMax, 0, ranges::kLevelDefault},
    {kCrunchLevelId, ranges::kLevelMin, ranges::kLevelMax, 0, ranges::kLevelDefault},
    {kOd1LevelId, ranges::kLevelMin, ranges::kLevelMax, 0, ranges::kLevelDefault},
    {kOd2LevelId, ranges::kLevelMin, ranges::kLevelMax, 0, ranges::kLevelDefault},

    {kNoiseGateThresholdId, ranges::kNgMin, ranges::kNgMax, 0, ranges::kNgDefault},
    {kBassId, ranges::kToneMin, ranges::kToneMax, 0, ranges::kToneDefault},
    {kMiddleId, ranges::kToneMin, ranges::kToneMax, 0, ranges::kToneDefault},
    {kTrebleId, ranges::kToneMin, ranges::kToneMax, 0, ranges::kToneDefault},

    {kNoiseGateOnId, 0.0, 1.0, 1, 1.0},
    {kToneStackOnId, 0.0, 1.0, 1, 1.0},
    {kSlimId, ranges::kSlimMin, ranges::kSlimMax, 0, ranges::kSlimDefault},
    {kIrBlendId, 0.0, 1.0, 0, 0.0},

    {kOutputModeId, 0.0, kOutputModeCount - 1, kOutputModeCount - 1,
     static_cast<double>(kOutputNormalized)},
    {kCalibrateInputId, 0.0, 1.0, 1, 0.0},
    {kInputCalLevelId, ranges::kCalMin, ranges::kCalMax, 0, ranges::kCalDefault},
};

// A pedal control's spec, derived from the one table that already decides everything about it.
// The step count follows the kind for the same reason the controller's switch does.
inline constexpr ControlSpec pedalControlSpec(int pedalIndex)
{
    const PedalParamSpec &spec = kPedalParams[pedalIndex];
    const int steps =
        spec.kind == PedalParamKind::Toggle
            ? 1
            : (spec.kind == PedalParamKind::List ? static_cast<int>(spec.max - spec.min) : 0);
    // A Toggle's default in kPedalParams is already 0 or 1, and a List's is already an index, so
    // both are plain values exactly as a Range's is.
    return ControlSpec{spec.id, spec.min, spec.max, steps, spec.def};
}

inline constexpr ControlSpec controlSpec(int port)
{
    if (port < 0 || port >= kControlInCount)
        return ControlSpec{0, 0.0, 1.0, 0, 0.0};
    if (port < kFixedControlCount)
        return kFixedControlSpecs[port];
    return pedalControlSpec(port - kFixedControlCount);
}

// Plain -> normalized, which is what every VST3 parameter queue carries. Clamped rather than
// trusted: a control port is host input and a host may write anything into it.
inline double controlNorm(const ControlSpec &spec, double plain)
{
    const double span = spec.max - spec.min;
    if (span <= 0.0)
        return 0.0;
    double n = (plain - spec.min) / span;
    return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
}

// Normalized -> plain, the inverse, with a stepped control snapped to a whole step so a host that
// reads the port back sees the value it would have set.
inline double controlPlain(const ControlSpec &spec, double norm)
{
    norm = norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
    const double plain = spec.min + norm * (spec.max - spec.min);
    if (spec.steps <= 0)
        return plain;
    const double step = (spec.max - spec.min) / static_cast<double>(spec.steps);
    if (step <= 0.0)
        return spec.min;
    return spec.min + std::floor((plain - spec.min) / step + 0.5) * step;
}

// --- state ------------------------------------------------------------------------------------
//
// The VST3 state blob is stored whole, under one key, and it is the authority for everything:
// the shared controls, the trims, the MIDI learn table, the pedalboard, the output section and
// the capture and IR paths. There is no second state format, so a project saved by either build
// is read by the same reader that has been proved against seven state versions.
//
// state:mapPath is layered ON TOP of that rather than replacing it, because it is the one thing
// the VST3 format cannot do: a host that moves or archives a session can relocate the files a
// plug-in references, but only for paths the plug-in handed it through abstract_path(). So each
// file path is stored twice — once abstracted, which is what survives a move, and once raw, which
// is what the blob itself contains.
//
// The raw copy is not redundant. At restore the wrapper has to answer one question — did the
// files move? — and the blob is the only other place the answer lives. Parsing it here to find
// out would mean a second reader for a format that already has one, which is exactly the drift
// this file keeps warning about; storing the raw path costs a few dozen bytes and answers it
// outright. When abstract and raw resolve to the same file, restore is the blob alone and the
// banks build once. When they differ, the wrapper re-sends the load for the relocated path and
// the bank the blob named is superseded before it finishes.
inline constexpr const char *kStateBlobUri = "https://github.com/rations/NAMp-rations#state";
// <prefix>Abstract / <prefix>Raw per slot, with the slot's own name appended.
inline constexpr const char *kStatePathAbstractPrefix =
    "https://github.com/rations/NAMp-rations#pathAbstract";
inline constexpr const char *kStatePathRawPrefix =
    "https://github.com/rations/NAMp-rations#pathRaw";
// Whether each capture slot was a FOLDER, as a bitmask over the four channels. Needed because a
// relocated path has to be re-sent with the same answer the user gave at the browser, and
// re-deriving it from the disk answers a different question — see kMsgIsDirAttr.
inline constexpr const char *kStateCaptureDirsUri =
    "https://github.com/rations/NAMp-rations#captureDirs";

// The path slots, in one order shared by save and restore. The four capture banks first, then the
// two IR slots, so a slot index below kPathSlotIrFirst is a channel index and nothing has to be
// decoded twice.
inline constexpr int kPathSlotIrFirst = kChannelCount;
inline constexpr int kPathSlotCount = kChannelCount + kIrSlotCount;
inline constexpr const char *kPathSlotName[kPathSlotCount] = {
    "Clean", "Crunch", "OD1", "OD2", "IrA", "IrB",
};

} // namespace lv2
} // namespace Rations
