// MidiRoute implementation. See midiroute.h.

#include "midiroute.h"

#include "pluginterfaces/vst/ivstcomponent.h" // MediaTypes::kEvent, BusDirections::kInput
#include "pluginterfaces/vst/ivstunits.h"

#include <cstdio>

using namespace Steinberg;

namespace Rations
{

//------------------------------------------------------------------------
MidiRoute::MidiRoute()
{
    for (int ch = 0; ch < kChannels; ++ch)
        for (int cc = 0; cc < kControllers; ++cc)
            mCc[ch][cc] = Vst::kNoParamId;
}

//------------------------------------------------------------------------
namespace
{

// The program list a unit uses, or kNoProgramListId. Walks getUnitInfo comparing ids rather than
// indexing, because a unit's index and its id are different numbers.
Vst::ProgramListID programListOfUnit(Vst::IUnitInfo &units, Vst::UnitID unitId)
{
    const int32 count = units.getUnitCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::UnitInfo info = {};
        if (units.getUnitInfo(i, info) != kResultOk)
            continue;
        if (info.id == unitId)
            return info.programListId;
    }
    return Vst::kNoProgramListId;
}

int32 programsInList(Vst::IUnitInfo &units, Vst::ProgramListID listId)
{
    const int32 count = units.getProgramListCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::ProgramListInfo info = {};
        if (units.getProgramListInfo(i, info) != kResultOk)
            continue;
        if (info.id == listId)
            return info.programCount;
    }
    return 0;
}

// A parameter list is not an assertion that the parameter exists: IMidiMapping and IUnitInfo both
// hand back an id, and a host that writes to an id nothing was declared under simply loses the
// message with no error anywhere. So both routes are checked against the declared parameters
// before they are believed - which is the same check tools/rations_midicheck.cpp makes.
bool parameterExists(Vst::IEditController &controller, Vst::ParamID id, int32 requiredFlags)
{
    const int32 count = controller.getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info = {};
        if (controller.getParameterInfo(i, info) != kResultOk)
            continue;
        if (info.id != id)
            continue;
        return requiredFlags == 0 || (info.flags & requiredFlags) == requiredFlags;
    }
    return false;
}

} // namespace

//------------------------------------------------------------------------
void MidiRoute::resolve(Vst::IEditController *controller)
{
    if (!controller)
        return;

    // --- Control Change --------------------------------------------------
    if (FUnknownPtr<Vst::IMidiMapping> mapping = FUnknownPtr<Vst::IMidiMapping>(controller)) {
        for (int ch = 0; ch < kChannels; ++ch) {
            for (int cc = 0; cc < kControllers; ++cc) {
                Vst::ParamID id = Vst::kNoParamId;
                if (mapping->getMidiControllerAssignment(0, static_cast<int16>(ch),
                                                         static_cast<Vst::CtrlNumber>(cc),
                                                         id) != kResultTrue)
                    continue;
                if (id == Vst::kNoParamId || !parameterExists(*controller, id, 0))
                    continue;
                mCc[ch][cc] = id;
                mHasCc = true;
            }
        }
    }

    // --- Program Change --------------------------------------------------
    if (FUnknownPtr<Vst::IUnitInfo> units = FUnknownPtr<Vst::IUnitInfo>(controller)) {
        for (int ch = 0; ch < kChannels; ++ch) {
            Vst::UnitID unitId = Vst::kRootUnitId;
            if (units->getUnitByBus(Vst::kEvent, Vst::kInput, 0, static_cast<int16>(ch), unitId) !=
                kResultTrue)
                continue;

            const Vst::ProgramListID listId = programListOfUnit(*units, unitId);
            if (listId == Vst::kNoProgramListId)
                continue;

            // EditControllerEx1 builds a program list's parameter with the LIST's own id as the
            // ParamID (public.sdk/source/vst/vsteditcontroller.cpp), so the two numbers are the
            // same number by construction rather than by coincidence.
            const Vst::ParamID id = static_cast<Vst::ParamID>(listId);
            if (!parameterExists(*controller, id, Vst::ParameterInfo::kIsProgramChange))
                continue;

            const int32 count = programsInList(*units, listId);
            if (count < 2)
                continue;

            mProgram[ch].id = id;
            mProgram[ch].count = count;
            mHasProgram = true;
        }
    }

    if (!mHasCc)
        fprintf(stderr,
                "namp-rations-standalone: no CC mapping - a footswitch sending Control Change "
                "will not be heard\n");
    if (!mHasProgram)
        fprintf(stderr,
                "namp-rations-standalone: no Program Change parameter - a footswitch sending "
                "Program Change will not be heard\n");
}

} // namespace Rations
