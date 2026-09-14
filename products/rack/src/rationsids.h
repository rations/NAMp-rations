// Rations — a four-channel Neural Amp Modeler amp head, a raw VST3 plug-in (no framework).
//
// Based on NeuralAmpModelerPlugin by Steven Atkinson (MIT licence), and directly on the author's
// own NAMp, whose crossfade engine this plug-in runs four times — once per channel. The DSP core
// (NeuralAmpModelerCore, AudioDSPTools) is reused directly. Written against the VST3 SDK only.
//
// Parameter IDs 100-108 keep NAMp's numbering for the controls the two plug-ins share, so an
// author moving between them reads the same automation lanes. IDs NAMp used for controls Rations
// does not have are RETIRED rather than recycled — see below. The class UIDs are this amp's own
// and must never be confused with either NAMp's or the parent amp's: a host that loaded one
// expecting the other would silently give the user the wrong plug-in, and against the parent it
// would do so while reading the state blob successfully, because the two formats are deliberately
// wire-compatible. See the UID declarations at the foot of this file.

#pragma once

#include "engineconfig.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <algorithm>
#include <cmath>

namespace Rations
{

// Parameter IDs. Never change these after a release — projects embed them.
enum ParamIDs : Steinberg::Vst::ParamID {
    kBypassId = 100,
    kInputGainId = 101,          // -40 .. +40 dB, default 0
    kOutputGainId = 102,         // -40 .. +40 dB, default 0
    kNoiseGateThresholdId = 103, // -100 .. 0 dB, default -80
    kBassId = 104,               // 0 .. 10, default 5
    kMiddleId = 105,             // 0 .. 10, default 5
    kTrebleId = 106,             // 0 .. 10, default 5
    kNoiseGateOnId = 108,        // toggle, default on

    // Bass / Middle / Treble in or out of circuit, as one switch — the EQ bat on the faceplate.
    // 114 and not NAMp's 107, for the reason the retirement note below gives: retiring an ID is a
    // promise about a NUMBER, and the promise does not become void because the control came back.
    // A project written against a build in which 107 did not exist must not later find something
    // answering on that lane. It sits at 114 rather than in a fresh block because it belongs with
    // the shared signal-path controls at 100..108 and 114 is the first number after the
    // retirements that has never meant anything.
    kToneStackOnId = 114, // toggle, default on

    // Slim: which variant of a SlimmableContainer capture gets built, 0 = smallest, 1 = whole.
    // Reached from the icon left of the settings button rather than from a page, because it is a
    // hardware accommodation set once and then forgotten, not a control anyone plays.
    //
    // 115 and NOT NAMp's 110, which stays retired below, for the third time this file has had to
    // make the same point: retiring an ID is a promise about a NUMBER and the control coming back
    // does not release it. 115 is the first number after 114 that has never meant anything.
    //
    // Default 1.0 — the whole model — which is what every build before this one was hard-wired
    // to, so an existing project sounds the same. The sibling plug-ins default to 0.0; they are
    // making a different trade for a different user.
    kSlimId = 115, // 0 .. 1, default 1

    // RETIRED NAMp IDs — never reuse these numbers in this plug-in.
    //   107  Tone Stack on/off   The control DOES exist here now, at 114. See the note on the
    //                            output section below: the same rule, for the same reason.
    //   110  Slim                The control DOES exist here now, at 115. It was fixed at 1.0
    //                            and this note said permanently; it is user-settable because a
    //                            player on older hardware had no other way to buy back CPU. The
    //                            number stays dead: see 109/111/112 below for the rule.
    //   113  Capture             NAMp's single bank position. Rations has four banks, so the
    //                            meaning changed; a new ID per channel is used instead of
    //                            silently redefining this one under existing automation.
    //
    //   109  Output Mode         These three DO exist in this plug-in now, and they still carry
    //   111  Calibrate Input     NAMp's meaning — but at 150..152, not here. Retiring an ID is a
    //   112  Input Cal Level     promise about a NUMBER, and the promise does not become void
    //                            because the control came back; a project written against a
    //                            build in which 109 did not exist must not later find something
    //                            answering on that lane. Giving them fresh numbers costs one
    //                            line and keeps the retirement list a list of facts rather than
    //                            of intentions. It does mean Rations and NAMp put the same three
    //                            controls on different lanes, which is the price.

