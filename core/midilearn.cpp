// MidiLearn implementation. See midilearn.h for why the three message types are not treated
// alike.

#include "midilearn.h"

#include <algorithm>

namespace Rations
{

namespace
{
// Field widths, and the only place they are written down. Channel is stored biased by one so
// that zero means "any" and the whole word is zero for an unlearned row.
constexpr std::uint32_t kMsgBits = 2, kChanBits = 5, kDataBits = 7;
constexpr std::uint32_t kMsgMask = (1u << kMsgBits) - 1;
constexpr std::uint32_t kChanMask = (1u << kChanBits) - 1;
constexpr std::uint32_t kDataMask = (1u << kDataBits) - 1;
constexpr std::uint32_t kChanShift = kMsgBits;
constexpr std::uint32_t kDataShift = kMsgBits + kChanBits;
} // namespace

std::uint32_t packBinding(const MidiBinding &b)
{
    if (b.msg == MidiMsg::Unlearned)
        return 0;
    const std::uint32_t msg = static_cast<std::uint32_t>(b.msg) & kMsgMask;
    const std::uint32_t chan =
        (b.channel < 0 || b.channel > 15) ? 0u : static_cast<std::uint32_t>(b.channel + 1);
    const std::uint32_t data = static_cast<std::uint32_t>(std::clamp(b.data1, 0, 127));
    return msg | (chan << kChanShift) | (data << kDataShift);
}

MidiBinding unpackBinding(std::uint32_t word)
{
    MidiBinding b;
    const std::uint32_t msg = word & kMsgMask;
    // A word from a state blob is untrusted input. Anything that is not one of the three known
    // message types reads back as unlearned rather than as a row that matches something nobody
    // can name.
    if (msg < static_cast<std::uint32_t>(MidiMsg::ControlChange) ||
        msg > static_cast<std::uint32_t>(MidiMsg::NoteOn))
        return b;
    b.msg = static_cast<MidiMsg>(msg);
    const std::uint32_t chan = (word >> kChanShift) & kChanMask;
    b.channel = (chan == 0 || chan > 16) ? kMidiAnyChannel : static_cast<int>(chan) - 1;
    b.data1 = static_cast<int>((word >> kDataShift) & kDataMask);
    return b;
}

bool bindingMatches(const MidiBinding &b, MidiMsg msg, int channel, int data1)
{
    if (b.msg != msg || b.msg == MidiMsg::Unlearned)
        return false;
    if (b.data1 != data1)
        return false;
    // A binding pinned to one channel only matches that channel. A message whose route did not
    // carry a channel matches a pinned binding too: refusing it would silently break a note
    // binding if a host ever delivered notes some other way, and there is no second interpretation
    // of "the user pressed the pedal they learned".
    if (b.channel == kMidiAnyChannel || channel == kMidiAnyChannel)
        return true;
    return b.channel == channel;
}

std::string describeBinding(const MidiBinding &b)
{
    switch (b.msg) {
        case MidiMsg::Unlearned:
            return "not learned";
        case MidiMsg::ControlChange:
            return "CC " + std::to_string(b.data1);
        case MidiMsg::ProgramChange:
            return "PC " + std::to_string(b.data1);
        case MidiMsg::NoteOn:
            break;
    }

    // Note numbering follows the SDK's own statement of it: pitch 0 .. 127 is C-2 .. G8 with
    // A3 = 440 Hz (pluginterfaces/vst/ivstevents.h, NoteOnEvent::pitch), which puts middle C
    // (60) at C3. Naming it any other way would print a note the pedal does not say it sent.
    static const char *kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};
    const int pitch = std::clamp(b.data1, 0, 127);
    std::string s = "Note ";
    s += kNames[pitch % 12];
    s += std::to_string(pitch / 12 - 2);
    if (b.channel != kMidiAnyChannel)
        s += " ch " + std::to_string(b.channel + 1);
    return s;
}

} // namespace Rations
