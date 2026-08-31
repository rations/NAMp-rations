// Rations — a four-channel Neural Amp Modeler amp head, a raw VST3 plug-in (no framework).
//
// Based on NeuralAmpModelerPlugin by Steven Atkinson (MIT licence), and directly on the author's
// own NAMp, whose crossfade engine this plug-in runs four times — once per channel. The DSP core
// (NeuralAmpModelerCore, AudioDSPTools) is reused directly. Written against the VST3 SDK only.
//
// Parameter IDs 100-108 keep NAMp's numbering for the controls the two plug-ins share, so an
// author moving between them reads the same automation lanes. IDs NAMp used for controls Rations
// does not have are RETIRED rather than recycled — see below. The class UIDs are Rations' own and
// must never be confused with NAMp's: a host that loaded one expecting the other would silently
// give the user the wrong plug-in.

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

    // RETIRED NAMp IDs — never reuse these numbers in this plug-in.
    //   107  Tone Stack on/off   Bass/Middle/Treble are always on here, by design.
    //   110  Slim                fixed at 1.0 (full size), permanently. The captures support a
    //                            smaller variant and it would cost less CPU; this plug-in always
    //                            plays them whole, so there is nothing here to expose.
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

    // --- The pedalboard, 300 .. 399 ---------------------------------------------------------
    //
    // One contiguous block for the feature, twenty lanes per pedal, which is this file's own
    // convention and the reason the trims went to 140 rather than into the gap at 125. 300 is far
    // above everything retired (107, 109-113) and far below the MIDI block at 1000, so neither
    // can ever be reached by an off-by-something here.
    //
    // Twenty per pedal is deliberate slack. A pedal has at most five controls today and the
    // widest could plausibly grow to eight; leaving room means adding a knob later is a state
    // change (see kStateVersion) and not an ID change, and an ID change is the one thing this
    // file exists to make impossible. The ORDER within a pedal is fixed by kPedalParams below and
    // is what the state blob is written in, so entries may be appended to a pedal but never
    // reordered.
    //
    // Every pedal's <base> + 0 is its footswitch. That regularity is load-bearing: it is what
    // lets the MIDI-learn table take a pedal's on/off as an ordinary row without knowing which
    // pedal it is, and what lets the editor draw five identical footswitches from one loop.
    kBoostBaseId = 300,
    kChorusBaseId = 320,
    kFlangerBaseId = 340,
    kDelayBaseId = 360,
    kReverbBaseId = 380,
    kPedalIdStride = 20,

    // Boost - Ibanez TS-9. Drive sets the clipping amplifier's feedback resistance, Tone the
    // second-order low-pass after it, Level the output attenuator.
    kBoostOnId = kBoostBaseId + 0,
    kBoostDriveId = kBoostBaseId + 1,
    kBoostToneId = kBoostBaseId + 2,
    kBoostLevelId = kBoostBaseId + 3,

    kChorusOnId = kChorusBaseId + 0,
    kChorusRateId = kChorusBaseId + 1,
    kChorusDepthId = kChorusBaseId + 2,
    kChorusMixId = kChorusBaseId + 3,

    kFlangerOnId = kFlangerBaseId + 0,
    kFlangerRateId = kFlangerBaseId + 1,
    kFlangerDepthId = kFlangerBaseId + 2,
    kFlangerManualId = kFlangerBaseId + 3,
    kFlangerRegenId = kFlangerBaseId + 4,

    // Delay. kDelaySyncId is a list, not a toggle: "free" is one of its values rather than a
    // second parameter, so a host automating it cannot express "synced AND free at once".
    kDelayOnId = kDelayBaseId + 0,
    kDelayTimeId = kDelayBaseId + 1,
    kDelayFeedbackId = kDelayBaseId + 2,
    kDelayToneId = kDelayBaseId + 3,
    kDelayMixId = kDelayBaseId + 4,
    kDelaySyncId = kDelayBaseId + 5,
    kDelayPingPongId = kDelayBaseId + 6,

    kReverbOnId = kReverbBaseId + 0,
    kReverbDecayId = kReverbBaseId + 1,
    kReverbToneId = kReverbBaseId + 2,
    kReverbPreDelayId = kReverbBaseId + 3,
    kReverbMixId = kReverbBaseId + 4,

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
// The pedalboard's own Unit, so a host's generic list groups twenty-five pedal controls together
// rather than interleaving them with Bass, Middle and Treble.
inline constexpr Steinberg::Vst::UnitID kPedalUnitId = 2;
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
} // namespace ranges

