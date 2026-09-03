// Rations edit controller implementation.

#include "rationscontroller.h"
#include "rationsids.h"
#include "rationsview.h"

#include "platform/respath.h" // pathBaseName, for the channel-name fallback

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace Steinberg;

namespace Rations
{

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::initialize(FUnknown *context)
{
    tresult result = EditControllerEx1::initialize(context);
    if (result != kResultOk)
        return result;

    parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0,
                            Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsBypass,
                            kBypassId);

    auto *inGain =
        new Vst::RangeParameter(STR16("Input"), kInputGainId, STR16("dB"), ranges::kGainMin,
                                ranges::kGainMax, ranges::kGainDefault);
    inGain->setPrecision(1);
    parameters.addParameter(inGain);

    auto *outGain =
        new Vst::RangeParameter(STR16("Output"), kOutputGainId, STR16("dB"), ranges::kGainMin,
                                ranges::kGainMax, ranges::kGainDefault);
    outGain->setPrecision(1);
    parameters.addParameter(outGain);

    // The channel. One list parameter, so "exactly one channel is on" needs no invariant kept in
    // four places, and the host gets one automation lane and one MIDI target. The string order is
    // the Channel enum's order and must not be rearranged.
    auto *channel = new Vst::StringListParameter(STR16("Channel"), kChannelId);
    channel->appendString(STR16("Clean"));
    channel->appendString(STR16("Crunch"));
    channel->appendString(STR16("OD1"));
    channel->appendString(STR16("OD2"));
    channel->setNormalized(0.0); // default: Clean
    parameters.addParameter(channel);

    // One dial per channel, sweeping that channel's own bank of captures. Plain 0 .. 1 rather
    // than a capture index: a bank's size must not change a parameter's range under a host that
    // has already written automation against it.
    static const Vst::TChar *kGainTitles[kChannelCount] = {
        STR16("Clean Gain"), STR16("Crunch Gain"), STR16("OD1 Gain"), STR16("OD2 Gain")};
    for (int c = 0; c < kChannelCount; ++c) {
        auto *g =
            new Vst::RangeParameter(kGainTitles[c], kChannelGainId[c], nullptr, 0.0, 1.0, 0.0);
        g->setPrecision(2);
        parameters.addParameter(g);
    }

    // Per-channel output trim, on the settings page. The captures are already loudness-normalized
    // per capture, so what this corrects is the perceptual residual: a high-gain channel is far
    // more compressed than a clean one and reads louder at the same measured loudness. Narrow on
    // purpose - see the range's own note.
    static const Vst::TChar *kLevelTitles[kChannelCount] = {
        STR16("Clean Level"), STR16("Crunch Level"), STR16("OD1 Level"), STR16("OD2 Level")};
    for (int c = 0; c < kChannelCount; ++c) {
        auto *l =
            new Vst::RangeParameter(kLevelTitles[c], kChannelLevelId[c], STR16("dB"),
                                    ranges::kLevelMin, ranges::kLevelMax, ranges::kLevelDefault);
        l->setPrecision(1);
        parameters.addParameter(l);
    }

    // "Threshold" and not "Gate": the parameter is a threshold in dB and the panel silkscreens it
    // that way, so a host's automation lane says what the dial says. The ID is unchanged, so
    // automation written against an older build still lands on it — a title is not an identity.
    // The boolean below keeps its own name; the thing called GATE is that switch.
    auto *ngThresh = new Vst::RangeParameter(STR16("Threshold"), kNoiseGateThresholdId, STR16("dB"),
                                             ranges::kNgMin, ranges::kNgMax, ranges::kNgDefault);
    ngThresh->setPrecision(1);
    parameters.addParameter(ngThresh);

    auto *bass = new Vst::RangeParameter(STR16("Bass"), kBassId, nullptr, ranges::kToneMin,
                                         ranges::kToneMax, ranges::kToneDefault);
    bass->setPrecision(1);
    parameters.addParameter(bass);

    auto *middle = new Vst::RangeParameter(STR16("Middle"), kMiddleId, nullptr, ranges::kToneMin,
                                           ranges::kToneMax, ranges::kToneDefault);
    middle->setPrecision(1);
    parameters.addParameter(middle);

    auto *treble = new Vst::RangeParameter(STR16("Treble"), kTrebleId, nullptr, ranges::kToneMin,
                                           ranges::kToneMax, ranges::kToneDefault);
    treble->setPrecision(1);
    parameters.addParameter(treble);

    // The gate's own toggle, and the tone stack's. Both are deliberately off the MIDI path: they
    // stay as the user set them, and neither is something a foot reaches for mid-song. Only the
    // channel and the five pedals are learnable — see kMidiLearnRows.
    parameters.addParameter(STR16("Gate On"), nullptr, 1, 1.0, Vst::ParameterInfo::kCanAutomate,
                            kNoiseGateOnId);
    parameters.addParameter(STR16("EQ On"), nullptr, 1, 1.0, Vst::ParameterInfo::kCanAutomate,
                            kToneStackOnId);

    // Slim, behind the icon left of the settings button. Flags 0 — visible to the host, saved
    // with the project, and NOT automatable, which is the one place this differs from the sibling
    // plug-ins.
    // Applying a value here rebuilds every model in every loaded bank (ChannelRack::setSlim), so a
    // host sweeping this lane would ask for a full rebuild per automation point and the amp would
    // sit at ramped silence for the length of the sweep. Flags 0 and not kIsHidden: the SDK
    // documents kIsHidden as implying kIsReadOnly, which would make it unwritable.
    auto *slim = new Vst::RangeParameter(STR16("Slim"), kSlimId, nullptr, ranges::kSlimMin,
                                         ranges::kSlimMax, ranges::kSlimDefault, 0, 0);
    slim->setPrecision(2);
    parameters.addParameter(slim);

