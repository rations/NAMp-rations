// Rations plug-in factory.

#include "rationscontroller.h"
#include "rationsids.h"
#include "rationsprocessor.h"
#include "version.h"

#include "public.sdk/source/main/pluginfactory_constexpr.h"

BEGIN_FACTORY_DEF(stringCompanyName, stringCompanyWeb, stringCompanyEmail, 2)

DEF_CLASS(Rations::RationsProcessorUID, Steinberg::PClassInfo::kManyInstances, kVstAudioEffectClass,
          stringPluginName, Steinberg::Vst::kDistributable, "Fx|Distortion", FULL_VERSION_STR,
          kVstVersionString, Rations::RationsProcessor::createInstance, nullptr)

DEF_CLASS(Rations::RationsControllerUID, Steinberg::PClassInfo::kManyInstances,
          kVstComponentControllerClass, stringPluginName "Controller", 0, "", FULL_VERSION_STR,
          kVstVersionString, Rations::RationsController::createInstance, nullptr)

END_FACTORY
