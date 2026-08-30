// MidiLearn — the table that maps a footswitch button to something this plug-in does.
//
// A four-button footswitch is the reason the channel switch exists at all, and it has to work
// with the editor closed, so the table lives in the processor and is evaluated on the audio
// thread. This header is the shared vocabulary: the processor matches against it, the controller
// caches a copy for the editor to draw, and both ends of the state blob agree on its layout.
//
// WHAT A PEDAL CAN SEND, AND HOW MUCH OF IT VST3 HANDS BACK. Verified against the SDK rather
// than assumed, because the three message types do NOT arrive by the same route and they do not
// carry the same information:
//
//   * Control Change reaches a VST3 plug-in only as a PARAMETER CHANGE, routed by
//     IMidiMapping::getMidiControllerAssignment (pluginterfaces/vst/ivsteditcontroller.h). That
//     call returns one ParamID per controller number, so the MIDI channel a CC arrived on is not
//     recoverable: sixteen channels collapse onto one parameter. Recovering it would mean
//     declaring 16 x 128 parameters, which is not a thing to inflict on a host's parameter list
//     for a feature that switches an amp channel. So a learned CC matches on ANY channel, and
//     the editor says so.
//   * Program Change does not come through IMidiMapping at all. Controller numbers stop at
//     kCountCtrlNumber (130) and kCtrlProgramChange is 130 - the same number - because 130 and
//     up are the namespace for kLegacyMIDICCOutEvent, which is an OUTPUT event
//     (pluginterfaces/vst/ivstmidicontrollers.h:104-112). The SDK's own host-side converter
//     routes Program Change to a parameter carrying ParameterInfo::kIsProgramChange, found
//     through IUnitInfo::getUnitByBus for that MIDI channel
//     (public.sdk/source/vst/basewrapper/basewrapper.cpp:794-820, 1203-1223). That is the route
//     taken here, and it has the same consequence as CC: the parameter is per unit, not per
//     channel, so a learned Program Change also matches any channel.
//   * Note On arrives directly, as Event::kNoteOnEvent in ProcessData::inputEvents
//     (pluginterfaces/vst/ivstevents.h:161), and NoteOnEvent DOES carry its channel. So a
//     learned note is the one binding of the three that can be pinned to one MIDI channel, and
//     it is stored that way rather than being flattened to match the other two.
//
// The table is GENERIC OVER ParamID and value rather than hard-wired to the four channels, and
// the pedalboard is what that generality was for: the five footswitch rows are five rows of data
// and no new mechanism. The gate toggle is deliberately absent - it is not on the MIDI path at
// all and stays on for as long as the user has it on.
//
// WHAT A ROW PERFORMS, AND WHY THE TWO HALVES OF THE TABLE DIFFER. A channel row SETS: the four
// of them are four positions of one switch, and "go to OD2" is the whole of what a player means
// by stamping on that button. A pedal row TOGGLES, and it has to, because a footswitch that could
// only ever turn a pedal ON would need a second button to turn it off - five pedals would eat ten
// of the four buttons a footswitch has.
//
// Toggling on the press is not free of a compromise, and the compromise is in the pedal rather
// than in this file, so it is written down here rather than discovered. A footswitch controller
// sends one of two things for one physical press:
//
//   * MOMENTARY - 127 down, 0 up (or nothing on the way up). The rising edge is one press, so a
//     toggle is exactly right and this is the common case.
//   * LATCHING - 127, then 0, then 127, alternating with each press. Only every other press is a
//     rising edge, so a toggle changes the pedal on every SECOND press.
//
// No single rule serves both, because the two send contradictory messages for the same gesture.
// Following the value instead ("64 and over is on") would serve the latching pedal perfectly and
// make the momentary one useless - the pedal would be on only while a foot was held down. So the
// press is what acts, one rule for the whole table and the same edge the channel rows already
// use, and a latching controller costs its owner a second stamp rather than anything worse.

#pragma once

#include "engineconfig.h"
#include "rationsids.h"

#include "pluginterfaces/vst/vsttypes.h"

#include <cstdint>
#include <string>

namespace Rations
{

// What kind of MIDI message a row is listening for. Values are persisted in the state blob, so
// they are fixed once written: append, never renumber.
// Unlearned, not None: <X11/Xlib.h> is in this editor's include graph and defines None as a
// macro, so a member by that name does not survive the preprocessor on the platform this is
// built on.
enum class MidiMsg : std::uint32_t {
    Unlearned = 0, // the row is not learned
    ControlChange = 1,
    ProgramChange = 2,
    NoteOn = 3,
};

// Channel 0 .. 15, or this. CC and Program Change are always kAnyChannel for the reasons in the
// file header; a note may be either.
inline constexpr int kMidiAnyChannel = -1;

// One learned binding. Small and trivially copyable on purpose: it is packed into a single
// atomic word so the audio thread can read a row without a lock and without ever seeing half of
// an edit.
struct MidiBinding {
    MidiMsg msg = MidiMsg::Unlearned;
    int channel = kMidiAnyChannel; // 0 .. 15, or kMidiAnyChannel
    int data1 = 0;                 // controller number, program number, or note number