    // Cabinet blend: 0 = IR A, 1 = IR B. Inert while only one slot is filled, in which case that
    // IR runs at unity and the editor draws this control disabled.
    auto *blend = new Vst::RangeParameter(STR16("Cab Blend"), kIrBlendId, nullptr, 0.0, 1.0, 0.0);
    blend->setPrecision(2);
    parameters.addParameter(blend);

    // The output section, at the bottom of the settings page.
    //
    // Normalized is the default, which is the upstream plug-in's choice and this plug-in's own
    // previous hard-wired behaviour, so a project made before this parameter existed sounds the
    // same now that it does. The parent plug-in defaults to Raw instead, and that is not an
    // inconsistency to be tidied away: there a single dial sweeps one bank whose whole point is
    // that gain rises across it the way the amp's own control does, and normalizing would flatten
    // exactly that. Here the four channels are four different amps and levelling them is useful.
    auto *outputMode = new Vst::StringListParameter(STR16("Output Mode"), kOutputModeId);
    outputMode->appendString(STR16("Raw"));
    outputMode->appendString(STR16("Normalized"));
    outputMode->appendString(STR16("Calibrated"));
    outputMode->setNormalized(normFromOutputMode(kOutputNormalized));
    parameters.addParameter(outputMode);

    parameters.addParameter(STR16("Calibrate Input"), nullptr, 1, 0.0,
                            Vst::ParameterInfo::kCanAutomate, kCalibrateInputId);
    auto *calLevel =
        new Vst::RangeParameter(STR16("Input Calibration Level"), kInputCalLevelId, STR16("dBu"),
                                ranges::kCalMin, ranges::kCalMax, ranges::kCalDefault);
    calLevel->setPrecision(1);
    parameters.addParameter(calLevel);

