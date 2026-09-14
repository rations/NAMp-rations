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
// what that generality buys differs by product, which is why the rows themselves are not in this
// file. products/rations spends it on the pedalboard: the five footswitch rows are five rows of
// data and no new mechanism. products/rack has no pedalboard -- its pedals are separate plug-ins
// it hosts -- so every row it holds today is a channel row, and the generality is kept there
// against the same shape arriving again: a footswitch reaching a HOSTED plug-in's bypass is a
// ParamID and a value on some other object, which is exactly what a row already is. Rebuilding
// the mechanism at that point would be rebuilding this file.
//
// The gate toggle is deliberately absent from both - it is not on the MIDI path at all and stays
// on for as long as the user has it on.
//
// WHAT A ROW PERFORMS, AND WHY THE TWO ACTIONS DIFFER. A channel row SETS: the four of them are
// four positions of one switch, and "go to OD2" is the whole of what a player means by stamping on
// that button. A row that TOGGLES has to, because a footswitch that could only ever turn a pedal
// ON would need a second button to turn it off - five pedals would eat ten of the four buttons a
// footswitch has. Every toggling row today is one of products/rations' pedals; products/rack keeps
// MidiAction::Toggle with no row using it, because it is one line to keep and a mechanism to
// re-derive.
//
// WHAT COUNTS AS A PRESS depends on the controller, and there are three kinds. This mattered more
// than it looks, because the rule here was originally written for one of them and quietly broke
// the other:
//
//   * PROGRAMMED - each slot sends one fixed number on every press and nothing on release
//     (CC 4 value 127, say, or Program Change 4). This is what a programmable MIDI footswitch
//     normally is, and the identical message arrives every time.
//   * MOMENTARY - 127 when the foot goes down, 0 when it comes up. A spring-return switch.
//   * ALTERNATING - 127, then 0, then 127, one message per press, the value tracking a latch
//     inside the controller.
//
// A press is therefore any value at or above 64 - the MIDI switch threshold - and a value below it
// is a release and does nothing. The rule used to require a RISING edge, at or above 64 having
// previously been below, which serves a momentary switch exactly and makes a PROGRAMMED one work
// once and then go dead: its second press is not an edge. That was invisible on a channel row,
// because selecting Clean twice is selecting Clean, and it would have been fatal on a row that
// toggles.
// Measured against the built bundle rather than reasoned about: three presses of one value gave
// on, nothing, nothing.
//
// What the edge test was really protecting is done by the BLOCK instead, in the processor: the
// thing that must not fire repeatedly is a host writing the same value into the parameter every
// block, and that is exactly a repeat in the immediately following block. A foot cannot arrive
// twice inside one 2.67 ms period, so no real press is suppressed and no clock is consulted.
//
// ALTERNATING is the one kind not fully served: its releases are indistinguishable from a
// momentary switch's, so it takes two stamps per change. Serving it instead would mean following
// the value, which would make a momentary switch useless - on only while a foot was held down -
// so it is a mode to avoid programming rather than a case to guess at.

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

// What a row does with its parameter. See the file header for why a channel row sets and a
// footswitch row toggles, and for what a latching footswitch costs.
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

// THE ROWS THEMSELVES ARE THE PRODUCT'S, and arrive from its own midilearnrows.h. That file
// supplies kMidiLearnChannelRows, kMidiLearnRowCount, kMidiLearnRows[] and whatever assertions are
// particular to its table; everything above this line and everything below it is the mechanism and
// is the same in both products.
//
// It is reached by name and resolved by the include path, which puts the product's src/ ahead of
// core/ -- the same rule that decides which geometry.h or rationsids.h this file just included.
// Include midilearn.h, never midilearnrows.h directly: the rows are written in terms of
// MidiLearnTarget and MidiAction, which are declared above.
//
// The namespace is closed around the include and reopened after it, so that file is an ordinary
// header that opens its own namespace rather than a fragment that only compiles in one context.

} // namespace Rations

#include "midilearnrows.h"

namespace Rations
{

// A channel row does not toggle, and its value has to be a step kChannelId can actually take.
constexpr bool midiChannelRowsAreChannels()
{
    for (int c = 0; c < kMidiLearnChannelRows; ++c)
        if (kMidiLearnRows[c].param != kChannelId || kMidiLearnRows[c].action != MidiAction::Set ||
            kMidiLearnRows[c].value != normFromChannel(static_cast<Channel>(c)))
            return false;
    return true;
}
static_assert(midiChannelRowsAreChannels(), "a channel row must set kChannelId to its own step");

// How many rows a state blob written before the pedalboard holds. FROZEN: it is a fact about
// versions 2 to 5 of that format, not about any build's table, so it stays 4 whatever
// kMidiLearnRowCount becomes and whichever product is asking. From version 6 the block carries its
// own count and this is not consulted - see kStateVersion.
//
// In products/rack it happens to equal kMidiLearnRowCount, because that table is the four channel
// rows and nothing else. That is a coincidence of arithmetic and not a reason to merge them: one
// is history and the other is this build. The assertion below is what the split has to keep true.
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
