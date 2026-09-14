// The MIDI-learn rows THIS product offers. Included by core/midilearn.h, which declares the
// MidiLearnTarget and MidiAction this table is written in; include that, never this.
//
// Four rows, one per channel. The parent plug-in had nine - these four, then five pedal
// footswitches - and the five went with the pedalboard, whose pedals are separate plug-ins the
// rack hosts. products/rations still carries all nine.
//
// kChannelId is a list parameter, so a channel row's value is that channel's step - see
// normFromChannel in rationsids.h, which this must agree with. Written out rather than computed so
// the table reads as a table; midiChannelRowsAreChannels() back in core/midilearn.h is what keeps
// it honest.
//
// kMidiLearnChannelRows and kMidiLearnRowCount are the same number here and are still two names,
// because they mean different things: one is "how many of these rows are channels" and the other
// is "how long is this table". A row pointing at something else would move them apart - a
// footswitch reaching a hosted plug-in's bypass is exactly that row - and every loop that walks
// the table already says which of the two it means.

#pragma once

namespace Rations
{

inline constexpr int kMidiLearnChannelRows = kChannelCount;
inline constexpr int kMidiLearnRowCount = kChannelCount;
inline constexpr MidiLearnTarget kMidiLearnRows[kMidiLearnRowCount] = {
    {"Clean", kChannelId, MidiAction::Set, 0.0},
    {"Crunch", kChannelId, MidiAction::Set, 1.0 / 3.0},
    {"OD1", kChannelId, MidiAction::Set, 2.0 / 3.0},
    {"OD2", kChannelId, MidiAction::Set, 1.0},
};

} // namespace Rations
