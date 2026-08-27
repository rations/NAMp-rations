// Rations — a four-channel Neural Amp Modeler amp head, a raw VST3 plug-in (no framework).
//
// Based on NeuralAmpModelerPlugin by Steven Atkinson (MIT licence), and directly on the author's
// own NAMp, whose crossfade engine this plug-in runs four times — once per channel. The DSP core
// (NeuralAmpModelerCore, AudioDSPTools) is reused directly. Written against the VST3 SDK only.
//
// Parameter IDs 100-108 keep NAMp's numbering for the controls the two plug-ins share, so an
// author moving between them reads the same automation lanes. IDs NAMp used for controls Rations
// does not have are RETIRED rather than recycled — see below. The class UIDs are Rations' own and
// must never be confused with NAMp's: a host that loaded one expecting the other would silently
// give the user the wrong plug-in.

#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace Rations
{

// Parameter IDs. Never change these after a release — projects embed them.
enum ParamIDs : Steinberg::Vst::ParamID {
    kBypassId = 100,
    kInputGainId = 101,          // -40 .. +40 dB, default 0
    kOutputGainId = 102,         // -40 .. +40 dB, default 0
    kNoiseGateThresholdId = 103, // -100 .. 0 dB, default -80
    kBassId = 104,               // 0 .. 10, default 5
    kMiddleId = 105,             // 0 .. 10, default 5
    kTrebleId = 106,             // 0 .. 10, default 5
    kNoiseGateOnId = 108,        // toggle, default on

    // RETIRED NAMp IDs — never reuse these numbers in this plug-in.
    //   107  Tone Stack on/off   Bass/Middle/Treble are always on here, by design.
    //   109  Output Mode         pinned to Normalized; not a user choice.
    //   110  Slim                fixed at 1.0 (full size), permanently. The captures support a
    //                            smaller variant and it would cost less CPU; this plug-in always
    //                            plays them whole, so there is nothing here to expose.
    //   111  Calibrate Input     no input calibration in this plug-in.
    //   112  Input Cal Level     likewise.
    //   113  Capture             NAMp's single bank position. Rations has four banks, so the
    //                            meaning changed; a new ID per channel is used instead of
    //                            silently redefining this one under existing automation.

    // The channel. ONE list parameter rather than four mutually-exclusive booleans: that makes
    // "exactly one channel is on" a property of the type instead of an invariant four places
    // would have to agree about (RT, editor, setState, automation), and it gives the host a
    // single clean automation lane and a single MIDI target. Values are the Channel enum below.
    kChannelId = 120,

    // Position along each channel's own bank of captures. 0 = the first (lowest-gain) capture,
    // 1 = the last; the processor maps this onto p in [0, N-1] for that bank, so the dial's
    // physical travel matches the amp's evenly-spaced gain marks. Plain 0 .. 1 rather than an
    // index, because the bank size must not change a parameter's range under a host that has
    // already written automation against it. Order matches Channel.
    kCleanGainId = 121,
    kCrunchGainId = 122,
    kOd1GainId = 123,
    kOd2GainId = 124,

    // Cabinet page. 0 = IR A only, 1 = IR B only. INERT unless both slots are filled: with one IR
    // loaded that IR runs at unity and this parameter does nothing, because a naive a*A + b*B
    // with B silent would attenuate a one-IR user at every position but one.
    kIrBlendId = 130,

    // Hidden, read-only parameters (processor -> editor via output parameter changes; never
    // automated, never persisted). The meters carry the per-block peak level mapped to 0 .. 1
    // over the meter dB range below.
    kInputMeterId = 200,
    kOutputMeterId = 201,
    // Fraction of all four banks whose entries are built and primed, 0 .. 1. Every channel
    // becomes switchable well before this reaches 1: the workers build the entry the RT thread
    // needs first, then one entry of each other channel, then fill in.
    kBankProgressId = 202,
    // Which capture is actually sounding in the ACTIVE channel, as a normalized index over that
    // channel's bank. Read by the editor to name the current capture; distinct from the channel's
    // gain parameter, which is where the dial is.
    kActiveIndexId = 203,

    // RESERVED, not yet declared: the MIDI-mapped parameter block. IDs 1000 + cc for CC 0 .. 127,
    // and 1128 for Program Change. They exist because VST3 delivers CC and Program Change ONLY as
    // parameter changes through IMidiMapping, so a footswitch needs a real parameter to land on.
    // Declared with the MIDI phase, in their own Unit, with flags 0 (not kIsHidden, which the SDK
    // documents as implying kIsReadOnly and would make them unwritable).
    kMidiCcBaseId = 1000,
    kMidiProgramChangeId = 1128,
};

// The four channels, in panel order. This is the value space of kChannelId and the index space of
// every four-element array in the processor and the editor; nothing may reorder it.
enum Channel : int {
    kChannelClean = 0,
    kChannelCrunch = 1,
    kChannelOd1 = 2,
    kChannelOd2 = 3,
    kChannelCount = 4,
};

// The bank directory name for each channel, relative to the bundle's Resources/captures. These
// are tracked content shipped inside the bundle, not a user selection: there is no capture
// browser in this plug-in.
inline constexpr const char *kChannelDirName[kChannelCount] = {"Clean", "Crunch", "OD1", "OD2"};

// The per-channel gain parameter, indexed by Channel.
inline constexpr Steinberg::Vst::ParamID kChannelGainId[kChannelCount] = {
    kCleanGainId, kCrunchGainId, kOd1GainId, kOd2GainId};

// Plain-value ranges shared by the processor (denormalization) and the controller
// (RangeParameter setup). Keep the two sides in sync via these.
namespace ranges
{
inline constexpr double kGainMin = -40.0, kGainMax = 40.0, kGainDefault = 0.0;
inline constexpr double kNgMin = -100.0, kNgMax = 0.0, kNgDefault = -80.0;
inline constexpr double kToneMin = 0.0, kToneMax = 10.0, kToneDefault = 5.0;
// Level-meter display range (dB): a linear peak is mapped to 0 .. 1 across this window before
// travelling to the editor.
inline constexpr double kMeterMinDb = -70.0, kMeterMaxDb = 0.0;
} // namespace ranges

// Message IDs for controller -> processor IR loading (IConnectionPoint).
// Attribute "path" carries a UTF-8 byte string (setBinary); empty = clear the slot.
inline constexpr const char *kMsgLoadIrA = "RationsLoadIRA";
inline constexpr const char *kMsgLoadIrB = "RationsLoadIRB";
inline constexpr const char *kMsgPathAttr = "path";

// The editor asking the processor to re-send its capability message. The four banks are scanned,
// parsed and built on worker threads, so at the moment a load is acknowledged the capture names do
// not exist yet and the first caps message necessarily reports zero. The reply that carries the
// real counts is the one to this request, made once the workers have caught up.
inline constexpr const char *kMsgRequestCaps = "RationsRequestCaps";

// Capabilities travel processor -> controller after every load or clear, so the editor can name
// the capture each dial is sitting on and can disable what the current capture set does not
// support.
inline constexpr const char *kMsgModelCaps = "RationsModelCaps";
// Per-channel capture counts, as int attributes named by kChannelDirName.
inline constexpr const char *kCapsEntryCountAttr = "entryCount";
// The capture filenames of every channel, in the same gain order the dials sweep, joined by '\n'
// within a channel and by '\f' between channels, carried as UTF-8 through setBinary (setString
// would need UTF-16 and a fixed buffer). Re-deriving the order in the editor would duplicate the
// workers' filename sort, which is exactly the kind of thing that drifts.
inline constexpr const char *kCapsNamesAttr = "names";

// Generated fresh for this plug-in. NOT derived from NAMp's — see the file header.
static DECLARE_UID(RationsProcessorUID, 0x8A5AA3AE, 0x663E9844, 0x382D4813, 0x83F3CAB9);
static DECLARE_UID(RationsControllerUID, 0x65D32B78, 0x44AF4B61, 0x7BA5877B, 0xDC491359);

} // namespace Rations