    // The channel. ONE list parameter rather than four mutually-exclusive booleans: that makes
    // "exactly one channel is on" a property of the type instead of an invariant four places
    // would have to agree about (RT, editor, setState, automation), and it gives the host a
    // single clean automation lane and a single MIDI target. Values are the Channel enum below.
    kChannelId = 120,

    // Position along each channel's own bank of captures. 0 = the first (lowest-gain) capture,
    // 1 = the last; the processor maps this onto p in [0, N-1] for that bank, so the dial's
    // physical travel matches the amp's evenly-spaced gain marks. Plain 0 .. 1 rather than an
    // index, because the bank size must not change a parameter's range under a host that has
    // already written automation against it. Order matches Channel.
    kCleanGainId = 121,
    kCrunchGainId = 122,
    kOd1GainId = 123,
    kOd2GainId = 124,

    // Cabinet page. 0 = IR A only, 1 = IR B only. INERT unless both slots are filled: with one IR
    // loaded that IR runs at unity and this parameter does nothing, because a naive a*A + b*B
    // with B silent would attenuate a one-IR user at every position but one.
    kIrBlendId = 130,

    // Per-channel output trim, on the settings page. A separate block from the gain dials above
    // rather than 125..128 beside them, because they are a different control: 121..124 say where
    // along a bank of captures a channel sits, and these say how loud that channel is once it has
    // been chosen. Order matches Channel.
    //
    // These exist because the captures are already loudness-normalized per capture (see the
    // Normalized decision in the notes) and are still not level-MATCHED: a high-gain channel is
    // far more compressed than a clean one, so it reads louder at the same measured loudness. The
    // residual is perceptual, it is a few dB, and no metadata field can tell the plug-in what it
    // is - only the player can.
    kCleanLevelId = 140,
    kCrunchLevelId = 141,
    kOd1LevelId = 142,
    kOd2LevelId = 143,

    // The output section, on the settings page below MIDI learn. Three controls, one block,
    // because the second two only mean anything when the first is set to Calibrated.
    //
    // These are NAMp's controls at Rations' own numbers — see the retirement note above for why
    // they are not at 109/111/112. What each one does is unchanged and is applied in exactly the
    // place NAMp applies it: the mode per BRANCH inside the crossfade, before the mix, because
    // adjacent captures of one amp differ in measured loudness by more than a decibel and not
    // monotonically, so compensating after the mix would step the level at every crossing.
    //
    // Default is Normalized, which is the upstream plug-in's default and this plug-in's previous
    // hard-wired behaviour, so an existing project sounds the same after this parameter exists as
    // it did before. NAMp defaults to Raw instead, because there a single dial sweeps a bank
    // whose whole point is that gain rises across it the way the amp's own control does, and
    // normalizing would flatten exactly that. Here the four channels are four different amps and
    // levelling them is the useful default.
    kOutputModeId = 150,     // list: 0 Raw, 1 Normalized, 2 Calibrated
    kCalibrateInputId = 151, // toggle, default off
    kInputCalLevelId = 152,  // -60 .. +60 dBu, default 12

    // RETIRED — the pedalboard, 300 .. 399. Never reuse these numbers in this plug-in.
    //
    // The parent plug-in put five pedals here, twenty lanes each: Boost 300, Chorus 320,
    // Flanger 340, Delay 360, Reverb 380. This project has no pedalboard. The pedals are separate
    // plug-ins loaded into the rack, where they carry their own parameter IDs in their own
    // namespace, and nothing in this file can collide with them.
    //
    // The whole block stays dead, under the same rule the retirement list above states three
    // times over: retiring an ID is a promise about a NUMBER, and the promise does not become
    // void because the control left. A project written against a build in which 300 meant
    // "Boost on" must not later find this plug-in answering on that lane with something else.
    // Ninety-nine dead lanes is a cheap price for that, and nothing is short of room — the next
    // block after this one starts at 1000.

