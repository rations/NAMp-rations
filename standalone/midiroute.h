// MidiRoute — where a MIDI message has to be delivered, worked out once on the main thread.
//
// A DAW does this translation and the standalone is the DAW here, so it has to do it too. The
// three things a footswitch can send do NOT arrive at a VST3 plug-in by the same route and only
// one of them is still MIDI by the time it lands (src/midilearn.h documents all three against the
// SDK sites they were verified at):
//
//   * Control Change  -> a PARAMETER change, on the parameter IMidiMapping names for that
//                        controller number. The MIDI channel is gone by then.
//   * Program Change  -> a PARAMETER change too, on the parameter carrying kIsProgramChange in
//                        the unit IUnitInfo::getUnitByBus names for the incoming channel. The
//                        SDK's own converter does exactly this
//                        (public.sdk/source/vst/basewrapper/basewrapper.cpp:794-820, 1203-1223).
//   * Note On / Off   -> an EVENT, in ProcessData::inputEvents, still carrying its channel.
//
// WHY THIS IS A CLASS OF ITS OWN rather than three lines inside the JACK callback: both lookups
// are IEditController calls, and the audio thread may never touch the controller. So every answer
// is resolved here, once, on the main thread before the client is activated, and what the RT
// thread gets afterwards is an array subscript.
//
// A controller that offers neither interface is not an error. The affected route is simply dead
// and the other two still work, which is also what happens in a host.

#pragma once

#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace Rations
{

//------------------------------------------------------------------------
class MidiRoute
{
public:
    static constexpr int kChannels = 16;
    static constexpr int kControllers = 128;

    // kNoParamId is 0xffffffff rather than 0, so the tables cannot be left to zero-initialise:
    // "no destination" has to be written in explicitly or every controller number would appear to
    // be mapped to parameter 0.
    MidiRoute();

    // Main thread, once, before the audio client is activated.
    void resolve(Steinberg::Vst::IEditController *controller);

    // --- RT thread: pure array reads from here down ----------------------

    // The parameter a Control Change lands on, or kNoParamId if none does.
    Steinberg::Vst::ParamID ccParam(int channel, int cc) const
    {
        if (channel < 0 || channel >= kChannels || cc < 0 || cc >= kControllers)
            return Steinberg::Vst::kNoParamId;
        return mCc[channel][cc];
    }

    // The parameter a Program Change lands on, or kNoParamId.
    Steinberg::Vst::ParamID programParam(int channel) const
    {
        if (channel < 0 || channel >= kChannels)
            return Steinberg::Vst::kNoParamId;
        return mProgram[channel].id;
    }

    // A program number as that parameter's normalized value. The denominator is the list's own
    // length, read from the plug-in rather than assumed to be 128, because it is the plug-in that
    // decides how long its program list is.
    double programValue(int channel, int program) const
    {
        if (channel < 0 || channel >= kChannels)
            return 0.0;
        const int count = mProgram[channel].count;
        if (count < 2)
            return 0.0;
        if (program < 0)
            program = 0;
        if (program > count - 1)
            program = count - 1;
        return static_cast<double>(program) / static_cast<double>(count - 1);
    }

    // For the one line the standalone prints about what it can route.
    bool hasCc() const
    {
        return mHasCc;
    }
    bool hasProgramChange() const
    {
        return mHasProgram;
    }

private:
    struct Program {
        Steinberg::Vst::ParamID id = Steinberg::Vst::kNoParamId;
        Steinberg::int32 count = 0;
    };

    Steinberg::Vst::ParamID mCc[kChannels][kControllers] = {};
    Program mProgram[kChannels] = {};
    bool mHasCc = false;
    bool mHasProgram = false;
};

} // namespace Rations
