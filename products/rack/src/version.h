#pragma once

#include "pluginterfaces/base/fplatform.h"

// KEEP IN STEP WITH project(... VERSION x.y.z) IN CMakeLists.txt. They are two
// separate sources of truth -- this file is the SDK's own pattern and is what the
// plug-in reports to a host for each CLASS, while the project() version is what
// moduleinfo.json, the makedist scripts and the installer read. Nothing checks
// that they agree, and in the parent amp they have already disagreed once: the
// module said 0.2.0 while every class still answered 0.1.0.1.
#define MAJOR_VERSION_STR "0"
#define MAJOR_VERSION_INT 0
#define SUB_VERSION_STR "1"
#define SUB_VERSION_INT 1
#define RELEASE_NUMBER_STR "0"
#define RELEASE_NUMBER_INT 0
#define BUILD_NUMBER_STR "1"
#define BUILD_NUMBER_INT 1

#define FULL_VERSION_STR                                                                           \
    MAJOR_VERSION_STR "." SUB_VERSION_STR "." RELEASE_NUMBER_STR "." BUILD_NUMBER_STR
#define VERSION_STR MAJOR_VERSION_STR "." SUB_VERSION_STR "." RELEASE_NUMBER_STR

// The amp this rack is built around, named as its own product rather than as the parent's.
//
// This file was a verbatim copy of the parent amp's for as long as this tree has existed, so it
// announced "NAMp Rations" to every host while the build produced NAMp-Rack-Amp.vst3 — a bundle
// whose folder, inner binary and reported name disagreed three ways. A host shows the name from
// here, so that is what a user would have seen in their plug-in list, twice, with no way to tell
// which was which. The class UIDs were the same pair as well; see src/rationsids.h, which is
// where the consequence of that is written down.
#define stringPluginName "NAMp Rack Amp"
#define stringOriginalFilename "NAMp-Rack-Amp.vst3"
#if SMTG_PLATFORM_64
#define stringFileDescription stringPluginName " (64Bit)"
#else
#define stringFileDescription stringPluginName
#endif
// The category a host files the plug-in under. Named here rather than written into the factory,
// because an LV2 bundle has to say the same thing in its own vocabulary and a second literal in
// a second file is how the two formats end up in different folders of the same host's browser.
#define stringSubCategory "Fx|Distortion"
#define stringCompanyName "rations"
#define stringCompanyWeb "https://github.com/rations/NAMp-Rack"
#define stringCompanyEmail "mailto:ehqcar@proton.me"
#define stringLegalCopyright "MIT licence; NAM DSP core (C) Steven Atkinson"
#define stringLegalTrademarks "VST is a trademark of Steinberg Media Technologies GmbH"