// --- The pedalboard's parameter table ---------------------------------------------------------
//
// ONE table, read by four things that must never disagree: the controller declares its parameters
// from it, the processor sizes its atomic array and writes its state block from it, the editor
// draws its knobs from it, and rations_pedalcheck sweeps it. Everything about a pedal control -
// its range, its default, how it prints, which pedal it belongs to and where it sits on the face -
// is decided here and nowhere else.
//
// This is the same reasoning as `namespace ranges` above, which exists because the controller's
// RangeParameter and the processor's denorm() were two places to write one number. Five pedals
// with four or five controls each would have been twenty-five such places.
enum PedalIndex : int {
    kPedalBoost = 0,
    kPedalChorus,
    kPedalFlanger,
    kPedalDelay,
    kPedalReverb,
};
inline constexpr int kPedalCount = 5;

// How a control behaves, which is all the controller needs to pick a parameter class.
enum class PedalParamKind {
    Toggle, // step count 1; the footswitches and Ping-Pong
    Range,  // Vst::RangeParameter over [min, max]
    List,   // Vst::StringListParameter; the strings come from kDelaySyncNames
};

struct PedalParamSpec {
    Steinberg::Vst::ParamID id;
    int pedal;          // PedalIndex; which face this control is drawn on
    const char *title;  // host-visible, disambiguated: "Delay Tone", not "Tone"
    const char *legend; // drawn under the knob, in the pedal's own words: "Tone"
    const char *unit;   // nullptr = none
    PedalParamKind kind;
    double min, max, def;
    int precision;
};

// Delay sync divisions. "Free" is a VALUE here rather than a separate toggle, so a host cannot
// automate the delay into being synced and free at the same time - the same reasoning that made
// the active channel one list parameter instead of four booleans.
inline constexpr int kDelaySyncCount = 12;
inline constexpr const char *kDelaySyncNames[kDelaySyncCount] = {
    "Free",  "1/1",   "1/2",    "1/4.",  "1/4",  "1/4T",
    "1/8.",  "1/8",   "1/8T",   "1/16.", "1/16", "1/16T",
};
// Beats per repeat for each of the above, index 0 unused (Free reads kDelayTimeId instead).
inline constexpr double kDelaySyncBeats[kDelaySyncCount] = {
    0.0, 4.0, 2.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.375, 0.25, 1.0 / 6.0,
};