    // Processor -> editor feedback. Hidden and read-only: never automated, never persisted,
    // invisible to generic parameter UIs.
    parameters.addParameter(STR16("Input Meter"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kInputMeterId);
    parameters.addParameter(STR16("Output Meter"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kOutputMeterId);
    parameters.addParameter(STR16("Bank Progress"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kBankProgressId);
    parameters.addParameter(STR16("Active Capture"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kActiveIndexId);
    // The channel that is SOUNDING, as opposed to kChannelId, which is the one being asked for.
    // The panel's LEDs follow this one so a lamp cannot light over a channel whose captures are
    // still being built.
    parameters.addParameter(STR16("Active Channel"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kActiveChannelId);

    // --- the units, and their ORDER is load-bearing --------------------------------------
    //
    // Both units are declared here, together, before either block's parameters, because the MIDI
    // unit has to be the FIRST one this controller adds and that is a MEASURED requirement rather
    // than a tidiness one.
    //
    // It was found by bisecting five builds against a real host after a footswitch that had worked
    // for months stopped being seen at all. The pedalboard added a second unit and added it FIRST,
    // which pushed the MIDI unit from index 0 to index 1; moving that one addUnit call back behind
    // this one - changing nothing else, not a parameter, not a flag, not an id - restored the
    // footswitch completely. Every other candidate was eliminated by its own build: the twenty-five
    // new parameters, their position in the declaration order, and the Boost's four samples of
    // reported latency were each removed on their own and each one still failed.
    //
    // What that means INSIDE the host is not verifiable from here and is not claimed. The SDK's own
    // MIDI-to-parameter converter resolves the event bus's unit by ID - it walks getUnitInfo and
    // compares against what getUnitByBus returned (public.sdk/source/vst/basewrapper/
    // basewrapper.cpp, getProgramListAndUnit) - and a host doing that is indifferent to the order.
    // A host that instead takes the first unit, or indexes by position, is not, and the measurement
    // says at least one real host is in the second group. Since nothing whatever is gained by
    // declaring the pedals first, the order that works everywhere is the order to use.
    //
    // The MIDI unit carries the program list, which is the only route a Program Change travels;
    // the Pedals unit exists so a host groups twenty-five knobs away from Bass and Treble.
    addUnit(new Vst::Unit(STR16("MIDI"), kMidiUnitId, Vst::kRootUnitId, kMidiProgramListId));
    addUnit(new Vst::Unit(STR16("Pedals"), kPedalUnitId, Vst::kRootUnitId));

    // --- the pedalboard ------------------------------------------------------------------
    //
    // Declared by walking kPedalParams rather than written out twenty-five times, because that
    // table is also what the processor stores, what the state blob is written in and what the
    // editor draws. Five hand-written blocks would be five chances for one of them to disagree
    // with the other four about a range or a default.
    //
    // kPedalUnitId is declared above rather than here - see the units block for why the order is
    // not ours to choose. A parameter may name a unit that was added earlier in the same
    // initialize(); the SDK stores the id and never dereferences it.
    for (int i = 0; i < kPedalParamCount; ++i) {
        const PedalParamSpec &spec = kPedalParams[i];
        Vst::String128 title = {};
        UString(title, USTRINGSIZE(title)).fromAscii(spec.title);
        Vst::String128 unit = {};
        if (spec.unit)
            UString(unit, USTRINGSIZE(unit)).fromAscii(spec.unit);
        const Vst::TChar *units = spec.unit ? unit : nullptr;

        switch (spec.kind) {
            case PedalParamKind::Toggle:
                // Step count 1, and the default is NORMALIZED here (addParameter's argument is),
                // which for a toggle is the same 0 or 1. Every footswitch defaults OFF: a fresh
                // instance is an amp with an empty board, and a project written before the
                // pedalboard existed has to open sounding exactly as it did.
                parameters.addParameter(title, nullptr, 1, spec.def,
                                        Vst::ParameterInfo::kCanAutomate, spec.id, kPedalUnitId);
                break;
            case PedalParamKind::List: {
                // Only the Delay's sync division. The strings are a saved value - a host stores
                // the normalized position, so appending is safe and reordering is not.
                auto *list = new Vst::StringListParameter(
                    title, spec.id, nullptr,
                    Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsList, kPedalUnitId);
                for (int k = 0; k < kDelaySyncCount; ++k) {
                    Vst::String128 entry = {};
                    UString(entry, USTRINGSIZE(entry)).fromAscii(kDelaySyncNames[k]);
                    list->appendString(entry);
                }
                list->setNormalized(spec.def / (kDelaySyncCount - 1));
                parameters.addParameter(list);
                break;
            }
            case PedalParamKind::Range: {
                auto *range = new Vst::RangeParameter(title, spec.id, units, spec.min, spec.max,
                                                      spec.def, 0,
                                                      Vst::ParameterInfo::kCanAutomate,
                                                      kPedalUnitId);
                range->setPrecision(spec.precision);
                parameters.addParameter(range);
                break;
            }
        }
    }

    // --- the MIDI block ------------------------------------------------------------------
    //
    // 129 parameters nobody will ever turn. They are here because a footswitch's messages do not
    // arrive as MIDI: Control Change arrives as a parameter change routed by IMidiMapping, and
    // Program Change as a parameter change on a kIsProgramChange parameter found through
    // IUnitInfo. Both need a real parameter to land on or they do not arrive at all. midilearn.h
    // carries the SDK sites this was verified against.
    //
    // Their Unit, and its program list, are declared in the units block above, first of all - the
    // order is a measured requirement and is explained there.
    // FLAGS 0, and this is not an oversight. kIsHidden would be the obvious choice for a
    // parameter with no UI, and the SDK documents it as implying kIsReadOnly - which would make
    // these unwritable and silently discard every CC the host routed to them. Flags 0 is the
    // SDK's own pattern for MIDI-mapped parameters.
    for (int cc = 0; cc < kMidiCcCount; ++cc) {
        char ascii[32];
        snprintf(ascii, sizeof(ascii), "MIDI CC %d", cc);
        Vst::String128 title = {};
        UString(title, USTRINGSIZE(title)).fromAscii(ascii);
        parameters.addParameter(title, nullptr, 0, 0.0, 0,
                                static_cast<Vst::ParamID>(kMidiCcBaseId + cc), kMidiUnitId);
    }

    // Program Change. A program LIST rather than a plain parameter, because that is the only
    // route a Program Change actually travels: the host looks up the unit for the incoming MIDI
    // channel, finds that unit's program list, and writes the program number onto the list's
    // kIsProgramChange parameter. ProgramList builds that parameter itself, with the list id as
    // the ParamID, which is why kMidiProgramListId and kMidiProgramChangeId are the same number.
    //
    // The programs are named for what they are - the message, not a preset. This plug-in has no
    // presets and the list is not pretending to be one; it is the doorway a PC number comes
    // through, and the learn table decides what any given number does.
    auto *programs = new Vst::ProgramList(STR16("Program Change"), kMidiProgramListId, kMidiUnitId);
    for (int pc = 0; pc < kMidiProgramCount; ++pc) {
        char ascii[32];
        snprintf(ascii, sizeof(ascii), "PC %d", pc);
        Vst::String128 name = {};
        UString(name, USTRINGSIZE(name)).fromAscii(ascii);
        programs->addProgram(name);
    }
    addProgramList(programs);
    if (Vst::Parameter *pcParam = programs->getParameter())
        parameters.addParameter(pcParam);

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::queryInterface(const char *iid, void **obj)
{
    QUERY_INTERFACE(iid, obj, Vst::IMidiMapping::iid, Vst::IMidiMapping)
    return EditControllerEx1::queryInterface(iid, obj);
}

//------------------------------------------------------------------------
// Controller numbers 0 .. 127 only. 128 and 129 are aftertouch and pitch bend, which are not
// switches and have nothing to learn; 130 and up are not controller numbers at all - the SDK's
// kCountCtrlNumber is 130, the same value as kCtrlProgramChange, because everything from there up
// belongs to the legacy MIDI-CC-OUT namespace. Answering for those would be answering a question
// the host is not asking, and the SDK's own validator says so.
tresult PLUGIN_API RationsController::getMidiControllerAssignment(int32 busIndex, int16 /*channel*/,
                                                                  Vst::CtrlNumber ccNumber,
                                                                  Vst::ParamID &id)
{
    if (busIndex != 0 || ccNumber < 0 || ccNumber >= kMidiCcCount)
        return kResultFalse;
    id = static_cast<Vst::ParamID>(kMidiCcBaseId + ccNumber);
    return kResultTrue;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::getUnitByBus(Vst::MediaType type, Vst::BusDirection dir,
                                                   int32 busIndex, int32 channel,
                                                   Vst::UnitID &unitId)
{
    // Every channel of the one event input, onto the one unit that has a program list. Rations
    // has no per-channel behaviour to express here: a Program Change means the same thing
    // whichever channel the pedal sends it on, which is the same limitation CC has and for the
    // same reason (midilearn.h).
    if (type == Vst::kEvent && dir == Vst::kInput && busIndex == 0 && channel >= 0 &&
        channel < 16) {
        unitId = kMidiUnitId;
        return kResultTrue;
    }
    return kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::setComponentState(IBStream *state)
{
    // Mirror of RationsProcessor::getState — keep the two in sync.
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    int32 version = 0;
    if (!streamer.readInt32(version) || version < 1 || version > kStateVersion)
        return kResultFalse;

    static const Vst::ParamID kOrder[] = {
        kBypassId, kInputGainId, kOutputGainId, kNoiseGateThresholdId,
        kBassId,   kMiddleId,    kTrebleId,     kNoiseGateOnId};
    for (Vst::ParamID id : kOrder) {
        double v = 0.0;
        if (!streamer.readDouble(v))
            return kResultFalse;
        setParamNormalized(id, v);
    }

    double channel = 0.0;
    if (!streamer.readDouble(channel))
        return kResultFalse;
    setParamNormalized(kChannelId, channel);

    for (int c = 0; c < kChannelCount; ++c) {
        double g = 0.0;
        if (!streamer.readDouble(g))
            return kResultFalse;
        setParamNormalized(kChannelGainId[c], g);
    }

    double blend = 0.0;
    if (!streamer.readDouble(blend))
        return kResultFalse;
    setParamNormalized(kIrBlendId, blend);

    // The two IR paths follow. They are not parameters, but they are still read here, because the
    // editor has no other way to learn them: getIrFile() answers out of this controller's own copy
    // and nothing asks the processor. Skip this and reopening a project leaves both cabinet rows
    // reading "no IR loaded" while the IRs are audibly playing — and, worse since the second slot
    // became real, leaves the blend dial drawn disabled over a blend that is actually running.
    for (int slot = 0; slot < kIrSlotCount; ++slot) {
        mIrPath[slot].clear();
        if (char8 *p = streamer.readStr8()) {
            mIrPath[slot] = p;
            delete[] p;
        }
    }

    // The MIDI learn table, added in state version 2. A version 1 blob simply has nothing here,
    // which is not a failure: it is a project saved before the pedal could do anything, and it
    // opens with an unlearned table.
    for (int row = 0; row < kMidiLearnRowCount; ++row)
        mMidiTable[row] = MidiBinding();

    // From version 6 the block carries its own row count, because the pedalboard's five footswitch
    // rows made this build's table nine and an older blob's is four. This side has to walk the blob
    // in exactly the same steps the processor's setState does or everything after it is read at the
    // wrong offset, so the two are deliberately the same shape - see kStateVersion.
    int32 midiRows = (version >= 2) ? kMidiLearnRowsV2 : 0;
    if (version >= 6) {
        if (!streamer.readInt32(midiRows))
            return kResultFalse;
        if (midiRows < 0 || midiRows > kMidiRowStateMax)
            return kResultFalse;
    }
    for (int32 row = 0; row < midiRows; ++row) {
        int32 word = 0;
        if (!streamer.readInt32(word))
            return kResultFalse;
        if (row < kMidiLearnRowCount)
            mMidiTable[row] = unpackBinding(static_cast<std::uint32_t>(word));
    }

    // The per-channel trims, added in state version 3. A version 1 or 2 project has nothing here
    // and opens with every trim at its default, which is 0 dB.
    for (int c = 0; c < kChannelCount; ++c) {
        double level = 0.5;
        if (version >= 3 && !streamer.readDouble(level))
            return kResultFalse;
        setParamNormalized(kChannelLevelId[c], level);
    }

    // The output section, added in state version 4. An older project opens at Normalized with no
    // calibration, because that is what every build before version 4 was hard-wired to and so is
    // what that project actually sounded like.
    double outputMode = normFromOutputMode(kOutputNormalized);
    double calibrate = 0.0;
    double calLevel = 0.6; // +12 dBu
    if (version >= 4) {
        if (!streamer.readDouble(outputMode) || !streamer.readDouble(calibrate) ||
            !streamer.readDouble(calLevel))
            return kResultFalse;
    }
    setParamNormalized(kOutputModeId, normFromOutputMode(outputModeFromNorm(outputMode)));
    setParamNormalized(kCalibrateInputId, calibrate > 0.5 ? 1.0 : 0.0);
    setParamNormalized(kInputCalLevelId, std::clamp(calLevel, 0.0, 1.0));

    // The four capture sources and names. Read here for exactly the reason the IR paths above are:
    // they are not parameters, and the editor has no other way to learn them. Skip this and a
    // reopened project draws four empty capture rows and four default channel names over captures
    // that are audibly playing.
    //
    // Nothing is SENT from here. The processor read the same blob and has already issued its own
    // loads; sending again would rebuild all four banks a second time for nothing.
    for (int c = 0; c < kChannelCount; ++c) {
        mCapturePath[c].clear();
        mChannelNameOverride[c].clear();
        mCaptureIsDir[c] = false;
        if (version < 4)
            continue;
        int32 isDir = 0;
        if (!streamer.readInt32(isDir))
            return kResultFalse;
        if (char8 *p = streamer.readStr8()) {
            mCapturePath[c] = p;
            delete[] p;
        }
        if (char8 *p = streamer.readStr8()) {
            mChannelNameOverride[c] = p;
            delete[] p;
        }
        mCaptureIsDir[c] = !mCapturePath[c].empty() && isDir != 0;
    }

    // Version 5 onwards: the pedalboard, length-prefixed. Mirror of the processor's own read -
    // same order, same bounds, same treatment of a count this build does not recognise. A version
    // 1-4 project leaves every pedal at the default the parameter was declared with, which is off.
    if (version >= 5) {
        int32 count = 0;
        if (!streamer.readInt32(count))
            return kResultFalse;
        if (count < 0 || count > kPedalStateMax)
            return kResultFalse;
        for (int32 i = 0; i < count; ++i) {
            double value = 0.0;
            if (!streamer.readDouble(value))
                return kResultFalse;
            if (i < kPedalParamCount)
                setParamNormalized(kPedalParams[i].id, std::clamp(value, 0.0, 1.0));
        }
    }

    // Version 7 onwards: the EQ switch, at the end of the blob. A version 1-6 project opens with
    // it on, which is what those builds were hard-wired to.
    double toneStackOn = 1.0;
    if (version >= 7 && !streamer.readDouble(toneStackOn))
        return kResultFalse;
    setParamNormalized(kToneStackOnId, toneStackOn > 0.5 ? 1.0 : 0.0);

    // Version 8 onwards: Slim, after it. A version 1-7 project opens at 1.0, the whole model.
    //
    // The base class's setter, deliberately: our own override tells the view, which is wanted, but
    // what must NOT happen here is applySlim() — the processor has just read the same value out of
    // the same blob and any rebuild it needs is the load's, not a second one bounced back at it.
    double slim = ranges::kSlimDefault;
    if (version >= 8 && !streamer.readDouble(slim))
        return kResultFalse;
    setParamNormalized(kSlimId, std::clamp(slim, 0.0, 1.0));

    refreshParamTitles();
    if (mView)
        mView->FilesChanged();
    return kResultOk;
}

//------------------------------------------------------------------------
// The processor reporting what the four banks hold. The message comes from our own processor, but
// it is still parsed defensively: a truncated or absent blob must leave empty lists rather than an
// out-of-range read, because the editor indexes into them.
tresult PLUGIN_API RationsController::notify(Vst::IMessage *message)
{
    const char *id = message ? message->getMessageID() : nullptr;
    if (id && strcmp(id, kMsgMidiTable) == 0)
        return receiveMidiTable(message);
    if (!id || strcmp(id, kMsgModelCaps) != 0)
        return EditControllerEx1::notify(message);

    Vst::IAttributeList *attrs = message->getAttributes();
    if (!attrs)
        return kResultFalse;

    for (int c = 0; c < kChannelCount; ++c) {
        // Mirror of the sender's own helper: the attribute names are a wire format, and spelling
        // the concatenation out five times is five chances for a channel to go quietly missing.
        auto channelAttr = [&](const char *prefix, int64 fallback) -> int64 {
            std::string attr(prefix);
            attr += kChannelDefaultName[c];
            int64 value = 0;
            if (attrs->getInt(attr.c_str(), value) != kResultOk)
                return fallback;
            return value;
        };
        const int64 count = channelAttr(kCapsEntryCountAttr, 0);
        mEntryCount[c] = count > 0 ? static_cast<int>(count) : 0;
        mBankIsDir[c] = channelAttr(kCapsIsDirAttr, 0) != 0;
        mBankLevels[c].hasLoudness = channelAttr(kCapsHasLoudnessAttr, 0) != 0;
        mBankLevels[c].hasInputLevel = channelAttr(kCapsHasInLevelAttr, 0) != 0;
        mBankLevels[c].hasOutputLevel = channelAttr(kCapsHasOutLevelAttr, 0) != 0;
        mBankLevels[c].slimmable = channelAttr(kCapsSlimmableAttr, 0) != 0;
        mCaptureNames[c].clear();
    }

    // Names arrive as UTF-8, '\n' between captures and '\f' between channels — one blob rather
    // than four, because setBinary is per-attribute and four attributes would be four chances for
    // the channels to disagree about how many there are.
    const void *data = nullptr;
    uint32 size = 0;
    if (attrs->getBinary(kCapsNamesAttr, data, size) == kResultOk && data && size > 0) {
        const char *begin = static_cast<const char *>(data);
        const char *end = begin + size;
        int channel = 0;
        const char *p = begin;
        while (p < end && channel < kChannelCount) {
            const char *ff =
                static_cast<const char *>(memchr(p, '\f', static_cast<size_t>(end - p)));
            const char *stop = ff ? ff : end;
            for (const char *q = p; q < stop;) {
                const char *nl =
                    static_cast<const char *>(memchr(q, '\n', static_cast<size_t>(stop - q)));
                const char *lineEnd = nl ? nl : stop;
                if (lineEnd > q)
                    mCaptureNames[channel].emplace_back(q, static_cast<size_t>(lineEnd - q));
                q = nl ? nl + 1 : stop;
            }
            ++channel;
            p = ff ? ff + 1 : end;
        }
    }

    refreshParamTitles();
    if (mView)
        mView->ModelCapsChanged(mEntryCount, mCaptureNames);
    return kResultOk;
}

//------------------------------------------------------------------------
IPlugView *PLUGIN_API RationsController::createView(FIDString name)
{
    if (name && strcmp(name, Vst::ViewType::kEditor) == 0)
        return new RationsEditorView(this);
    return nullptr;
}

// The view registers itself here rather than in createView, because a host may create a view and
// never attach it (the SDK validator does exactly that). Both hooks run on the host's UI thread,
// the same thread setParamNormalized and notify() arrive on, so mView needs no locking.
void RationsController::editorAttached(Vst::EditorView *editor)
{
    mView = static_cast<RationsEditorView *>(editor);
    if (!mView)
        return;
    // Push the current values in immediately: the editor has just built its window and knows
    // nothing yet, and without this it would paint every control at its default until the next
    // parameter change happened to arrive.
    for (int32 i = 0; i < parameters.getParameterCount(); ++i) {
        if (Vst::Parameter *p = parameters.getParameterByIndex(i))
            mView->ParamChanged(p->getInfo().id, p->getNormalized());
    }
    mView->ModelCapsChanged(mEntryCount, mCaptureNames);
}

void RationsController::editorRemoved(Vst::EditorView *editor)
{
    if (mView == static_cast<RationsEditorView *>(editor))
        mView = nullptr;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::setParamNormalized(Vst::ParamID tag, Vst::ParamValue value)
{
    const tresult result = EditController::setParamNormalized(tag, value);
    if (mView && result == kResultOk)
        mView->ParamChanged(tag, value);
    return result;
}

//------------------------------------------------------------------------
void RationsController::requestCaps()
{
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(kMsgRequestCaps);
    sendMessage(message);
}

//------------------------------------------------------------------------
// Forward a path to the processor over the connection. When no peer is connected (a host
// inspecting the controller alone), the local copy is still updated and kResultFalse is returned.
tresult RationsController::sendPath(const char *messageID, const char8 *path)
{
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return kResultFalse;
    message->setMessageID(messageID);
    const char *p = path ? path : "";
    message->getAttributes()->setBinary(kMsgPathAttr, p, static_cast<uint32>(strlen(p)));
    return sendMessage(message);
}

tresult RationsController::setIrFile(int slot, const char8 *path)
{
    if (slot < 0 || slot >= kIrSlotCount)
        return kInvalidArgument;
    // Sent BEFORE the row is updated, and the row is normally updated only if the processor
    // accepted it: a malformed or unreadable WAV is refused, and a row naming a file the audio
    // path does not have would be telling the user something untrue about what they hear.
    //
    // The exception is a host that has not connected the two halves. sendMessage answers
    // kResultFalse both for "the processor said no" and for "there was nobody to ask", and only
    // the first is a reason to leave the row empty; treating the second the same way would make
    // the browser look inert in a host whose connection is merely late. So the peer is checked
    // first, and with no peer the row is updated optimistically - which is the behaviour this
    // plug-in has always had, kept for exactly the case where nothing better is knowable.
    const tresult result = sendPath(kMsgLoadIr[slot], path);
    if (result != kResultOk && getPeer())
        return result;
    mIrPath[slot] = path ? path : "";
    if (mView)
        mView->FilesChanged();
    return result;
}

//------------------------------------------------------------------------
// The four capture rows. Same shape as setIrFile above and for the same reasons, including the
// no-peer case: sendMessage cannot distinguish "the processor refused" from "there was nobody to
// ask", and only the first is a reason to leave the row empty.
//
// Unlike the parent plug-in there is nothing to clear here. NAMp's single-capture and bank loaders
// are alternatives and each wipes the other; each channel here owns exactly one source, so a load
// replaces what that channel had and no other channel is involved.
// Slim does NOT go out on every setParamNormalized, which is where the sibling plug-in puts it.
// There, applying a value swaps a size on a model that is already built; here it rebuilds every
// capture in every bank, so a drag would ask for one rebuild per step. The epoch counter would
// cancel all but the last and the result would be correct — and every channel would sit at ramped
// silence for the whole gesture rather than for one build. So the editor calls this on release.
tresult RationsController::applySlim()
{
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return kResultFalse;
    message->setMessageID(kMsgSetSlim);
    message->getAttributes()->setFloat(kSlimAttr, getParamNormalized(kSlimId));
    return sendMessage(message);
}

//------------------------------------------------------------------------
tresult RationsController::setCaptureSource(int channel, const char8 *path, bool isDirectory)
{
    if (channel < 0 || channel >= kChannelCount)
        return kInvalidArgument;

    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return kResultFalse;
    message->setMessageID(kMsgLoadCapture[channel]);
    const char *p = path ? path : "";
    message->getAttributes()->setBinary(kMsgPathAttr, p, static_cast<uint32>(strlen(p)));
    // An empty path is a clear, and a clear is not a directory whatever the caller said. Recording
    // it as one would make the next state blob claim a folder that is not there.
    message->getAttributes()->setInt(kMsgIsDirAttr, (*p && isDirectory) ? 1 : 0);
    const tresult result = sendMessage(message);
    if (result != kResultOk && getPeer())
        return result;

    mCapturePath[channel] = p;
    mCaptureIsDir[channel] = *p && isDirectory;
    // The names and counts this channel had describe captures that are no longer loaded. Clearing
    // them here rather than waiting for the reply keeps the row from naming the old bank's
    // captures for the second or so the new ones take to build.
    mEntryCount[channel] = 0;
    mBankIsDir[channel] = mCaptureIsDir[channel];
    mBankLevels[channel] = CaptureLevels();
    mCaptureNames[channel].clear();
    refreshParamTitles();
    if (mView)
        mView->FilesChanged();
    return result;
}

const std::string &RationsController::capturePath(int channel) const
{
    static const std::string kEmpty;
    if (channel < 0 || channel >= kChannelCount)
        return kEmpty;
    return mCapturePath[channel];
}

bool RationsController::captureIsDirectory(int channel) const
{
    return channel >= 0 && channel < kChannelCount && mCaptureIsDir[channel];
}

//------------------------------------------------------------------------
tresult RationsController::setChannelName(int channel, const char8 *name)
{
    if (channel < 0 || channel >= kChannelCount)
        return kInvalidArgument;

    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return kResultFalse;
    message->setMessageID(kMsgChannelName);
    const char *n = name ? name : "";
    message->getAttributes()->setInt(kMidiRowAttr, channel);
    message->getAttributes()->setBinary(kMsgNameAttr, n, static_cast<uint32>(strlen(n)));
    const tresult result = sendMessage(message);

    // Updated whatever the processor said. A name is not a load: there is nothing for the other
    // half to refuse, and it holds a copy only because it is the half that writes the state blob.
    mChannelNameOverride[channel] = n;
    if (mView)
        mView->FilesChanged();
    return result;
}

const std::string &RationsController::channelNameOverride(int channel) const
{
    static const std::string kEmpty;
    if (channel < 0 || channel >= kChannelCount)
        return kEmpty;
    return mChannelNameOverride[channel];
}

//------------------------------------------------------------------------
// The three-deep name rule, resolved in exactly one place so the head panel's dial legend, the
// settings page's level rows and its MIDI rows cannot drift apart.
//
// The basename is the middle step and it is the one that carries the feature. Whether a host hands
// keyboard events to an embedded plug-in view is the host's business and not something this code
// can guarantee, so a name that could ONLY be typed would be a name some users could never set. A
// folder called "JCM800" names the channel JCM800 with nothing typed at all.
std::string RationsController::channelName(int channel) const
{
    if (channel < 0 || channel >= kChannelCount)
        return std::string();
    if (!mChannelNameOverride[channel].empty())
        return mChannelNameOverride[channel];
    if (!mCapturePath[channel].empty()) {
        // For a single capture this drops the ".nam", which is what the user would have called it
        // anyway; for a folder there is no extension to drop and it is the folder's own name.
        std::string base = pathBaseName(mCapturePath[channel]);
        if (!mCaptureIsDir[channel]) {
            const size_t dot = base.find_last_of('.');
            if (dot != std::string::npos && dot > 0)
                base.erase(dot);
        }
        if (!base.empty())
            return base;
    }
    return kChannelDefaultName[channel];
}

//------------------------------------------------------------------------
std::string RationsController::midiRowLabel(int row) const
{
    if (row < 0 || row >= kMidiLearnRowCount)
        return std::string();
    if (row < kMidiLearnChannelRows)
        return channelName(row);
    return kMidiLearnRows[row].label;
}

//------------------------------------------------------------------------
int RationsController::entryCount(int channel) const
{
    return (channel >= 0 && channel < kChannelCount) ? mEntryCount[channel] : 0;
}

bool RationsController::bankIsDirectory(int channel) const
{
    return channel >= 0 && channel < kChannelCount && mBankIsDir[channel];
}

bool RationsController::bankHasLoudness(int channel) const
{
    return channel >= 0 && channel < kChannelCount && mBankLevels[channel].hasLoudness;
}

bool RationsController::bankHasInputLevel(int channel) const
{
    return channel >= 0 && channel < kChannelCount && mBankLevels[channel].hasInputLevel;
}

bool RationsController::bankHasOutputLevel(int channel) const
{
    return channel >= 0 && channel < kChannelCount && mBankLevels[channel].hasOutputLevel;
}

bool RationsController::anyBankSlimmable() const
{
    for (int c = 0; c < kChannelCount; ++c)
        if (mEntryCount[c] > 0 && mBankLevels[c].slimmable)
            return true;
    return false;
}

//------------------------------------------------------------------------
// Retitle a parameter in place and let the host re-read the titles. This is what a generic
// (host-drawn) parameter UI shows in place of the editor's greyed-out controls: an option the
// loaded captures cannot honour reads "(n/a)" there rather than looking available and doing
// nothing.
// Returns whether the title actually changed, which the caller needs: restartComponent is a heavy
// request - the host invalidates every cached parameter info and asks for all of them again - and
// this is reached from the capability poll, which fires several times a second while the banks
// build. Rewriting the same string and then telling the host to re-read everything would be a dozen
// full parameter re-reads for no change at all.
bool RationsController::retitleParam(Vst::ParamID tag, const char *title)
{
    Vst::Parameter *param = parameters.getParameter(tag);
    if (!param)
        return false;
    Vst::String128 wanted = {};
    UString(wanted, USTRINGSIZE(wanted)).fromAscii(title);
    if (memcmp(param->getInfo().title, wanted, sizeof(wanted)) == 0)
        return false;
    memcpy(param->getInfo().title, wanted, sizeof(wanted));
    return true;
}

// Which channel's captures the titles describe is the SOUNDING one, for the same reason the
// editor greys against it: the mode applies per capture and falls back to unity where the metadata
// is absent, so what a player needs to know is whether it will do anything to what they are
// hearing now.
void RationsController::refreshParamTitles()
{
    const int active = static_cast<int>(channelFromNorm(getParamNormalized(kActiveChannelId)));
    // A channel with nothing loaded says nothing about what its captures support, because it has
    // none — so nothing is marked unavailable until there is something to have said it. Same rule
    // the editor's own greying follows, and the same one the upstream plug-in follows by leaving
    // its controls alone entirely while no model is loaded.
    const bool loaded = mEntryCount[active] > 0;
    const bool hasLoudness = !loaded || bankHasLoudness(active);
    const bool hasIn = !loaded || bankHasInputLevel(active);
    const bool hasOut = !loaded || bankHasOutputLevel(active);

    bool changed = false;
    changed |=
        retitleParam(kOutputModeId, (hasLoudness || hasOut) ? "Output Mode" : "Output Mode (n/a)");
    changed |= retitleParam(kCalibrateInputId, hasIn ? "Calibrate Input" : "Calibrate Input (n/a)");
    // Slim is retitled rather than hidden for a host's own parameter list, which has no way to
    // hide anything - the editor is what drops the icon. Nothing loaded reads as available for the
    // same reason the three above do: an empty channel has no captures, so it has said nothing
    // about what its captures support.
    const bool anyLoaded = [this] {
        for (int c = 0; c < kChannelCount; ++c)
            if (mEntryCount[c] > 0)
                return true;
        return false;
    }();
    changed |= retitleParam(kSlimId, (!anyLoaded || anyBankSlimmable()) ? "Slim" : "Slim (n/a)");
    changed |= retitleParam(kInputCalLevelId,
                            hasIn ? "Input Calibration Level" : "Input Calibration Level (n/a)");
    for (int c = 0; c < kChannelCount; ++c) {
        // The gain dial is named after the channel and says so when there is nothing to sweep: a
        // bank of one is a bank the dial cannot travel across.
        const std::string gain = channelName(c) + (mEntryCount[c] > 1 ? " Gain" : " Gain (n/a)");
        changed |= retitleParam(kChannelGainId[c], gain.c_str());
        changed |= retitleParam(kChannelLevelId[c], (channelName(c) + " Level").c_str());
    }
    // Only when something actually moved — see retitleParam.
    if (changed && componentHandler)
        componentHandler->restartComponent(Vst::kParamTitlesChanged);
}

// Truncation is a failure rather than a silent short path: a caller that acted on half a path
// would open the wrong file.
tresult RationsController::copyPath(const std::string &src, char8 *buffer, int32 bufferSize)
{
    if (!buffer || bufferSize <= 0)
        return kInvalidArgument;
    if (src.size() + 1 > static_cast<size_t>(bufferSize)) {
        buffer[0] = 0;
        return kResultFalse;
    }
    memcpy(buffer, src.c_str(), src.size() + 1);
    return kResultOk;
}

tresult RationsController::getIrFile(int slot, char8 *buffer, int32 bufferSize) const
{
    if (slot < 0 || slot >= kIrSlotCount)
        return kInvalidArgument;
    return copyPath(mIrPath[slot], buffer, bufferSize);
}

//------------------------------------------------------------------------
// MIDI learn. Every one of these is a request, not a change: the table this controller holds is a
// copy for the editor to draw, and the processor's is the one a footswitch talks to. Editing this
// copy directly and hoping the two stay in step is exactly the class of bug the single-parameter
// channel decision exists to avoid, so it is not done here either.
void RationsController::sendMidiRow(const char *messageID, int row)
{
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(messageID);
    message->getAttributes()->setInt(kMidiRowAttr, row);
    sendMessage(message);
}

void RationsController::armMidiLearn(int row)
{
    if (row < -1 || row >= kMidiLearnRowCount)
        return;
    // Shown as armed straight away rather than waiting for the round trip, because the user is
    // about to stamp on a pedal and a button that takes a message round trip to light up reads as
    // one that did not register the click. The processor's reply corrects it either way.
    mArmedRow = row;
    sendMidiRow(kMsgMidiLearn, row);
    if (mView)
        mView->FilesChanged();
}

void RationsController::clearMidiLearn(int row)
{
    if (row < 0 || row >= kMidiLearnRowCount)
        return;
    sendMidiRow(kMsgMidiClear, row);
}

void RationsController::requestMidiTable()
{
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(kMsgRequestMidi);
    sendMessage(message);
}

const MidiBinding &RationsController::midiBinding(int row) const
{
    static const MidiBinding kUnlearned;
    if (row < 0 || row >= kMidiLearnRowCount)
        return kUnlearned;
    return mMidiTable[row];
}

// The processor's answer. Parsed defensively for the same reason the capability blob is: it is
// our own processor, but a short or absent attribute must leave a readable table rather than an
// out-of-range read, because the editor indexes into it every repaint.
tresult RationsController::receiveMidiTable(Vst::IMessage *message)
{
    Vst::IAttributeList *attrs = message ? message->getAttributes() : nullptr;
    if (!attrs)
        return kResultFalse;

    const void *data = nullptr;
    uint32 size = 0;
    if (attrs->getBinary(kMidiTableAttr, data, size) == kResultOk && data &&
        size == kMidiLearnRowCount * sizeof(std::uint32_t)) {
        std::uint32_t words[kMidiLearnRowCount] = {};
        memcpy(words, data, sizeof(words));
        for (int row = 0; row < kMidiLearnRowCount; ++row)
            mMidiTable[row] = unpackBinding(words[row]);
    }

    int64 armed = -1;
    if (attrs->getInt(kMidiArmedAttr, armed) != kResultOk)
        armed = -1;
    mArmedRow = (armed >= 0 && armed < kMidiLearnRowCount) ? static_cast<int>(armed) : -1;

    if (mView)
        mView->FilesChanged();
    return kResultOk;
}

} // namespace Rations
