#pragma once

#include "pluginterfaces/base/fplatform.h"

// KEEP IN STEP WITH project(... VERSION x.y.z) IN CMakeLists.txt. They are two
// separate sources of truth -- this file is the SDK's own pattern and is what the
// plug-in reports to a host for each CLASS, while the project() version is what
// moduleinfo.json, the three makedist scripts and the installer read. Nothing
// checks that they agree, and they have already disagreed once: the module said
// 0.2.0 while every class still answered 0.1.0.1.
#define MAJOR_VERSION_STR "0"
#define MAJOR_VERSION_INT 0
#define SUB_VERSION_STR "3"
#define SUB_VERSION_INT 3
#define RELEASE_NUMBER_STR "0"
#define RELEASE_NUMBER_INT 0
#define BUILD_NUMBER_STR "1"
#define BUILD_NUMBER_INT 1

#define FULL_VERSION_STR                                                                           \
    MAJOR_VERSION_STR "." SUB_VERSION_STR "." RELEASE_NUMBER_STR "." BUILD_NUMBER_STR
#define VERSION_STR MAJOR_VERSION_STR "." SUB_VERSION_STR "." RELEASE_NUMBER_STR

#define stringPluginName "NAMp Rations"
#define stringOriginalFilename "NAMp-rations.vst3"
#if SMTG_PLATFORM_64
#define stringFileDescription stringPluginName " (64Bit)"
#else
#define stringFileDescription stringPluginName
#endif
// The category a host files the plug-in under. Named here rather than written into the factory,
// because the LV2 bundle has to say the same thing in its own vocabulary and a second literal in
// a second file is how the two formats end up in different folders of the same host's browser.
#define stringSubCategory "Fx|Distortion"
#define stringCompanyName "rations"
#define stringCompanyWeb "https://github.com/rations/NAMp-rations"
#define stringCompanyEmail "mailto:ehqcar@proton.me"
#define stringLegalCopyright "MIT licence; NAM DSP core (C) Steven Atkinson"
#define stringLegalTrademarks "VST is a trademark of Steinberg Media Technologies GmbH"
