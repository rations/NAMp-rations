// The MIDI-learn rows THIS product offers. Included by core/midilearn.h, which declares the
// MidiLearnTarget and MidiAction this table is written in; include that, never this.
//
// Nine rows: four channels, then five pedal footswitches in the order the board is wired.
//
// kChannelId is a list parameter, so a channel row's value is that channel's step - see
// normFromChannel in rationsids.h, which this must agree with. Written out rather than computed so
// the table reads as a table; the static_asserts are what keep it honest about the half of it that
// is computed elsewhere - the one below for the pedals, and midiChannelRowsAreChannels() back in
// core/midilearn.h for the channels.

#pragma once

namespace Rations
{

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

} // namespace Rations