// The order here is the order the state blob is written in. Entries may be APPENDED to a pedal;
// they may never be reordered, or an old project's values land on the wrong controls.
inline constexpr PedalParamSpec kPedalParams[] = {
    // Boost - the TS-9. 0..10 rather than 0..1 or a percentage because that is what is printed
    // around the knob on the pedal being modelled, and the mapping to the circuit (Drive to the
    // clipping stage's feedback resistance, Tone to the second-order low-pass) happens in the DSP.
    {kBoostOnId, kPedalBoost, "Boost", "", nullptr, PedalParamKind::Toggle, 0, 1, 0, 0},
    {kBoostDriveId, kPedalBoost, "Boost Drive", "Drive", nullptr, PedalParamKind::Range, 0, 10, 5, 1},
    {kBoostToneId, kPedalBoost, "Boost Tone", "Tone", nullptr, PedalParamKind::Range, 0, 10, 5, 1},
    {kBoostLevelId, kPedalBoost, "Boost Level", "Level", nullptr, PedalParamKind::Range, 0, 10, 5, 1},

    {kChorusOnId, kPedalChorus, "Chorus", "", nullptr, PedalParamKind::Toggle, 0, 1, 0, 0},
    {kChorusRateId, kPedalChorus, "Chorus Rate", "Rate", "Hz", PedalParamKind::Range, 0.1, 10.0, 0.8, 2},
    {kChorusDepthId, kPedalChorus, "Chorus Depth", "Depth", "%", PedalParamKind::Range, 0, 100, 50, 0},
    {kChorusMixId, kPedalChorus, "Chorus Mix", "Mix", "%", PedalParamKind::Range, 0, 100, 50, 0},

    // Flanger. Regen is SIGNED: negative feedback gives the hollow "jet", positive gives peaks,
    // and they are different enough that a player wants both from one control rather than a
    // polarity switch. 95 rather than 100 because unity feedback in a comb filter does not decay.
    {kFlangerOnId, kPedalFlanger, "Flanger", "", nullptr, PedalParamKind::Toggle, 0, 1, 0, 0},
    {kFlangerRateId, kPedalFlanger, "Flanger Rate", "Rate", "Hz", PedalParamKind::Range, 0.05, 5.0, 0.3, 2},
    {kFlangerDepthId, kPedalFlanger, "Flanger Depth", "Depth", "%", PedalParamKind::Range, 0, 100, 70, 0},
    {kFlangerManualId, kPedalFlanger, "Flanger Manual", "Manual", "%", PedalParamKind::Range, 0, 100, 30, 0},
    {kFlangerRegenId, kPedalFlanger, "Flanger Regen", "Regen", "%", PedalParamKind::Range, -95, 95, 50, 0},

    // Delay. Time is in ms and is IGNORED while Sync names a division; the knob stays live and
    // keeps its value so unsyncing returns to where the player left it.
    {kDelayOnId, kPedalDelay, "Delay", "", nullptr, PedalParamKind::Toggle, 0, 1, 0, 0},
    {kDelayTimeId, kPedalDelay, "Delay Time", "Time", "ms", PedalParamKind::Range, 20, 2000, 400, 0},
    // "Repeats" rather than "Feedback" on the enclosure: the word is the one an outer knob's
    // legend has room for (68 px against 62 at kPedalLabelSize, caught by panelrender's text
    // audit), and it is what the object being modelled prints - Boss abbreviates to F.BACK, MXR
    // prints REGEN, and "Repeats" says the same thing without an abbreviation. The HOST still sees
    // "Delay Feedback", which is the name that has to be unambiguous in an automation lane.
    {kDelayFeedbackId, kPedalDelay, "Delay Feedback", "Repeats", "%", PedalParamKind::Range, 0, 95, 35, 0},
    {kDelayToneId, kPedalDelay, "Delay Tone", "Tone", nullptr, PedalParamKind::Range, 0, 10, 5, 1},
    {kDelayMixId, kPedalDelay, "Delay Mix", "Mix", "%", PedalParamKind::Range, 0, 100, 30, 0},
    {kDelaySyncId, kPedalDelay, "Delay Sync", "Sync", nullptr, PedalParamKind::List, 0, kDelaySyncCount - 1, 0, 0},
    {kDelayPingPongId, kPedalDelay, "Delay Ping-Pong", "Ping", nullptr, PedalParamKind::Toggle, 0, 1, 0, 0},

    {kReverbOnId, kPedalReverb, "Reverb", "", nullptr, PedalParamKind::Toggle, 0, 1, 0, 0},
    {kReverbDecayId, kPedalReverb, "Reverb Decay", "Decay", nullptr, PedalParamKind::Range, 0, 10, 4, 1},
    {kReverbToneId, kPedalReverb, "Reverb Tone", "Tone", nullptr, PedalParamKind::Range, 0, 10, 5, 1},
    {kReverbPreDelayId, kPedalReverb, "Reverb Pre-delay", "Pre", "ms", PedalParamKind::Range, 0, 200, 20, 0},
    {kReverbMixId, kPedalReverb, "Reverb Mix", "Mix", "%", PedalParamKind::Range, 0, 100, 25, 0},
};

// The largest pedal-parameter count this build will accept out of a state blob. A blob is
// untrusted input, so the length prefix is bounded before it is believed: this is generous enough
// that a future build with far more controls still loads what this one understands, and small
// enough that a corrupt length cannot make the reader spin.
inline constexpr Steinberg::int32 kPedalStateMax = 1024;

