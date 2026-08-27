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
// The table is GENERIC OVER ParamID and value rather than hard-wired to the four channels: a
// pedal on/off row later is a row of data, not a rework. This build fills in four rows, one per
// channel. The gate toggle is deliberately absent - it is not on the MIDI path at all and stays
// on for as long as the user has it on.

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

// What a row performs when its binding matches: a parameter and the value to set it to. Fixed at
// compile time in this build - the four rows are the four channels - so the audio thread never
// has to publish a target, only a binding.
struct MidiLearnTarget {
    const char *label;             // what the settings page calls this row
    Steinberg::Vst::ParamID param; // what it performs
    double value;                  // ... and to what, normalized
};

// Four rows: one per channel. kChannelId is a list parameter, so the value is that channel's
// step - see normFromChannel in rationsprocessor.h, which this must agree with. Written out
// rather than computed so the table reads as a table.
inline constexpr int kMidiLearnRowCount = kChannelCount;
inline constexpr MidiLearnTarget kMidiLearnRows[kMidiLearnRowCount] = {
    {"Clean", kChannelId, 0.0},
    {"Crunch", kChannelId, 1.0 / 3.0},
    {"OD1", kChannelId, 2.0 / 3.0},
    {"OD2", kChannelId, 1.0},
};

// Does an incoming message match this binding? `channel` is the channel the message arrived on,
// or kMidiAnyChannel when the route did not carry one (CC and Program Change - see the header).
bool bindingMatches(const MidiBinding &b, MidiMsg msg, int channel, int data1);

// One line for the settings page: "CC 64", "PC 3", "Note C3 ch 2", or "not learned". Never
// allocates beyond the returned string, and never runs on the audio thread.
std::string describeBinding(const MidiBinding &b);

} // namespace Rations
