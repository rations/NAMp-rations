// INampFileLoader — host-discoverable file-loading interface for NAMp.
//
// THIS IS SOMEONE ELSE'S INTERFACE AND IS REPRODUCED HERE VERBATIM. NAMp is a separate plug-in;
// this host is a CONSUMER of the interface, never an implementer of it. The amp built into this
// product does not implement it and could not: it has four banks and two impulse responses where
// this has one of each, and it is linked in rather than hosted, so its loaders travel over the
// SDK's IConnectionPoint channel instead. What the copy buys is that a user who racks an actual
// NAMp gets file loading in the generic panel rather than three unreachable paths.
//
// Because it is an interface we do not own, nothing here may be changed — not a method, not an
// argument, and above all not the iid, which is the only thing that makes the queryInterface
// succeed against a binary built from the other tree.
//
// VST3 parameters are normalized floats only, so a model (.nam), capture directory or impulse
// response (.wav) path cannot travel as a parameter. The GUI has loader boxes for this; a
// GUI-less host needs another route. This interface is implemented by NAMp's edit controller and
// discovered by hosts via queryInterface. A host that finds it can offer file pickers (or REPL
// commands); a host that does not know it is unaffected. The controller forwards the path to the
// processor over the SDK's IConnectionPoint message channel, and all three paths persist in the
// plug-in state.
//
// The iid is NAMp's own. Frozen once released — never renumber.

#pragma once

#include "pluginterfaces/base/funknown.h"

namespace NAMp
{

//------------------------------------------------------------------------
class INampFileLoader : public Steinberg::FUnknown
{
public:
    // Set a single capture file (absolute path, UTF-8). nullptr or "" clears. Loading a single
    // file puts the plug-in in single-capture mode, where the Gain knob has nothing to sweep.
    virtual Steinberg::tresult PLUGIN_API setModelFile(const Steinberg::char8 *path) = 0;

    // Set a directory of captures of the same amp at ascending gain settings (absolute path,
    // UTF-8). nullptr or "" clears. This is what the Gain knob sweeps. Files are ordered by the
    // trailing gain token in their names, with a "MAX" token sorting last.
    virtual Steinberg::tresult PLUGIN_API setBankDirectory(const Steinberg::char8 *path) = 0;

    // Set the impulse response (absolute path, UTF-8). nullptr or "" clears.
    virtual Steinberg::tresult PLUGIN_API setIrFile(const Steinberg::char8 *path) = 0;

    // Get the current paths (UTF-8, empty string when nothing is loaded).
    // Returns kResultFalse if the buffer is too small.
    virtual Steinberg::tresult PLUGIN_API getModelFile(Steinberg::char8 *buffer,
                                                       Steinberg::int32 bufferSize) = 0;
    virtual Steinberg::tresult PLUGIN_API getBankDirectory(Steinberg::char8 *buffer,
                                                           Steinberg::int32 bufferSize) = 0;
    virtual Steinberg::tresult PLUGIN_API getIrFile(Steinberg::char8 *buffer,
                                                    Steinberg::int32 bufferSize) = 0;

    static const Steinberg::FUID iid;
};

DECLARE_CLASS_IID(INampFileLoader, 0x2AB9C630, 0x42CF8A1E, 0x67650385, 0x05B68609)

} // namespace NAMp
