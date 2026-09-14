// RackPreset — a chain written to a file, and read back into one.
//
// A pedalboard whose arrangement dies with the process is a demo, not an instrument. This is the
// persistence layer for the rack: capture the live chain into a value type, write it as text, parse
// it back, and rebuild the chain from it.
//
// THE FILE IS TEXT, AND THE FORMAT IS A LINE PER FIELD:
//
//     #NAMPRACK 1
//     node PRE
//     format VST3
//     key /usr/lib/vst3/Thing.vst3<US>7f3a...c1
//     name Analog Rack Delay
//     enabled 1
//     mix 1.000000
//     param 0 0.500000
//     state <base64>
//     end
//
// `node` opens a record and `end` closes it; a field before any `node` or after any `end` is
// ignored, as is a key this build does not recognise. That last part is the forward-compatibility
// rule: a rack written by a later build loses the fields this build cannot use and keeps everything
// else, rather than being rejected whole.
//
// FORMAT IS A TAG, NOT AN INTEGER (see format.h). A node naming a format this build has never heard
// of becomes a placeholder that is written back out verbatim, so booting an older build cannot
// silently delete the VST2 half of someone's rack.
//
// STATE IS AUTHORITATIVE; THE PARAMETER LIST IS THE FALLBACK AND THE CHECK. A node's `state` blob
// is applied first and is allowed to win. The `param` lines are then compared against what the
// plug-in actually reports, and only the ones that disagree beyond the plug-in's own quantisation
// grid are written back.
//
// The first draft trusted stateLoad()'s return value and stopped there. That is not enough: a
// plug-in can accept a blob, return success and restore nothing. `namp_hostcheck --state` measures
// exactly that, in memory, with no file and no restart, and two of the Windows plug-ins bridged on
// this machine fail it outright — every parameter back to its default. The knob positions are in
// the file regardless, so a preset that has them and does not use them is throwing away a user's
// rack for a plug-in bug it can see.
//
// The comparison is what makes this safe to do unconditionally. A state restore that worked agrees
// with the parameter list to within a rounding and nothing is written; one that did not disagrees
// on everything and is repaired, with one warning naming the plug-in.
//
// EVERY INPUT HERE IS UNTRUSTED. A preset comes off disk, may have been
// edited by hand or by another program, and names paths that are about to be dlopen'd. Sizes,
// counts, string lengths and the base64 alphabet are all bounds-checked before use, a node that
// fails to load is skipped with one warning, and a missing plug-in becomes a placeholder rather
// than a deletion.

#pragma once

#include "chainbuilder.h"
#include "pluginref.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NAMp::host
{

//------------------------------------------------------------------------
// Bumped only for a change a previous build could not read correctly. An unknown version is
// refused: unlike the scan cache, a rack is not regenerable, so guessing at it is worse than
// saying so.
constexpr int32_t kRackPresetVersion = 1;

//--- caps, all applied to untrusted input --------------------------------
constexpr size_t kMaxPresetBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxPresetLine = 8u * 1024u * 1024u; // one `state` line dominates this
constexpr size_t kMaxPresetStateBytes = 6u * 1024u * 1024u;
constexpr size_t kMaxPresetParams = 8192;
constexpr size_t kMaxPresetNameLength = 512;

// How far a plug-in may legitimately be from the value it was asked for. A VST3 plug-in is entitled
// to quantise a parameter to its own grid — measured here: 3.2e-4 for one plug-in, 4.2e-3 for one
// on a 0.01 grid — and the host must not argue with it about a rounding. Anything past this is not
// a grid, it is a value that was not restored.
constexpr double kParamGridTolerance = 0.01;

//------------------------------------------------------------------------
struct PresetNode {
    ChainSection section = ChainSection::Pre;
    // As written in the file. Kept verbatim even when this build cannot resolve it, so a rack
    // round-trips through a build that does not support the format.
    std::string formatTag;
    bool formatKnown = false;
    PluginRef ref; // ref.format is meaningful only when formatKnown
    std::string name;
    bool enabled = true;
    float mix = 1.0f;
    std::vector<double> params; // fallback only; see the header comment
    std::vector<uint8_t> state;
};

//------------------------------------------------------------------------
struct RackPreset {
    std::vector<PresetNode> nodes; // pre-section nodes first, each in chain order
};

//------------------------------------------------------------------------
// What applying a preset actually did. Reported rather than returned as a bool because "six of
// seven pedals loaded" is the interesting outcome and is neither success nor failure.
struct ApplyReport {
    int loaded = 0;
    int placeholders = 0; // named a plug-in that is not installed, or would not load
    int skipped = 0;      // malformed beyond the point of making a placeholder out of it
};

//========================================================================
// Text
//------------------------------------------------------------------------
std::string writeRackPreset(const RackPreset &preset);

// Parses `len` bytes. Returns false only when the file is not a rack preset at all (bad or missing
// header, or over the size cap); anything else recoverable is dropped and described in `warnings`.
bool parseRackPreset(const char *text, size_t len, RackPreset &out, std::string &warnings);

//========================================================================
// The live chain
//------------------------------------------------------------------------
// Snapshot the chain as it stands. `stateDir` is where a plug-in may copy external files its state
// refers to — an impulse response, a sample — and may be empty, in which case such a plug-in stores
// an absolute path and the preset is only portable within this machine.
bool captureRack(const ChainBuilder &builder, const std::string &stateDir, RackPreset &out);

// Replace the chain with this preset. Safe to call with the audio thread running: existing nodes
// are buried rather than destroyed, every new instance has its state applied BEFORE it is adopted,
// and the whole thing is published once at the end.
bool applyRack(ChainBuilder &builder, const RackPreset &preset, const std::string &stateDir,
               ApplyReport &report);

//========================================================================
// Files
//------------------------------------------------------------------------
// $XDG_CONFIG_HOME/NAMp-Rack/racks, or $HOME/.config/NAMp-Rack/racks, and on Windows
// %LOCALAPPDATA%\NAMp-Rack\racks. Empty when nothing resolves.
std::string rackDir();

// True for a name that is safe to turn into a filename: non-empty, at most 64 characters, no path
// separator, no leading dot, and nothing outside [A-Za-z0-9 ._-]. A preset name arrives from a
// command line or a saved file and is concatenated into a path, so it is validated rather than
// sanitised — quietly rewriting a name means saving to a file the user did not ask for.
bool rackNameIsSafe(const std::string &name);

// <rackDir()>/<name>.namprack, or empty if either half is unusable.
std::string rackPath(const std::string &name);

// The names of the presets that exist, sorted. Never throws; an unreadable directory is empty.
std::vector<std::string> listRacks();

// Where a preset keeps the external files its nodes' states refer to: "<path>.d". Created on
// demand by saveRack().
std::string rackStateDir(const std::string &presetPath);

bool saveRack(const ChainBuilder &builder, const std::string &path, std::string &error);
bool loadRack(ChainBuilder &builder, const std::string &path, ApplyReport &report,
              std::string &error);

//========================================================================
// base64, exposed for the round-trip test
//------------------------------------------------------------------------
std::string base64Encode(const uint8_t *data, size_t len);
// Rejects any character outside the alphabet, bad padding, and anything that would decode to more
// than `maxOut` bytes. Whitespace is not tolerated: every encoder here writes one unbroken line.
bool base64Decode(const std::string &text, std::vector<uint8_t> &out, size_t maxOut);

} // namespace NAMp::host