// The same bound for the MIDI learn table's own length prefix, which state version 6 gave it for
// the same reason: the table stopped being four rows the moment the pedalboard wanted five more,
// and a fixed count in the middle of a blob makes every later row of it unreadable when the count
// changes. 256 rather than 1024 only because a learn table is a list of footswitch buttons and
// nobody has 1024 of those.
inline constexpr Steinberg::int32 kMidiRowStateMax = 256;

// Derived, never hand-counted. The first version of this line carried a literal, got it wrong by
// one, and the compiler caught it — but the same literal is also what the state blob writes as its
// length prefix, where a wrong value would have been a silently truncated preset rather than a
// build error. So it is computed.
inline constexpr int kPedalParamCount =
    static_cast<int>(sizeof(kPedalParams) / sizeof(kPedalParams[0]));

// Each pedal's footswitch is its base id, which is what lets the editor draw five identical
// switches and the MIDI table learn one without knowing which pedal it is.
inline constexpr Steinberg::Vst::ParamID kPedalOnId[kPedalCount] = {
    kBoostOnId, kChorusOnId, kFlangerOnId, kDelayOnId, kReverbOnId,
};

// Index of a pedal parameter in kPedalParams, or -1. Linear over 24 entries and called off the
// audio thread only (the processor keeps its own dense array, indexed the same way).
inline constexpr int pedalParamIndex(Steinberg::Vst::ParamID id)
{
    for (int i = 0; i < kPedalParamCount; ++i)
        if (kPedalParams[i].id == id)
            return i;
    return -1;
}
inline constexpr bool isPedalParam(Steinberg::Vst::ParamID id)
{
    return pedalParamIndex(id) >= 0;
}

// kPedalParams is grouped by pedal, so each pedal owns one contiguous run of it and can be handed
// a pointer into the middle of the array rather than the whole thing. That is what lets a pedal
// index its own controls from 0 without knowing where it sits in the table.
inline constexpr int pedalParamFirst(int pedal)
{
    for (int i = 0; i < kPedalParamCount; ++i)
        if (kPedalParams[i].pedal == pedal)
            return i;
    return -1;
}
inline constexpr int pedalParamLen(int pedal)
{
    int n = 0;
    for (int i = 0; i < kPedalParamCount; ++i)
        if (kPedalParams[i].pedal == pedal)
            ++n;
    return n;
}
// Contiguity is the assumption every slice rests on, so it is checked rather than trusted.
inline constexpr bool pedalParamsAreGrouped()
{
    for (int p = 0; p < kPedalCount; ++p) {
        const int first = pedalParamFirst(p);
        if (first < 0)
            return false;
        for (int i = 0; i < pedalParamLen(p); ++i)
            if (kPedalParams[first + i].pedal != p)
                return false;
    }
    return true;
}
static_assert(pedalParamsAreGrouped(), "kPedalParams must stay grouped by pedal");
// And every pedal's slice must START with its footswitch, which is what lets the chain read the
// engage state without a special case and the editor draw five switches from one loop.
inline constexpr bool pedalSlicesStartWithSwitch()
{
    for (int p = 0; p < kPedalCount; ++p)
        if (kPedalParams[pedalParamFirst(p)].id != kPedalOnId[p])
            return false;
    return true;
}

// --- how a pedal's slice becomes a face -------------------------------------------------------
// The enclosure art is BLANK — no knobs, no lettering — so the editor generates each face from
// the parameter table rather than from a per-pedal layout. The rule is one line long: a Range
// control is a knob, and anything else that is not the footswitch is a small text control beside
// the LED. That is what makes adding a knob to a pedal a one-line change to kPedalParams, and it
// is why the two functions below live here, next to the table they read, rather than in
// geometry.h — where a control goes is layout, but WHICH controls exist is the parameter list.
//
// Both are constexpr and are used at compile time by geometry.h's static_asserts, which is the
// only thing standing between "a sixth knob was added" and a pedal face that silently draws four.