    bool learned() const
    {
        return msg != MidiMsg::Unlearned;
    }
    bool operator==(const MidiBinding &o) const
    {
        return msg == o.msg && channel == o.channel && data1 == o.data1;
    }
};

// Pack a binding into one 32-bit word, and back. Two bits of type, five of channel (0 = any,
// 1 .. 16 = channel + 1) and seven of data, so the whole thing is 14 bits and an atomic<uint32>
// is lock-free on every platform this builds for. unpack() clamps rather than trusting its
// input, because the same words come back out of an untrusted state blob.
std::uint32_t packBinding(const MidiBinding &b);
MidiBinding unpackBinding(std::uint32_t word);

// What a row does with its parameter. See the file header for why a channel sets and a pedal
// toggles, and for what a latching footswitch costs.
enum class MidiAction {
    Set = 0,    // store the row's value
    Toggle = 1, // flip between 0 and 1 - only legal on a parameter whose step count is 1
};

// What a row performs when its binding matches: a parameter, an action, and the value the action
// uses. Fixed at compile time, so the audio thread never has to publish a target, only a binding.
struct MidiLearnTarget {
    const char *label;             // what the settings page calls this row
    Steinberg::Vst::ParamID param; // what it performs
    MidiAction action;             // ... and how
    double value;                  // what Set stores, normalized. Toggle does not read it.
};

// Nine rows: four channels, then five pedal footswitches in the order the board is wired.
//
// kChannelId is a list parameter, so a channel row's value is that channel's step - see
// normFromChannel in rationsids.h, which this must agree with. Written out rather than computed so
// the table reads as a table; the static_asserts below are what keep it honest about the half of
// it that is computed elsewhere.
inline constexpr int kMidiLearnChannelRows = kChannelCount;
inline constexpr int kMidiLearnRowCount = kChannelCount + kPedalCount;
inline constexpr MidiLearnTarget kMidiLearnRows[kMidiLearnRowCount] = {
    {"Clean", kChannelId, MidiAction::Set, 0.0},
    {"Crunch", kChannelId, MidiAction::Set, 1.0 / 3.0},
    {"OD1", kChannelId, MidiAction::Set, 2.0 / 3.0},
    {"OD2", kChannelId, MidiAction::Set, 1.0},
    {"Boost", kBoostOnId, MidiAction::Toggle, 0.0},
    {"Chorus", kChorusOnId, MidiAction::Toggle, 0.0},
    {"Flanger", kFlangerOnId, MidiAction::Toggle, 0.0},
    {"Delay", kDelayOnId, MidiAction::Toggle, 0.0},
    {"Reverb", kReverbOnId, MidiAction::Toggle, 0.0},
};

// The pedal half of that table is written out by hand and derived in kPedalParams, so it is
// checked rather than trusted: a pedal reordered there, or a sixth one added, is a compile error
// here instead of a footswitch that turns on somebody else's pedal.
constexpr bool midiPedalRowsMatchPedals()
{
    for (int p = 0; p < kPedalCount; ++p) {
        const MidiLearnTarget &row = kMidiLearnRows[kMidiLearnChannelRows + p];
        if (row.param != kPedalOnId[p] || row.action != MidiAction::Toggle)
            return false;
        // Toggle flips between 0 and 1, which is only a value that parameter can take if its step
        // count is 1. Every pedal's first entry is its footswitch, and that is asserted in
        // rationsids.h; this is the other half of the claim - that it is a Toggle.
        if (kPedalParams[pedalParamFirst(p)].kind != PedalParamKind::Toggle)
            return false;
    }
    return true;
}
static_assert(midiPedalRowsMatchPedals(),
              "the pedal rows must stay in kPedalParams' order and stay toggles");

// A channel row does not toggle, and its value has to be a step kChannelId can actually take.
constexpr bool midiChannelRowsAreChannels()
{
    for (int c = 0; c < kMidiLearnChannelRows; ++c)
        if (kMidiLearnRows[c].param != kChannelId ||
            kMidiLearnRows[c].action != MidiAction::Set ||
            kMidiLearnRows[c].value != normFromChannel(static_cast<Channel>(c)))
            return false;
    return true;
}
static_assert(midiChannelRowsAreChannels(), "a channel row must set kChannelId to its own step");

// How many rows a state blob written before the pedalboard holds. FROZEN: it is a fact about
// versions 2 to 5 of that format, not about this build's table, so it stays 4 whatever
// kMidiLearnRowCount becomes. From version 6 the block carries its own count and this is not
// consulted - see kStateVersion.
inline constexpr int kMidiLearnRowsV2 = 4;
static_assert(kMidiLearnRowsV2 <= kMidiLearnRowCount,
              "an old blob's rows must all still have somewhere to land");

// Does an incoming message match this binding? `channel` is the channel the message arrived on,
// or kMidiAnyChannel when the route did not carry one (CC and Program Change - see the header).
bool bindingMatches(const MidiBinding &b, MidiMsg msg, int channel, int data1);

// One line for the settings page: "CC 64", "PC 3", "Note C3 ch 2", or "not learned". Never
// allocates beyond the returned string, and never runs on the audio thread.
std::string describeBinding(const MidiBinding &b);

} // namespace Rations
