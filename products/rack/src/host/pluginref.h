// The value types the host layer passes around: how a plug-in is named, how it is described in the
// catalogue, and how a block of audio is handed to it.
//
// None of these types include a VST3 or LV2 header. That is what lets pluginbackend.h stay
// format-agnostic, and what lets the whole NampHost library be linked into the plug-in later
// without dragging a second SDK into its include graph.

#pragma once

#include "format.h"

#include <cstdint>
#include <string>

namespace NAMp::host
{

//------------------------------------------------------------------------
// How a plug-in is found again after a restart.
//
// `key` is the reload identity and is format-specific:
//   VST3  "<absolute bundle path>\x1f<32 hex chars of PClassInfo::cid>"
//   LV2   the plug-in URI (globally unique and path-independent — the point of LV2)
//   VST2  the absolute file path
//
// The VST3 key deliberately carries the class UID rather than the class NAME. One .vst3 bundle can
// hold many classes, so the bundle path alone is not enough; but a class name changes when a vendor
// renames a plug-in between versions, which silently orphans every saved chain that referenced it.
// The cid does not change. The human-readable name travels in PluginDesc instead, and a load that
// cannot find the uid falls back to matching by name with a warning.
struct PluginRef {
    PluginFormat format = PluginFormat::Vst3;
    std::string key;

    bool valid() const
    {
        return !key.empty() && format >= PluginFormat::Vst3 && format < PluginFormat::Count;
    }

    bool operator==(const PluginRef &o) const
    {
        return format == o.format && key == o.key;
    }
};

//------------------------------------------------------------------------
// One row of the scanned catalogue: a PluginRef plus everything needed to draw a picker entry
// without loading the plug-in.
struct PluginDesc {
    PluginRef ref;
    std::string name;     // display name
    std::string category; // VST3 subcategories / LV2 class, free-form and only used for grouping
    std::string vendor;

    // "This was not here the last time the catalogue was shown." Set by Catalog::rescan() from a
    // persisted identity baseline, so it survives a restart: install a pedal, launch, and the
    // picker points at it instead of leaving it to be found in a list of two hundred. Not
    // persisted itself and not part of a plug-in's identity — it is a property of this scan.
    bool freshlyFound = false;
};

//------------------------------------------------------------------------
// What a node is configured for. Fixed for the life of an instantiation; a change to any field
// means tearing the node down and building a new one off the audio thread.
struct ProcessConfig {
    double sampleRate = 48000.0;
    int32_t maxBlock = 512;
    // 1 before the amp (a guitar's input is mono), 2 after it. A mono node runs its work once and
    // the second channel is an alias, which is where the parent project pays double for every mono
    // LV2 plug-in in the chain.
    int32_t channels = 1;
};

//------------------------------------------------------------------------
// One call's worth of audio. `in` and `out` may point at the SAME buffers — the chain runs in place
// wherever the backend allows it, so a backend must not assume they differ unless it reported
// prefersInPlace() == false.
// One MIDI message, exactly as it arrived, with its offset into the block.
//
// UNDECODED ON PURPOSE. There is no format-neutral way to decode this: a Control Change reaching a
// VST3 plug-in is a write onto whichever ParamID that plug-in's own IMidiMapping names, a Program
// Change is a write onto its own unit's program parameter, a note is an event in a different queue
// again — and an LV2 plug-in wants none of those, it wants the three raw bytes in an atom sequence.
// Decoding here would mean decoding once per format anyway, in a place that could not see the
// plug-in it was decoding for. So the chain carries the message and each backend opens its own
// door.
//
// `status` keeps its channel nibble. Losing it here would make a per-channel binding impossible to
// honour later, and a footswitch that answers on one channel only is an ordinary thing to want.
struct RtMidiEvent {
    int32_t frame = 0; // sample offset from the start of the block this arrived in
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

struct AudioBlock {
    float *const *in = nullptr;
    float *const *out = nullptr;
    int32_t channels = 1;
    int32_t frames = 0;

    // The messages that fall inside this block, already narrowed to it by the engine, with `frame`
    // rebased so 0 is this block's first sample. Null and 0 whenever there are none, which is the
    // overwhelmingly common case and costs a node exactly one comparison.
    const RtMidiEvent *midi = nullptr;
    int32_t midiCount = 0;
};

//------------------------------------------------------------------------
// A parameter as the generic panel needs to draw it. Fixed-size strings so a description can be
// fetched on the UI thread without allocating.
struct ParamInfo {
    char name[128] = {};
    char unit[32] = {};
    // 0 = continuous; >= 1 = that many steps (a switch, an enum, a mode selector).
    int32_t stepCount = 0;
    double defaultNormalized = 0.0;
    bool isBypass = false;
    bool isReadOnly = false;
    // A DESTINATION FOR AN INCOMING MIDI MESSAGE, NOT A SETTING.
    //
    // VST3 has no way to route MIDI to a plug-in except through a parameter: the host asks
    // IMidiMapping::getMidiControllerAssignment which ParamID a controller lands on, and writes the
    // controller's value onto that parameter. A Program Change travels the same way through a
    // different door — the host finds the unit for the incoming channel and writes onto that unit's
    // kIsProgramChange parameter. Either way the parameter is an INPUT PORT shaped like a knob: a
    // plug-in that accepts MIDI publishes a block of them — 128 in the pedals shipped beside this
    // host — and they carry whatever the last message said, not anything a user set.
    //
    // That makes them wrong to draw in a panel and wrong to count as state: a plug-in has no
    // business persisting them, and a round-trip check that treats them as settings reports every
    // such plug-in as broken. Measured before this field existed, on four different plug-ins: 127
    // "lost" parameters each, every one of them a CC destination behaving correctly.
    //
    // Format-neutral by meaning rather than by accident. LV2 routes controllers through an atom
    // port instead of through parameters, so an LV2 node never sets this and never needed to.
    bool isMidiMapped = false;
};

//------------------------------------------------------------------------
// How a plug-in's own editor can be shown, decided once at instantiation.
//
// NoEditor rather than None: <X11/Xlib.h> defines None as a macro, and this header is included on
// both sides of that include in the standalone. A scoped enumerator is no protection against the
// preprocessor.
enum class EditorKind {
    NoEditor,         // no editor: the generic Cairo panel is the only option
    Vst3PlugView,     // IPlugView embedded into a window we own
    Lv2X11Embed,      // ui:X11UI embedded into a window we own
    Lv2ShowInterface, // ui:showInterface — the plug-in owns its window, we drive idle()
};

} // namespace NAMp::host