// The k-th knob (Range control) of a pedal, as an index into kPedalParams, or -1 if there is no
// such knob. The footswitch is skipped by starting at 1: pedalSlicesStartWithSwitch() is what
// makes that safe, and it is asserted above.
inline constexpr int pedalKnobParam(int pedal, int k)
{
    const int first = pedalParamFirst(pedal);
    int n = 0;
    for (int i = 1; i < pedalParamLen(pedal); ++i) {
        if (kPedalParams[first + i].kind != PedalParamKind::Range)
            continue;
        if (n++ == k)
            return first + i;
    }
    return -1;
}
inline constexpr int pedalKnobCount(int pedal)
{
    int n = 0;
    while (pedalKnobParam(pedal, n) >= 0)
        ++n;
    return n;
}

// Everything else in the slice: today that is the Delay's Sync division and its Ping-Pong switch,
// and nothing on any other pedal. They are drawn as small text controls either side of the LED
// because a list and a two-state switch are both things a knob reads badly.
inline constexpr int pedalMiniParam(int pedal, int k)
{
    const int first = pedalParamFirst(pedal);
    int n = 0;
    for (int i = 1; i < pedalParamLen(pedal); ++i) {
        if (kPedalParams[first + i].kind == PedalParamKind::Range)
            continue;
        if (n++ == k)
            return first + i;
    }
    return -1;
}
inline constexpr int pedalMiniCount(int pedal)
{
    int n = 0;
    while (pedalMiniParam(pedal, n) >= 0)
        ++n;
    return n;
}

// Normalized (what the host and the parameter queue carry) to plain (what the DSP wants). One
// function, so the controller's RangeParameter and this can never disagree about a range.
inline double pedalPlain(const PedalParamSpec &spec, double norm)
{
    norm = norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
    if (spec.kind == PedalParamKind::List)
        return std::floor(norm * (spec.max - spec.min) + 0.5) + spec.min;
    return spec.min + norm * (spec.max - spec.min);
}
inline double pedalNorm(const PedalParamSpec &spec, double plain)
{
    const double span = spec.max - spec.min;
    if (span <= 0.0)
        return 0.0;
    const double n = (plain - spec.min) / span;
    return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
}

// Version of the state blob written by getState and accepted by setState / setComponentState.
// Version 1 ended after the two IR paths; version 2 appends the MIDI learn table; version 3
// appends the four channel trims; version 4 appends the output section and the four capture
// sources; version 5 appends the pedalboard, length-prefixed. An older blob is still loaded - it is a project saved before the pedal, the trims or
// the loader could do anything - so this is a minimum-compatible marker rather than a gate, and
// the readers check the version before reading anything an older writer would not have written.
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
inline constexpr Steinberg::int32 kStateVersion = 6;

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
// What the PROCESSOR has actually heard, so the settings page can say so while a row is listening.
// A learn UI that cannot tell you whether your pedal is being received leaves "nothing happened"
// meaning either "the plug-in is deaf" or "the plug-in heard it and did the wrong thing", and those
// want opposite fixes. "seen" is a packed description of the last message, "seencount" is how many
// have ever arrived, and "blocks" is how many times process() has run - which is what separates a
// plug-in that hears no MIDI from one whose audio thread is not running at all.
inline constexpr const char *kMidiSeenAttr = "seen";
inline constexpr const char *kMidiSeenCountAttr = "seencount";
inline constexpr const char *kMidiBlocksAttr = "blocks";

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
// The capture filenames of every channel, in the same gain order the dials sweep, joined by '\n'
// within a channel and by '\f' between channels, carried as UTF-8 through setBinary (setString
// would need UTF-16 and a fixed buffer). Re-deriving the order in the editor would duplicate the
// workers' filename sort, which is exactly the kind of thing that drifts.
inline constexpr const char *kCapsNamesAttr = "names";

// Generated fresh for this plug-in. NOT derived from NAMp's — see the file header.
static DECLARE_UID(RationsProcessorUID, 0x8A5AA3AE, 0x663E9844, 0x382D4813, 0x83F3CAB9);
static DECLARE_UID(RationsControllerUID, 0x65D32B78, 0x44AF4B61, 0x7BA5877B, 0xDC491359);

} // namespace Rations
