// Rations edit controller — the plug-in's parameters, and later the native editor it hands the
// host through createView().
//
// PHASE 0 SKELETON: parameters and state only. There is no INampFileLoader equivalent here yet;
// unlike NAMp, this plug-in has no capture browser at all (the captures ship in the bundle and
// are not a user choice), so the only file paths that will ever need a GUI-less route in are the
// cabinet page's two impulse responses.

#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

namespace Rations
{

//------------------------------------------------------------------------
class RationsController : public Steinberg::Vst::EditController
{
public:
    static Steinberg::FUnknown *createInstance(void *)
    {
        return (Steinberg::Vst::IEditController *)new RationsController();
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream *state) SMTG_OVERRIDE;
};

} // namespace Rations