    // Hidden, read-only parameters (processor -> editor via output parameter changes; never
    // automated, never persisted). The meters carry the per-block peak level mapped to 0 .. 1
    // over the meter dB range below.
    kInputMeterId = 200,
    kOutputMeterId = 201,
    // Fraction of all four banks whose entries are built and primed, 0 .. 1. Every channel
    // becomes switchable well before this reaches 1: the workers build the entry the RT thread
    // needs first, then one entry of each other channel, then fill in.
    kBankProgressId = 202,
    // Which capture is actually sounding in the ACTIVE channel, as a normalized index over that
    // channel's bank. Read by the editor to name the current capture; distinct from the channel's
    // gain parameter, which is where the dial is.
    kActiveIndexId = 203,
    // Which channel is actually SOUNDING, in kChannelId's own value space. Not a duplicate of
    // kChannelId: that one is the request, and this one is the answer. A switch whose target
    // capture is still being built is held rather than faked, and the panel LEDs read this so a
    // lamp never lights over a channel the audio has not reached yet.
    kActiveChannelId = 204,

    // The MIDI-mapped parameter block: 1000 + cc for CC 0 .. 127, and 1128 for Program Change.
    // These exist because a footswitch's messages do not arrive as MIDI at all - they arrive as
    // parameter changes, so they need real parameters to land on. Both routes are documented at
    // the top of midilearn.h, with the SDK sites they were verified against.
    //
    // The CC parameters carry flags 0 - NOT kIsHidden, which the SDK documents as implying
    // kIsReadOnly and would make them unwritable, defeating the whole point. Flags 0 is the SDK's
    // own pattern for MIDI-mapped parameters (public.sdk/samples/vst/mda-vst3/source/
    // mdaJX10Controller.cpp:147-159). They live in kMidiUnitId so hosts group them out of the way.
    kMidiCcBaseId = 1000,
    kMidiCcLastId = kMidiCcBaseId + 127,
    // Program Change. Also the ProgramListID, which is not a coincidence and not free choice:
    // EditControllerEx1's ProgramList builds its parameter with the list's own id as the ParamID
    // (public.sdk/source/vst/vsteditcontroller.cpp:603-606), so the two numbers are the same
    // number by construction.
    kMidiProgramChangeId = 1128,
};

// Units. The root unit is everything a player touches; the MIDI unit holds the 129 parameters
// that exist only so that CC and Program Change have somewhere to arrive, and exists so a host
// can fold them away instead of listing them beside Bass and Treble.
inline constexpr Steinberg::Vst::UnitID kMidiUnitId = 1;
inline constexpr Steinberg::Vst::ProgramListID kMidiProgramListId = kMidiProgramChangeId;
inline constexpr int kMidiCcCount = 128;
inline constexpr int kMidiProgramCount = 128;

// Channel, kChannelCount and kChannelDefaultName are defined in engineconfig.h, which carries no
// VST3 dependency, because the channel rack and the offline switch proof both name channels without
// linking the plug-in.

// The per-channel gain parameter, indexed by Channel.
inline constexpr Steinberg::Vst::ParamID kChannelGainId[kChannelCount] = {
    kCleanGainId, kCrunchGainId, kOd1GainId, kOd2GainId};

// The per-channel output trim, indexed by Channel.
inline constexpr Steinberg::Vst::ParamID kChannelLevelId[kChannelCount] = {
    kCleanLevelId, kCrunchLevelId, kOd1LevelId, kOd2LevelId};

// Decode kChannelId's normalized value to a Channel, and back. A kIsList parameter with N steps
// reports value i as i / (N - 1), so this is that inverse, rounded and clamped: a host is free to
// hand over any double in [0, 1], including one that lands between steps.
//
// These live here rather than with the processor because they are not the processor's: the
// controller decodes the same parameter to work out which channel's captures a title should
// describe, and the editor decodes it to light a lamp. One definition, three readers.
inline Channel channelFromNorm(double norm)
{
    const double steps = static_cast<double>(kChannelCount - 1);
    int i = static_cast<int>(std::lround(std::clamp(norm, 0.0, 1.0) * steps));
    i = std::clamp(i, 0, kChannelCount - 1);
    return static_cast<Channel>(i);
}

inline constexpr double normFromChannel(Channel ch)
{
    const int i = (ch < 0) ? 0 : (ch >= kChannelCount ? kChannelCount - 1 : static_cast<int>(ch));
    return static_cast<double>(i) / static_cast<double>(kChannelCount - 1);
}

// What kOutputModeId selects, and the only definition of the mapping between its value space and
// its normalized value. The conversion is trivial and that is exactly why it lives here: the parent
// plug-in spells `norm * 2.0 + 0.5` out at three sites and `index * 0.5` at a fourth, so the number
// of entries is written into four files and a fourth mode could not be added without finding all
// of them. Order is the upstream plug-in's and must not be reordered — it is a saved value.
enum OutputMode : int {
    kOutputRaw = 0,        // the model's own level, untouched
    kOutputNormalized = 1, // each capture's measured loudness brought to a common target
    kOutputCalibrated = 2, // the capture's stated output level against the user's interface level
    kOutputModeCount = 3,
};

inline constexpr double normFromOutputMode(OutputMode mode)
{
    return static_cast<double>(mode) / static_cast<double>(kOutputModeCount - 1);
}

// Snapped through a clamp rather than trusted: this decodes an automation value and a state blob,
// both of which are untrusted input, and an out-of-range mode would index nothing in particular.
inline OutputMode outputModeFromNorm(double norm)
{
    const double steps = static_cast<double>(kOutputModeCount - 1);
    int i = static_cast<int>(std::lround(std::clamp(norm, 0.0, 1.0) * steps));
    i = std::clamp(i, 0, kOutputModeCount - 1);
    return static_cast<OutputMode>(i);
}

// Plain-value ranges shared by the processor (denormalization) and the controller
// (RangeParameter setup). Keep the two sides in sync via these.
namespace ranges
{
inline constexpr double kGainMin = -40.0, kGainMax = 40.0, kGainDefault = 0.0;
inline constexpr double kNgMin = -100.0, kNgMax = 0.0, kNgDefault = -80.0;
inline constexpr double kToneMin = 0.0, kToneMax = 10.0, kToneDefault = 5.0;
// Level-meter display range (dB): a linear peak is mapped to 0 .. 1 across this window before
// travelling to the editor.
inline constexpr double kMeterMinDb = -70.0, kMeterMaxDb = 0.0;
// Per-channel trim (dB). Deliberately NARROW, and the narrowness is the design: what this control
// corrects is the perceptual residual left after per-capture loudness normalization, which is a
// few dB, so a wider range would spend most of the slider's travel on values nobody wants and
// leave the useful part of it a third the size. Widening it before a release costs nothing;
// widening it after one changes what saved automation means, so it starts narrow.
inline constexpr double kLevelMin = -12.0, kLevelMax = 12.0, kLevelDefault = 0.0;
// The interface's calibration level (dBu), used only when kOutputModeId is Calibrated or
// kCalibrateInputId is on. The range and the default are the upstream plug-in's, and the default is
// not arbitrary: +12 dBu at 0 dBFS is the commonest figure among audio interfaces, so a player who
// enables calibration without knowing their interface's number is already close.
inline constexpr double kCalMin = -60.0, kCalMax = 60.0, kCalDefault = 12.0;
// Slim. Normalized and plain are the same thing here, so these exist for the state reader and the
// default rather than for a denormalization. 1.0 is the whole model and is the default; see
// kSlimId, and see buildCaptureModel for what a value between the two selects — the threshold
// test is `slim < maxValue`, exclusive, so 1.0 can never select anything but the largest variant,
// which is exactly the intent.
inline constexpr double kSlimMin = 0.0, kSlimMax = 1.0, kSlimDefault = 1.0;
} // namespace ranges

// The pedalboard's parameter table, its five faces and every helper that read them lived here.
// All of it is gone with the feature (see the retirement note in ParamIDs above): a pedal is a
// separate plug-in in the rack now, and it declares its own parameters to the host that loads it.
//
// The one piece of that machinery that OUTLIVED it is the length-prefixed slot the pedal block
// occupied in the state blob. That slot is the format's, not the pedalboard's — see the state
// version note below, which is where the reasoning now lives.

// The MIDI learn table's own length prefix bound. A blob is untrusted input, so the count is
// checked before it is believed: generous enough that a blob from a build with more rows still
// loads what this one understands, small enough that a corrupt length cannot make the reader
// spin. 256 rather than something larger only because a learn table is a list of footswitch
// buttons and nobody has 256 of those.
inline constexpr Steinberg::int32 kMidiRowStateMax = 256;

// The same bound for the length-prefixed slot that used to hold the pedalboard. This build writes
// a count of 0 there and skips whatever it finds; the bound is what stops a corrupt or hostile
// count from making that skip loop forever. Kept at the original 1024 so a blob written by the
// parent plug-in — twenty-five pedal values — is still read and skipped rather than rejected.
inline constexpr Steinberg::int32 kPedalStateMax = 1024;

// Version of the state blob written by getState and accepted by setState / setComponentState.
// Version 1 ended after the two IR paths; version 2 appends the MIDI learn table; version 3
// appends the four channel trims; version 4 appends the output section and the four capture
// sources; version 5 appends the pedalboard, length-prefixed. An older blob is still loaded - it is
// a project saved before the pedal, the trims or the loader could do anything - so this is a
// minimum-compatible marker rather than a gate, and the readers check the version before reading
// anything an older writer would not have written.
//
// What an older project opens as: every trim at 0 dB, which is exactly the level it was mixed at;
// output mode at Normalized, which is what every build before version 4 was hard-wired to; and
// calibration off. The one thing it CANNOT open as is the captures it was mixed with, because a
// version 3 build resolved those from inside the bundle and never wrote down where they came
// from. There is no honest way to recover that, so such a project opens with four empty channels
// and the settings page asking for them - silence a user can fix, rather than a guess at a path.
//
// Version 6 does not APPEND anything, which is why it is a version at all. It gives the MIDI
// learn table a length prefix, in the middle of the blob where that table has always sat, because
// the pedalboard's five footswitch rows made kMidiLearnRowCount grow from four to nine - and a
// fixed count in the middle of a blob is unreadable by a build that disagrees about it. A version
// 2-5 reader would take the first four words and then read five of them as channel trims; a
// version 6 reader given an old blob would eat five of the trims as bindings. So the count is
// written down, an old blob is read as exactly kMidiLearnRowsV2 rows (frozen at 4 in midilearn.h),
// and rows beyond what this build has are skipped rather than refused - which is what lets a blob
// from a build with MORE rows still open here.
//
// Version 7 appends the EQ switch — one double, after the pedalboard block, which is the end of
// the blob. It goes at the END and not beside the other seven shared controls at the front, even
// though that is where it belongs by meaning: those eight doubles are the first thing every
// reader since version 1 takes, so inserting a ninth would move every field after it and make
// every existing project unreadable. A version 1-6 project opens with EQ ON, which is what every
// build before this one was hard-wired to and so is what that project actually sounded like.
//
// Version 8 appends Slim — one double, after the EQ switch, which is again the end of the blob and
// again not where it belongs by meaning, for the reason version 7 gives. A version 1-7 project
// opens at 1.0, the whole model, which is what every build before this one was hard-wired to and
// so is what that project actually sounded like. That is also the rule the upstream plug-in
// applies to its own older presets, and for the same reason: the safe default for a size setting
// is the size the audio was made at.
// Version 9 REMOVES the pedalboard's contents without removing its slot, and the distinction is
// the whole point. The five pedals are gone from this plug-in — they are separate plug-ins in the
// rack now — so there are no pedal values to write. But the slot they sat in is length-prefixed,
// and it sits in the MIDDLE of the blob with the EQ switch and Slim after it, so deleting the
// slot would move both of those and make every version 5-8 project unreadable for the sake of
// four bytes.
//
// So the slot stays and the count is written as 0, which is what the length prefix was FOR: its
// own note said it existed because "a pedal growing a knob is a thing that will happen more than
// once", and a count going to zero is that same mechanism doing that same job. The reader reads
// the count and skips that many doubles, so a blob from the parent plug-in still opens here with
// everything except its pedals, and a blob written here still opens THERE with its pedals at
// their defaults. Neither direction needed a special case; both fall out of the prefix.
//
// The version still moves to 9 because what the blob MEANS changed even though its shape did not:
// a version 8 blob asserts pedal values that were live, and a version 9 blob asserts there are
// none. A reader that cares about the difference can tell, which is the only thing a version
// number is for.
inline constexpr Steinberg::int32 kStateVersion = 9;

// The cabinet's two IR slots. Two, not N: the second is a blend partner for the first, and a list
// of them would be a different feature with a different UI. Slot 0 is A, slot 1 is B.
inline constexpr int kIrSlotCount = 2;

// Message IDs for controller -> processor IR loading (IConnectionPoint).
// Attribute "path" carries a UTF-8 byte string (setBinary); empty = clear the slot.
inline constexpr const char *kMsgLoadIrA = "RationsLoadIRA";
inline constexpr const char *kMsgLoadIrB = "RationsLoadIRB";
inline constexpr const char *kMsgPathAttr = "path";
// Indexed by slot, so neither side has to spell out which of the two it means twice.
inline constexpr const char *kMsgLoadIr[kIrSlotCount] = {kMsgLoadIrA, kMsgLoadIrB};

// Capture loading, controller -> processor, one message per channel. Attribute "path" carries a
// UTF-8 byte string (setBinary, never setString: setString is UTF-16 into a caller-sized buffer and
// the SDK's own text-message helper silently truncates at 255 characters). An empty path clears the
// channel. Attribute "isDir" says which of ModelBank's two loaders to call, and it is the browser's
// own answer rather than a fresh stat of the path: a path can stop being a directory between the
// click and the question, and the user's intent was fixed at the click.
//
// Unlike the parent plug-in there is no mutual exclusion to maintain here. NAMp's single-capture
// and bank loaders clear each other because it has one bank; each channel here has exactly one
// source, so a load simply replaces what that channel had.
inline constexpr const char *kMsgLoadCaptureClean = "RationsLoadCaptureClean";
inline constexpr const char *kMsgLoadCaptureCrunch = "RationsLoadCaptureCrunch";
inline constexpr const char *kMsgLoadCaptureOd1 = "RationsLoadCaptureOD1";
inline constexpr const char *kMsgLoadCaptureOd2 = "RationsLoadCaptureOD2";
inline constexpr const char *kMsgLoadCapture[kChannelCount] = {
    kMsgLoadCaptureClean, kMsgLoadCaptureCrunch, kMsgLoadCaptureOd1, kMsgLoadCaptureOd2};
inline constexpr const char *kMsgIsDirAttr = "isDir";

// The user's name for a channel, controller -> processor: int "row" plus a UTF-8 "name". It travels
// to the processor for one reason only - the processor is what writes the state blob - and nothing
// on the audio path ever reads it. An empty name means "no override": the channel falls back to the
// basename of whatever is loaded, and then to its default name.
inline constexpr const char *kMsgChannelName = "RationsChannelName";
inline constexpr const char *kMsgNameAttr = "name";

// The editor asking the processor to re-send its capability message. The four banks are scanned,
// parsed and built on worker threads, so at the moment a load is acknowledged the capture names do
// not exist yet and the first caps message necessarily reports zero. The reply that carries the
// real counts is the one to this request, made once the workers have caught up.
inline constexpr const char *kMsgRequestCaps = "RationsRequestCaps";

// Slim, controller -> processor. Attribute "slim" carries the 0..1 value (setFloat).
//
// A MESSAGE and not the parameter queue, for the same reason the capture paths are messages: what
// applying this value does is rebuild every model in every loaded bank, and ModelBank::post takes
// a mutex. The parameter still travels the ordinary way and the processor still records it, but
// only so getState has it — the RT thread must never be the one that asks for the rebuild.
//
// It is sent when the knob is RELEASED, not while it moves. A drag that published every step
// would ask for one rebuild per pixel; the epoch counter would cancel all but the last, so it
// would be correct, but every bank would sit at ramped silence for the whole gesture instead of
// for one build.
inline constexpr const char *kMsgSetSlim = "RationsSetSlim";
inline constexpr const char *kSlimAttr = "slim";

// MIDI learn, editor <-> processor. The table lives in the processor, because a footswitch has to
// work with the editor closed; these messages are how the editor arms a row, clears one, and finds
// out what the table now says. The reply is polled rather than pushed for the same reason the
// capture names are: the processor cannot call the controller, and the moment a learn completes is
// on the audio thread.
//   kMsgMidiLearn   controller -> processor, int "row": arm that row, or -1 to disarm.
//   kMsgMidiClear   controller -> processor, int "row": forget that row's binding.
//   kMsgRequestMidi controller -> processor: send the table.
//   kMsgMidiTable   processor -> controller: the packed bindings, plus which row is armed.
inline constexpr const char *kMsgMidiLearn = "RationsMidiLearn";
inline constexpr const char *kMsgMidiClear = "RationsMidiClear";
inline constexpr const char *kMsgRequestMidi = "RationsRequestMidi";
inline constexpr const char *kMsgMidiTable = "RationsMidiTable";
inline constexpr const char *kMidiRowAttr = "row";
// The bindings, as kMidiLearnRowCount little-endian uint32 words in row order - the same packing
// midilearn.h defines and the state blob stores, so there is one layout and not three.
inline constexpr const char *kMidiTableAttr = "table";
inline constexpr const char *kMidiArmedAttr = "armed";

// Capabilities travel processor -> controller after every load or clear, so the editor can name
// the capture each dial is sitting on and can disable what the current capture set does not
// support.
inline constexpr const char *kMsgModelCaps = "RationsModelCaps";
// Per-channel attributes, each named by its own prefix plus kChannelDefaultName[c] - so the wire
// format is keyed by channel rather than positional, and adding an attribute cannot shift another
// channel's answer the way the names blob below could.
inline constexpr const char *kCapsEntryCountAttr = "entryCount";
// 1 when that channel's source is a directory of captures rather than a single file. The editor
// needs it to word the row ("12 captures" against "single capture") and cannot derive it: a bank of
// one is a bank of one whichever way it was loaded.
inline constexpr const char *kCapsIsDirAttr = "isDir";
// What the loaded captures actually state about their own levels, so the editor can grey an output
// mode the current captures cannot honour. These are read off the built bank entries and must be
// the REAL values: the parent plug-in hard-codes the level pair to zero here, with the result that
// Calibrated and the whole input-calibration block are permanently dead in its shipped build even
// for captures that do carry the metadata. That is a bug to avoid, not a pattern to follow.
inline constexpr const char *kCapsHasLoudnessAttr = "hasLoudness";
inline constexpr const char *kCapsHasInLevelAttr = "hasInLevel";
inline constexpr const char *kCapsHasOutLevelAttr = "hasOutLevel";
// Whether that channel's captures are slimmable containers at all. The editor shows the Slim icon
// only when at least one loaded channel says yes, exactly as the two plug-ins this one descends
// from show theirs only for a slimmable model: the older captures the trainer now calls A1 have
// one variant, so for them the control cannot do anything and an icon offering it would be a lie.
inline constexpr const char *kCapsSlimmableAttr = "slimmable";
// The capture filenames of every channel, in the same gain order the dials sweep, joined by '\n'
// within a channel and by '\f' between channels, carried as UTF-8 through setBinary (setString
// would need UTF-16 and a fixed buffer). Re-deriving the order in the editor would duplicate the
// workers' filename sort, which is exactly the kind of thing that drifts.
inline constexpr const char *kCapsNamesAttr = "names";

// Generated fresh for this plug-in, with uuidgen, and derived from nobody's — not NAMp's, and not
// the parent amp's either. That second half is the one that cost something to get right.
//
// This amp and the parent Rations amp declared the SAME two UIDs for as long as this tree has
// existed, because it began as a copy of that one. On its own that is merely wrong; what makes it
// dangerous is the state format directly above, which is wire-compatible with the parent's BY
// DESIGN — a version 9 blob opens there and a version 8 blob opens here, both by way of the pedal
// slot's length prefix. A host matches a plug-in in a saved project by class UID, so two bundles
// answering to one UID means a host may load either and the wrong one then reads the blob
// SUCCESSFULLY, with no error on any interface: the user gets the other amp and a panel that
// half-matches their project.
//
// The parent keeps its UIDs because its bundle has shipped and changing them would orphan every
// saved project made against it. This one has never been published, so the fresh pair is free
// here and belongs here. Retiring works the way the parameter IDs' retirement does: the old pair
// is spent and must never be typed into this tree again.
static DECLARE_UID(RationsProcessorUID, 0xDFFE5D10, 0xF2624DEA, 0xB56D4F5E, 0x64F5469E);
static DECLARE_UID(RationsControllerUID, 0x3995BB20, 0xC17245BC, 0x9AFBC4BB, 0xD8F505F0);

} // namespace Rations
