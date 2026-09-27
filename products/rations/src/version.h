#pragma once

#include "pluginterfaces/base/fplatform.h"

// THE VERSION IS NOT SET HERE. It is set once, in the VERSION file at the repository root; the
// build reads that file and hands its three numbers to every target as NAMP_VERSION_MAJOR,
// NAMP_VERSION_MINOR and NAMP_VERSION_PATCH, and this header only turns them into the macro names
// the SDK's own pattern uses. It is what the plug-in reports to a host for each CLASS, and the LV2
// build derives lv2:minorVersion and lv2:microVersion from it.
//
// This file held the numbers itself until 0.6.0, as a second source of truth beside project(),
// and nothing checked that the two agreed. They disagreed twice: once when the module said 0.2.0
// while every class still answered 0.1.0.1, and again when the first 0.6.0 commit bumped this
// header in one product and not the other.
#if !defined(NAMP_VERSION_MAJOR) || !defined(NAMP_VERSION_MINOR) || !defined(NAMP_VERSION_PATCH)
#error "NAMP_VERSION_* are not defined: the build sets them from the VERSION file at the root"
#endif
#define NAMP_VERSION_STRINGIFY_(x) #x
#define NAMP_VERSION_STRINGIFY(x) NAMP_VERSION_STRINGIFY_(x)

#define MAJOR_VERSION_STR NAMP_VERSION_STRINGIFY(NAMP_VERSION_MAJOR)
#define MAJOR_VERSION_INT NAMP_VERSION_MAJOR
#define SUB_VERSION_STR NAMP_VERSION_STRINGIFY(NAMP_VERSION_MINOR)
#define SUB_VERSION_INT NAMP_VERSION_MINOR
#define RELEASE_NUMBER_STR NAMP_VERSION_STRINGIFY(NAMP_VERSION_PATCH)
#define RELEASE_NUMBER_INT NAMP_VERSION_PATCH
// The SDK's fourth part. It is not part of the release version: the package names, the installers
// and moduleinfo.json's module "Version" all carry x.y.z, and only each class reports the fourth
// part, as in moduleinfo.json's per-class "0.6.0.1". It has been 1 in every release, so it stays a
// constant here.
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
