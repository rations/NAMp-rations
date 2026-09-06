# Rations — a four-channel Neural Amp Modeler amp head, a raw VST3 plug-in for Linux, Windows and
# macOS (no framework).
#
# The DSP core is the MIT-licensed NeuralAmpModelerCore + AudioDSPTools by Steven Atkinson, with
# Eigen (MPL2) as the linear-algebra backend. The editor is a Steinberg IPlugView embedded into
# the host's own window and painted with Cairo/FreeType — no plug-in or GUI framework.
#
# TWO products on Linux, ONE on Windows and macOS. The VST3 bundle is the plug-in and is all those
# two ship; Linux adds namp-rations-standalone, which hosts that same bundle on JACK so the amp can
# be played without a DAW. standalone/ is X11, Linux::IRunLoop and JACK throughout, so a macOS or
# Windows version of it would be a different program rather than the same one rebuilt. What is still absent, and is the real distinction from the parent NAMp, is the
# RACK: nothing here hosts anyone else's plug-ins, scans for them or links LV2, because the
# pedalboard is built into the plug-in rather than hosting other people's. The plug-in itself links
# neither JACK nor the SDK's hosting layer — only the standalone and the offline tools do.
#
# Builds two ways:
#   - standalone: point the four dependency variables below at source trees (each defaults to an
#     in-tree submodule directory) and configure this directory directly;
#   - as a subdirectory of a project that already builds the VST3 SDK (the parent must provide
#     the SDK targets and public_sdk_SOURCE_DIR).

cmake_minimum_required(VERSION 3.25.0)

# ---------------------------------------------------------------------------
# macOS build shape, set BEFORE project() because that is when it is read.
#
# CMAKE_OSX_DEPLOYMENT_TARGET and CMAKE_OSX_ARCHITECTURES are consumed during
# compiler detection and by every try_compile the SDK's own checks run, so a
# value set after project() reaches the targets defined later and NOT the probes
# -- which is a build configured against one deployment target and checked
# against another, and it fails as a link warning about mismatched minimum
# versions rather than as anything that names the cause.
#
# Cache entries without FORCE, so a -D on the command line (which is how CI
# selects the slice) still wins. Both are inert on Linux and Windows: CMake
# ignores them off Apple, which is why they need no if(APPLE) guard here --
# APPLE is not defined until project() has run.
#
# 11.0 is the floor for arm64 and the oldest macOS any current DAW supports. The
# x86_64 slice could reach further back; it does not, because two slices built
# against two different SDKs is a difference nobody would think to look for when
# one of them misbehaves.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# ONE slice per configure. The universal binary is made by lipo joining two
# builds afterwards rather than by asking one compiler for both, and the reason
# is the gates: a fat binary built on an arm64 host cannot RUN its x86_64 half
# without assuming Rosetta 2 is installed, so every proof would silently only
# ever cover one architecture. Building each slice on a host of its own
# architecture means every gate runs on the ISA it gates. Defaulted to the host
# and validated in the platform split below.
set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}"
    CACHE STRING "macOS architecture slice to build")

project(smtg-rations
    VERSION 0.1.0
    DESCRIPTION "Rations four-channel neural amp head VST3"
)

set(RATIONS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Objective-C++, and it belongs HERE rather than in the platform split below,
# which is where every other platform decision in this file lives. Three files
# in this build are .mm: the editor's NSView, and the SDK's own module_mac.mm,
# which the validator, moduleinfotool and the offline tools each compile into a
# target of their own. Those targets are created by the SDK's add_subdirectory
# sixty lines down, and enable_language sets its rules FORWARD ONLY -- enabling
# it after that call leaves every one of them with no rule for a .mm file, and
# generation then fails with eleven copies of "Missing variable is:
# CMAKE_OBJCXX_COMPILE_OBJECT" after configure has already reported success.
# Guarded so a Linux or Windows build never asks for a compiler it cannot use.
if(APPLE)
    enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# Dependency source trees. Each defaults to the in-tree submodule path and can
# be pointed at an external checkout instead, which is how development against
# pristine reference trees works without initialising submodules.
# ---------------------------------------------------------------------------
set(NAM_CORE_DIR "${RATIONS_ROOT}/NeuralAmpModelerCore"
    CACHE PATH "NeuralAmpModelerCore source tree (v0.5.5 or newer)")
set(AUDIO_DSP_TOOLS_DIR "${RATIONS_ROOT}/AudioDSPTools"
    CACHE PATH "AudioDSPTools source tree")
set(EIGEN_DIR "${RATIONS_ROOT}/eigen"
    CACHE PATH "Eigen source tree (the directory containing Eigen/)")

# NAM_CORE_DIR must be v0.5.5+: submodels are built directly through the public
# parse_model_config_json()/create_dsp() pair in NAM/model_config.h and the
# channel switch reads DSP::GetPrewarmSamples(), neither of which is available
# in v0.5.3.
foreach(_dep NAM_CORE_DIR AUDIO_DSP_TOOLS_DIR EIGEN_DIR)
    if(NOT IS_DIRECTORY "${${_dep}}")
        message(FATAL_ERROR
            "${_dep} does not name a directory ('${${_dep}}') - run "
            "'git submodule update --init --recursive' or set -D${_dep}=<path>")
    endif()
endforeach()
foreach(_probe "${NAM_CORE_DIR}/NAM/model_config.h"
               "${AUDIO_DSP_TOOLS_DIR}/dsp/ImpulseResponse.h"
               "${EIGEN_DIR}/Eigen/Core")
    if(NOT EXISTS "${_probe}")
        message(FATAL_ERROR "expected dependency header not found: ${_probe}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# VST 3 SDK
# ---------------------------------------------------------------------------
if(NOT TARGET sdk)
    set(VST3_SDK_DIR "${RATIONS_ROOT}/vst3sdk"
        CACHE PATH "VST 3 SDK source tree")
    if(NOT EXISTS ${VST3_SDK_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "VST 3 SDK not found at '${VST3_SDK_DIR}' - run "
            "'git submodule update --init --recursive' or set -DVST3_SDK_DIR=<path>")
    endif()

    # Headless SDK: core libraries plus the validator (part of the hosting
    # examples), no VSTGUI (we draw the editor ourselves), no plug-in examples.
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "editor is drawn directly" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "validator" FORCE)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "plug-in examples not needed" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "run manually instead" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "install manually instead" FORCE)

    # Defaults ON, and its Windows implementation runs a freshly built
    # moduleinfotool.exe as a POST_BUILD step — which is a Windows binary when
    # cross-compiling, so it cannot execute on the build host. The file is
    # optional (Module::getModuleInfoPath returns an empty optional when it is
    # absent, and the validator does not require it), and scripts/makedist-*.sh
    # generate it under Wine instead, where the same binary does run.
    if(CMAKE_CROSSCOMPILING)
        set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "generated under Wine instead" FORCE)
    endif()

    add_subdirectory(${VST3_SDK_DIR} vst3sdk)

    # The SDK's plugin helper functions read these directory-scoped variables
    # (smtg_target_add_library_main silently adds no module entry point without
    # public_sdk_SOURCE_DIR); the SDK sets them only in its own scope.
    set(public_sdk_SOURCE_DIR "${VST3_SDK_DIR}/public.sdk")
    set(pluginterfaces_SOURCE_DIR "${VST3_SDK_DIR}/pluginterfaces")

    # public.sdk/source/vst/utility/dataexchange.cpp does not compile with
    # MinGW. It reaches alignedalloc.h, which picks its allocator on
    # `#elif defined(_MSC_VER)` -> _aligned_malloc, `#else` -> std::aligned_alloc.
    # MinGW is neither: it defines no _MSC_VER, and its msvcrt has no C11
    # aligned_alloc (verified — the CRT offers _aligned_malloc and
    # __mingw_aligned_malloc only), so the else branch fails to compile. The
    # test is really "is this the Microsoft CRT", not "is this the Microsoft
    # compiler", which is an upstream bug we must not fix in place: the SDK is a
    # pinned submodule and a local edit would be silently reverted by the next
    # `git submodule update`, leaving a fresh clone unable to build for Windows.
    #
    # Dropping the file instead is safe because Rations does not use the feature
    # it implements: DataExchangeHandler is the plug-in side of VST 3.7.9's
    # IDataExchangeHandler, nothing in src/ mentions it, and no other SDK source
    # we compile references its symbols. vstinitiids.cpp still defines the IIDs,
    # so the interface itself remains declared.
    if(WIN32)
        get_target_property(_sdk_sources sdk SOURCES)
        list(FILTER _sdk_sources EXCLUDE REGEX "utility/dataexchange\\.cpp$")
        set_target_properties(sdk PROPERTIES SOURCES "${_sdk_sources}")
        message(STATUS "[Rations] dropped dataexchange.cpp from the SDK (unused, and MinGW has no std::aligned_alloc)")

        # The SDK's Windows command-line tools define wmain, not main. MinGW
        # only wires wmain up to the CRT entry point when -municode is on the
        # LINK line; without it the linker falls back to libmingw32's
        # crtexewin.o, whose main() calls WinMain, and the build dies on an
        # undefined WinMain. MSVC needs none of this, which is why upstream
        # does not set it.
        foreach(_tool validator moduleinfotool)
            if(TARGET ${_tool})
                target_link_options(${_tool} PRIVATE -municode)
            endif()
        endforeach()
    endif()
endif()

# ---------------------------------------------------------------------------
# Neutralise the SDK's Windows folder-icon step when cross-compiling.
#
# smtg_target_make_plugin_package() adds a POST_BUILD step that copies
# PlugIn.ico and desktop.ini into the bundle and then runs "attrib +s" on all
# three paths. attrib is a cmd.exe built-in, not a program, so it cannot exist
# on a Linux build host and the step fails the build outright. The SDK offers no
# switch for it: SMTG_PACKAGE_ICON_PATH is forced back to the bundled Steinberg
# logo inside smtg_add_vst3plugin_with_pkgname() whenever it does not name an
# existing file, so it cannot be pointed at nothing.
#
# Redefining the function here — before smtg_add_vst3plugin() is called, so this
# is the definition that call resolves to — replaces it without touching the
# pinned submodule. What is lost is only the Explorer folder icon, which is
# cosmetic, is Steinberg's logo rather than ours, and does not survive a ZIP in
# any case: the +s attribute is not carried by the archive format.
# ---------------------------------------------------------------------------
if(CMAKE_CROSSCOMPILING AND WIN32)
    function(smtg_target_add_folder_icon target icon)
        # Deliberately empty. See above.
    endfunction()
endif()

# ---------------------------------------------------------------------------
# Symbol visibility: hidden by default, for everything we compile.
#
# This is a correctness requirement, not an optimisation. GCC gives a function-
# local static inside an inline function STB_GNU_UNIQUE binding, and glibc
# resolves those through a table that is shared by the whole link-map namespace
# — dlopen's RTLD_LOCAL does not scope it, and neither does anything else a host
# can do. So a default-visibility NAM symbol exported from Rations.so is not
# just visible to another plug-in built on the same DSP core; it IS that
# plug-in's copy. NeuralAmpModelerCore's architecture registry is exactly that
# shape (nam::ConfigParserRegistry::instance() in NAM/model_config.h), it
# registers from file-scope static initialisers, and registering a name twice
# throws. Loading a second NAM-based plug-in therefore threw out of an
# .init_array entry during dlopen, where there is no frame that can catch it,
# and the whole process aborted with "Config parser already registered for:
# SlimmableContainer". This plug-in and its parent NAMp are both NAM-based, and
# a guitarist may well have both in one project, so this is a live hazard here
# and not a theoretical one.
#
# Hidden symbols are not in the dynamic table at all, so they cannot be unique
# and cannot be interposed. Every NAM-based plug-in then gets its own registry,
# whichever order they load in. The three functions a VST3 host actually needs
# are re-exported by name through SMTG_EXPORT_SYMBOL — ModuleEntry, ModuleExit
# and GetPluginFactory — so nothing about the bundle's contract changes.
#
# Set here rather than before the SDK's add_subdirectory: these variables apply
# to targets defined after them, so this covers everything below and no target
# in an imported upstream tree.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
# Objective-C++ is a fourth language as far as CMake is concerned, and the C and
# C++ presets do not reach it: src/platform/macplugview.mm would otherwise be the
# one translation unit in the bundle compiled at default visibility. Set even
# where OBJCXX is not enabled, which costs nothing -- an unused variable.
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# The presets above cannot reach the standard library, and on GCC 12 that is not
# hypothetical: libstdc++ declares `namespace std` with _GLIBCXX_VISIBILITY(default),
# an explicit attribute, and an explicit attribute beats -fvisibility=hidden. So
# std::__detail::__to_chars_10_impl<T>::__digits - a function-local static in an
# inline template, i.e. exactly the shape described above - reaches the dynamic
# table with STB_GNU_UNIQUE binding no matter what the presets say, and the
# packaging script's unique-symbol gate rejects the bundle.
#
# -fno-gnu-unique makes GCC emit STB_GLOBAL for those statics instead. That
# removes the binding itself rather than hiding one instance of it, which is the
# property that actually matters: a unique symbol is resolved through a table
# dlopen's RTLD_LOCAL does not scope, and STB_GLOBAL in a bundle whose symbols
# are otherwise hidden and --exclude-libs'd is not.
#
# Note that these two symbols are libstdc++'s, not NAM's - a shared digit table
# is harmless in a way a shared architecture registry is not. The gate is a
# blanket "no unique symbols", though, and it is right to be: the safe cases
# cannot be told from the dangerous ones by name, and whitelisting the first one
# is how the check stops catching the second.
#
# GCC 13 reworked __to_chars_10_impl and the statics went away, so a newer
# toolchain never produces them; the flag is a no-op there rather than a
# behaviour change, and is left unconditional on GCC for that reason. Clang
# never emits STB_GNU_UNIQUE at all and does not take the flag.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fno-gnu-unique)
endif()

# ---------------------------------------------------------------------------
# Platform split. One product on all three platforms; the only OS-specific code
# is the single window class in src/platform, which arrives with the editor.
#
# THREE values, not two. This used to be RATIONS_WINDOWS alone, and "not
# Windows" silently meant Linux -- which was true while there were two
# platforms and became a trap the moment there were three: every
# if(NOT RATIONS_WINDOWS) in this file had to be re-read to find out whether it
# meant "Linux" (the JACK standalone, the rtcheck allocation harness, cairo-xlib)
# or genuinely "anything but Windows" (-fPIC, the module loader's else arm).
# Naming all three makes each of those a decision at its site instead of an
# inherited default.
# ---------------------------------------------------------------------------
set(RATIONS_WINDOWS OFF)
set(RATIONS_MACOS OFF)
set(RATIONS_LINUX OFF)
if(WIN32)
    set(RATIONS_WINDOWS ON)

    # Both, and not because it is tidy.
    #
    # UNICODE selects the wide variant of every TCHAR Win32 API; _UNICODE does
    # the same for the CRT's. MSVC's tchar.h defines each from the other, so on
    # that compiler the SDK's lone add_definitions(-D_UNICODE) gets both. MinGW
    # reconciles nothing (verified by grepping its headers), and the SDK's
    # if(MINGW) branch in SMTG_PlatformToolset.cmake sets neither.
    #
    # dllmain.cpp:87 is what that breaks. It calls the TCHAR-macro
    # GetModuleFileName into gPath, a Steinberg::tchar (char16_t) buffer, with
    # Steinberg::wscast papering over the pointer type, and then runs wcsrchr
    # over the result to find the directory separator. Without UNICODE the
    # macro resolves to GetModuleFileNameA, so ANSI bytes are written into a
    # UTF-16 buffer and the plug-in's own path comes out as garbage. In C the
    # compiler catches it -- "passing argument 2 of GetModuleFileNameA from
    # incompatible pointer type" -- but the C++ cast hides it completely.
    # NOMINMAX and WIN32_LEAN_AND_MEAN are defence and header weight, NOT a fix
    # for anything that currently bites. deps/wdl/wdltypes.h includes the whole
    # of <windows.h> under #ifdef _WIN32, and that reaches every translation
    # unit that includes src/pedals/pedalchain.h -- rationsprocessor.cpp and
    # tools/rations_pedalcheck.cpp, both of which use std::min/std::max freely.
    # Under MinGW that is already harmless twice over (windows.h self-defines
    # NOMINMAX under __cplusplus, and minwindef.h guards the min/max macros with
    # #ifndef __cplusplus anyway), so nothing here changes today. It would bite
    # under MSVC, it is exactly what NeuralAmpModelerCore's and AudioDSPTools'
    # own CMakeLists set, and this project compiles those sources into RationsDsp
    # and so never picks their flags up. WIN32_LEAN_AND_MEAN additionally keeps
    # winsock, RPC and OLE out of every pedal-including TU, which they have no
    # use for.
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE)
    set(RATIONS_MACOS ON)

    # Objective-C++ is enabled immediately after project(), NOT here: it has to
    # precede the SDK's add_subdirectory. See the comment at that site.

    # Both defaults are set before project() -- see the comment there for why
    # they cannot be set here. This is the check that the result is usable.
    list(LENGTH CMAKE_OSX_ARCHITECTURES _rations_arch_count)
    if(_rations_arch_count GREATER 1)
        message(FATAL_ERROR
            "[Rations] CMAKE_OSX_ARCHITECTURES names ${_rations_arch_count} architectures "
            "(${CMAKE_OSX_ARCHITECTURES}). Configure one slice at a time and join them with "
            "lipo -- see scripts/makedist-mac.sh. A fat build cannot run its own gates.")
    endif()
    message(STATUS "[Rations] macOS slice: ${CMAKE_OSX_ARCHITECTURES}, deployment target "
                   "${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
    set(RATIONS_LINUX ON)
endif()

# ---------------------------------------------------------------------------
# Editor graphics stack: Cairo drawing, FreeType text, PNG layers and runtime
# SVG rasterisation. Kept as its own library so the offline render tool can
# exercise exactly the code the editor paints with, without X11 or VST3.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
if(RATIONS_WINDOWS)
    # fontconfig is deliberately absent: there is not one Fc* call in this tree
    # (FontStack loads its two TTFs by path), and cairo makes fontconfig optional
    # on Windows, so requiring it here would only add a dependency nothing calls.
    # cairo-ft.h guards its fontconfig include behind CAIRO_HAS_FC_FONT.
    #
    # There is no cairo-xlib either. The Windows editor composes into the same
    # offscreen image surface the X11 one does and blits it with GDI, so cairo
    # needs only its image backend, its FreeType font backend and PNG.
    #
    # These come from scripts/build-win-deps.sh; Debian packages no MinGW build
    # of any of them. cmake/toolchain-mingw-w64.cmake points pkg-config at that
    # prefix through PKG_CONFIG_LIBDIR.
    #
    # --static is required, and not merely for completeness. Everything in that
    # sysroot is a static archive, and GNU ld resolves archives left to right in
    # one pass: pkg-config's shared Libs: line orders them "-lz -lpng16", which
    # is backwards for static linking and fails with undefined references to
    # crc32/inflate/deflate out of libpng16.a. The --static line repeats -lz
    # after -lpng16, which is exactly what the one-pass resolver needs.
    set(PKG_CONFIG_ARGN --static)
    pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo cairo-ft freetype2)

    # cairo.h declares every entry point __declspec(dllimport) unless this is
    # defined, and cairo's own .pc file does not carry it even for a static
    # build - so without it every cairo call fails to link as "undefined
    # reference to __imp_cairo_*". It belongs on the imported target rather than
    # on our own, because it describes how cairo must be USED, and it has to
    # reach every translation unit that includes a cairo header.
    target_compile_definitions(PkgConfig::CAIRO INTERFACE CAIRO_WIN32_STATIC_BUILD)
elseif(RATIONS_MACOS)
    # The same shape as the Windows branch above, and for the same reasons.
    # No fontconfig: there is not one Fc* call in this tree. No cairo-xlib and
    # no cairo-quartz either -- the mac editor composes into the same offscreen
    # image surface the other two do and blits it through CoreGraphics, so cairo
    # needs its image backend, its FreeType font backend and PNG, and nothing
    # else.
    #
    # --static for the reason the Windows branch spells out at length: this
    # points at scripts/build-mac-deps.sh's from-source sysroot, everything in
    # which is a static archive, and the shared Libs: line orders -lz before
    # -lpng16, which a one-pass resolver cannot satisfy. Homebrew is
    # deliberately NOT used: it ships dylibs under /opt/homebrew, and a bundle
    # that links those is loadable on the build machine and on no other.
    set(PKG_CONFIG_ARGN --static)
    pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo cairo-ft freetype2)
else()
    pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo cairo-ft freetype2 fontconfig)
    # The editor embeds into the host's X11 window; cairo-xlib paints it.
    pkg_check_modules(CAIROX11 REQUIRED IMPORTED_TARGET cairo-xlib x11)
endif()

# What the editor's window layer needs beyond Cairo itself. On Linux that is
# cairo-xlib and Xlib; on Windows it is GDI, because the Win32 view blits its
# offscreen image surface through a DIB section rather than drawing to an X
# drawable. Named once so the plug-in and the render tool cannot disagree.
if(RATIONS_WINDOWS)
    set(RATIONS_PLATFORM_GUI_LIBS user32 gdi32 msimg32)
elseif(RATIONS_MACOS)
    # Cocoa for the NSView and its event and timer plumbing, CoreGraphics for
    # the one CGImage blit that puts the cairo surface on screen, and
    # CoreFoundation because macmain.cpp's bundleEntry/bundleExit take a
    # CFBundleRef. The SDK adds CoreFoundation itself in
    # smtg_target_make_plugin_package(); naming it here as well costs nothing
    # and keeps this list a complete statement of what the window layer needs.
    set(RATIONS_PLATFORM_GUI_LIBS
        "-framework Cocoa" "-framework CoreGraphics" "-framework CoreFoundation")
else()
    set(RATIONS_PLATFORM_GUI_LIBS PkgConfig::CAIROX11)
endif()

add_library(RationsGfx STATIC
    ${RATIONS_ROOT}/src/gfx/canvas.cpp
    ${RATIONS_ROOT}/src/gfx/fontstack.cpp
    ${RATIONS_ROOT}/src/gfx/image.cpp
    ${RATIONS_ROOT}/src/gfx/svg.cpp
    ${RATIONS_ROOT}/src/gfx/resourcestore.h
    ${RATIONS_ROOT}/src/gfx/resourcestore.cpp
    ${RATIONS_ROOT}/src/platform/respath.cpp
)
target_include_directories(RationsGfx PUBLIC ${RATIONS_ROOT}/src)
target_link_libraries(RationsGfx PUBLIC PkgConfig::CAIRO ${CMAKE_DL_LIBS})
target_compile_features(RationsGfx PUBLIC cxx_std_17)
set_target_properties(RationsGfx PROPERTIES POSITION_INDEPENDENT_CODE ON)
# -Wall for our own code; the vendored NanoSVG headers are compiled as-is.
target_compile_options(RationsGfx PRIVATE -Wall)

# ---------------------------------------------------------------------------
# NAM DSP core as a static library, linked --whole-archive by the plug-in: each
# network architecture (WaveNet, LSTM, ConvNet, the slimmable container) self-
# registers via a file-scope static initialiser, and the linker would otherwise
# drop those object files from the plug-in .so, breaking model loading at
# runtime.
#
# NAM_ENABLE_A2_FAST routes models matching the A2 shape signature to the
# hand-optimised feed-forward WaveNet. That path is what makes running two
# models at once affordable, and its strictly finite memory is what makes both
# the gain crossfade and the channel switch exact once the incoming model has
# been fed one receptive field of input.
# ---------------------------------------------------------------------------
add_library(RationsDsp STATIC
    ${NAM_CORE_DIR}/NAM/activations.cpp
    ${NAM_CORE_DIR}/NAM/container.cpp
    ${NAM_CORE_DIR}/NAM/conv1d.cpp
    ${NAM_CORE_DIR}/NAM/convnet.cpp
    ${NAM_CORE_DIR}/NAM/dsp.cpp
    ${NAM_CORE_DIR}/NAM/get_dsp.cpp
    ${NAM_CORE_DIR}/NAM/linear.cpp
    ${NAM_CORE_DIR}/NAM/lstm.cpp
    ${NAM_CORE_DIR}/NAM/ring_buffer.cpp
    ${NAM_CORE_DIR}/NAM/util.cpp
    ${NAM_CORE_DIR}/NAM/wavenet/model.cpp
    ${NAM_CORE_DIR}/NAM/wavenet/a2_fast.cpp
    ${NAM_CORE_DIR}/NAM/wavenet/slimmable.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/dsp.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/ImpulseResponse.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/NoiseGate.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/RecursiveLinearFilter.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/wav.cpp
    ${RATIONS_ROOT}/deps/tonestack/ToneStack.cpp
)

# What it takes to COMPILE against these headers, held separately from the
# archive itself, because on macOS the two are consumed by different mechanisms.
#
# Everywhere else the plug-in names RationsDsp as an ordinary link item and CMake
# hands over its PUBLIC include paths, definitions and language level along with
# the archive. The mac arm of rations_link_whole_dsp() below cannot do that: ld64
# spells whole-archive as a -force_load FLAG carrying a path, so the target is
# never named and every one of those usage requirements is silently dropped. The
# first symptom is a missing NAM/dsp.h, which merely fails to build. The second
# is worse and would not have failed anything: without NAM_ENABLE_A2_FAST the
# A2 fast path is compiled out, and that path is the whole basis of the
# click-free gain crossfade and the instant channel switch.
#
# So the requirements live on an INTERFACE library both arms can name. It
# contributes no archive of its own, so naming it alongside -force_load cannot
# put the same objects on the link line twice.
add_library(RationsDspHeaders INTERFACE)

target_compile_definitions(RationsDspHeaders INTERFACE NAM_ENABLE_A2_FAST)

target_include_directories(RationsDspHeaders INTERFACE
    ${NAM_CORE_DIR}
    ${NAM_CORE_DIR}/NAM
    ${NAM_CORE_DIR}/Dependencies/nlohmann
    ${AUDIO_DSP_TOOLS_DIR}/dsp
    # And its parent, so a header outside that directory can say "dsp/dsp.h" and mean
    # AudioDSPTools' rather than NAM's. Both trees have a dsp.h, NAM's include dir is listed
    # first, and a bare "dsp.h" from src/pedals therefore reaches the wrong one -- with an error
    # about DSP_SAMPLE that says nothing about which of two identically-named headers arrived.
    ${AUDIO_DSP_TOOLS_DIR}
    ${EIGEN_DIR}
    ${RATIONS_ROOT}/deps/tonestack
    # The pedalboard's vendored DSP, reached as "bbm/..." and "wdl/..." so the
    # provenance of each block stays visible at every include site. Both trees are
    # header-only; there is nothing to add to the source list above.
    ${RATIONS_ROOT}/deps
)

# C++20: NAM's slimmable wavenet uses std::atomic<std::shared_ptr>, a C++20
# library feature. The plug-in itself stays at the SDK's C++17 baseline.
target_compile_features(RationsDspHeaders INTERFACE cxx_std_20)

# AudioDSPTools' ResamplingContainer.h declares, at class scope,
#
#     using LanczosResampler = LanczosResampler<T, NCHANS, A>;
#
# which redeclares a name already used in that scope to mean something else.
# [basic.scope.class]p2 forbade that, and GCC enforces it as a permerror - an
# error that only -fpermissive downgrades. GCC 14 implemented P1787R6, which
# deleted the rule, so the same header compiles clean there and the diagnostic
# is a pre-14 artefact rather than a defect in the code. It is why this tree
# builds on excalibur (GCC 14) and stops on daedalus (GCC 12).
#
# This is PUBLIC because it belongs to the header, not to this library: the
# AudioDSPTools include path above is PUBLIC, so every consumer that includes
# the resampler (Rations, rations_fadecheck, rations_switchcheck,
# rations_racecheck, rations_rtcheck, and anything added later) meets the same
# permerror, and each would otherwise have to rediscover the flag. Whoever
# publishes the header publishes what compiling it takes.
#
# Version-gated rather than unconditional so the loosened checking disappears on
# a compiler that does not need it, instead of quietly outliving its reason. The
# upstream tree is a pinned submodule, so patching the line there would be
# undone by the next submodule update.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    target_compile_options(RationsDspHeaders INTERFACE -fpermissive)
endif()
# The archive both compiles against those requirements and re-publishes them, so
# a target that links RationsDsp the ordinary way needs nothing else.
target_link_libraries(RationsDsp PUBLIC RationsDspHeaders)

# -fPIC has no meaning for PE and MinGW warns that it is being ignored; code in
# a Windows DLL is relocated by the loader, not by position-independent codegen.
if(RATIONS_WINDOWS)
    target_compile_options(RationsDsp PRIVATE -ffast-math -Wno-unused-parameter)
else()
    target_compile_options(RationsDsp PRIVATE -ffast-math -fPIC -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Link the whole of RationsDsp, whatever the linker is called.
#
# Five targets need every object out of that archive, not just the ones some
# symbol references: each network architecture registers itself through a
# file-scope static initialiser, and a normal static link drops the object files
# holding them because nothing names them. The symptom is a clean build that
# fails at run time with "No config parser registered for architecture: WaveNet".
#
# GNU ld spells that --whole-archive/--no-whole-archive around the library;
# Apple's ld64 has neither flag and spells it -force_load with the archive's
# path. -force_load already pulls in every object, so the library must NOT also
# be named as an ordinary link item or ld64 reports every symbol twice -- which
# is why the mac arm uses add_dependencies for the build ordering CMake would
# otherwise infer from the link line.
#
# Not naming the target also loses everything CMake would have carried with it:
# include paths, NAM_ENABLE_A2_FAST and the C++20 level. The mac arm therefore
# names RationsDspHeaders, the INTERFACE library those requirements were moved
# onto above -- it has no archive, so it cannot reintroduce the double-link this
# paragraph is about.
#
# Written as one function so the five call sites cannot drift apart, which they
# were already at risk of doing with the flags spelled out five times.
# ---------------------------------------------------------------------------
function(rations_link_whole_dsp target)
    if(RATIONS_MACOS)
        target_link_libraries(${target} PRIVATE
            RationsDspHeaders "-Wl,-force_load,$<TARGET_FILE:RationsDsp>")
        add_dependencies(${target} RationsDsp)
    else()
        target_link_libraries(${target}
            PRIVATE -Wl,--whole-archive RationsDsp -Wl,--no-whole-archive)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Art, fonts and captures, named once.
#
# The bundle copies all of these into Contents/Resources, and the offline panel
# render audits the art list so a layer that is drawn can never be absent from
# the build. Naming them in one place is what keeps those two from disagreeing.
# ---------------------------------------------------------------------------
set(RATIONS_FONT_FILES
    fonts/Michroma-Regular.ttf
    fonts/Roboto-Regular.ttf
)
# The *-base.png files in resources/img are the untrimmed source exports that
# gui/make_assets.sh reads; they are deliberately NOT in this list. What ships is
# what the editor loads.
set(RATIONS_IMG_FILES
    img/head.png
    # The head's title badge, drawn above "Rations" — see kBadgeH in src/geometry.h.
    img/namp-badge.png
    img/cabinet.png
    img/dial.png
    img/led_on.png
    img/led_off.png
    img/switch_up_ring.png
    img/switch_down_ring.png
    img/meter_track.png
    # The pedalboard. Five blank enclosures, each stored at the art's own trimmed
    # size and downscaled at draw time (the cabinet's arrangement, not the head's),
    # plus the chrome footswitch cap. The knob and the two LEDs are NOT repeated
    # here: the pedals blit the very same dial.png / led_on.png / led_off.png the
    # amp head does. Built by gui/make_pedals.sh from resources/img/pedals/, which
    # holds the untrimmed exports and the design references and ships nothing.
    img/pedal-boost.png
    img/pedal-chorus.png
    img/pedal-flanger.png
    img/pedal-delay.png
    img/pedal-reverb.png
    img/pedal_switch.png
    img/Folder.svg
    img/File.svg
    img/Gear.svg
    img/SlimmableIcon.svg
    img/Cross.svg
    img/ArrowLeft.svg
    img/ArrowRight.svg
)
# No badge: the "Rations" wordmark is drawn as text in Michroma at run time.

set(RATIONS_FONT_PATHS "")
foreach(_res IN LISTS RATIONS_FONT_FILES)
    list(APPEND RATIONS_FONT_PATHS "${RATIONS_ROOT}/resources/${_res}")
endforeach()
set(RATIONS_IMG_PATHS "")
foreach(_res IN LISTS RATIONS_IMG_FILES)
    list(APPEND RATIONS_IMG_PATHS "${RATIONS_ROOT}/resources/${_res}")
endforeach()

# No captures are built into the bundle, and nothing here looks for any.
#
# They used to be: four banks were globbed out of a captures/ directory, installed
# into Contents/Resources/captures/<channel>/, and a clone without them failed
# configure outright. That made the plug-in an amp head that could only play one
# person's amps, and made every build of it depend on files that are deliberately
# not in this repository. Both are gone. The four banks are now whatever folder or
# file the user picks on the settings page, remembered by absolute path in the
# state blob, and an instance with nothing loaded outputs ramped silence rather
# than failing to exist.
#
# The offline proofs still need captures to run against, and take a --captures
# argument (or $RATIONS_TEST_CAPTURES) rather than reading them out of a bundle.

# ---------------------------------------------------------------------------
# The plug-in.
#
# The native window class is picked here the same way src/platform/plugview.h
# picks the type it declares, so exactly one of the two is ever compiled: the
# other one's headers do not exist on this platform.
if(RATIONS_WINDOWS)
    set(RATIONS_PLATFORM_VIEW_SOURCES
        src/platform/win32plugview.h
        src/platform/win32plugview.cpp
    )
elseif(RATIONS_MACOS)
    set(RATIONS_PLATFORM_VIEW_SOURCES
        src/platform/macplugview.h
        src/platform/macplugview.mm
    )
else()
    set(RATIONS_PLATFORM_VIEW_SOURCES
        src/platform/x11plugview.h
        src/platform/x11plugview.cpp
    )
endif()

#
# The sources go straight into the bundle target rather than through an
# intermediate static library. NAMp needed that library because its standalone
# LINKS the plug-in's sources; namp-rations-standalone LOADS the built bundle instead
# — as a module, the way a DAW does, so the editor resolves its art through
# dladdr by the same route in both — and therefore compiles none of this. A
# library would only be a second name for one thing.
# ---------------------------------------------------------------------------
set(RATIONS_PLUGIN_SOURCES
    src/rationsids.h
    src/version.h
    src/geometry.h
    src/rationsprocessor.h
    src/rationsprocessor.cpp
    src/rationscontroller.h
    src/rationscontroller.cpp
    # The state blob's string reads, shared by the two functions above that decode one.
    src/statestream.h
    src/rationsfactory.cpp
    src/engineconfig.h
    src/spscqueue.h
    src/bank.h
    src/nativeresampler.h
    src/capturesource.h
    src/capturesource.cpp
    src/modelbank.h
    src/modelbank.cpp
    src/crossfadeengine.h
    src/crossfadeengine.cpp
    src/channelrack.h
    src/channelrack.cpp
    src/irblend.h
    src/irblend.cpp
    src/midilearn.h
    src/midilearn.cpp
    # The pedalboard. Five pedals plus the two chains they sit in; the pedals themselves are
    # header-only until the phase that gives each one a body, so only the chain has a .cpp.
    src/pedals/finite.h
    src/pedals/pedal.h
    src/pedals/boost.h
    src/pedals/chorus.h
    src/pedals/flanger.h
    src/pedals/delay.h
    src/pedals/reverb.h
    src/pedals/pedalchain.h
    src/pedals/pedalchain.cpp
    src/rationsview.h
    src/rationsview.cpp
    src/filebrowser.h
    src/filebrowser.cpp
    src/platform/plugview.h
    ${RATIONS_PLATFORM_VIEW_SOURCES}
)

# The TARGET stays "Rations" and the PACKAGE is what carries the product name, which is the
# whole reason this is written with PACKAGE_NAME rather than by renaming the target. The target
# name is threaded through every target_link_libraries, add_dependencies and get_target_property
# in this file and through nothing a user ever sees; the package name is the bundle folder
# NAMp-rations.vst3 and the binary inside it, which is all a user sees and none of what the build
# refers to. SOURCES_LIST is mandatory once PACKAGE_NAME is given — without it the sources land in
# PARAMS_UNPARSED_ARGUMENTS and the target is built with none
# (vst3sdk/cmake/modules/SMTG_AddVST3Library.cmake).
smtg_add_vst3plugin(Rations PACKAGE_NAME "NAMp-rations" SOURCES_LIST ${RATIONS_PLUGIN_SOURCES})

# macplugview.mm is Objective-C++ and is deliberately built WITHOUT -fobjc-arc,
# unlike the SDK's module_mac.mm above. It owns its NSView the way its Win32
# sibling owns its HWND -- an explicit create/destroy pair in a C++ object -- so
# manual retain/release is what makes the two files read alike, and ARC would
# additionally constrain how an Objective-C pointer may be held in a C++ member.
# Stated here because the absence of a flag is not visible at the file itself.

# ---------------------------------------------------------------------------
# Put the Windows DLL inside its own bundle.
#
# smtg_target_make_plugin_package() places it with
#     foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES}) ...
#         set_target_properties(... LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} ...)
# and CMAKE_CONFIGURATION_TYPES is EMPTY for every single-config generator, so
# with -G Ninja the loop body never runs: the DLL lands at the top of the build
# tree while Contents/Resources is created (and filled) inside the bundle. The
# result looks like a successful build and is not loadable.
#
# The path is read back from the properties the SDK itself set rather than
# spelled out, so this cannot drift from the layout the rest of the SDK — and
# module_win32.cpp's loader — expects.
#
# NAME. module_win32.cpp's validateBundleStructure requires the inner DLL to be
# named exactly like the bundle folder, i.e.
# NAMp-rations.vst3/Contents/x86_64-win/NAMp-rations.vst3, which is what the SUFFIX/PREFIX
# produce. A DLL called Rations.dll in the right folder does not load.
if(RATIONS_WINDOWS)
    get_target_property(RATIONS_PKG_PATH Rations SMTG_PLUGIN_PACKAGE_PATH)
    get_target_property(RATIONS_PKG_CONTENTS Rations SMTG_PLUGIN_PACKAGE_CONTENTS)
    get_target_property(RATIONS_WIN_ARCH Rations SMTG_WIN_ARCHITECTURE_NAME)

    # smtg_target_set_vst_win_architecture_name() derives this from
    # CMAKE_GENERATOR_PLATFORM, a Visual Studio concept that is simply unset
    # under Ninja + MinGW; it then falls into its else() and yields x86_64-win
    # by accident rather than by decision. Asserted so a future SDK change
    # cannot silently move the bundle out from under the resource path
    # resolver, which walks up two levels from the loaded module.
    if(NOT RATIONS_WIN_ARCH STREQUAL "x86_64-win")
        message(FATAL_ERROR
            "[Rations] the SDK chose architecture folder '${RATIONS_WIN_ARCH}', expected "
            "'x86_64-win'. The bundle layout and the resource path resolver both assume the "
            "latter.")
    endif()

    set_target_properties(Rations PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${RATIONS_PKG_PATH}/${RATIONS_PKG_CONTENTS}/${RATIONS_WIN_ARCH}")
    message(STATUS "[Rations] bundle binary -> ${RATIONS_PKG_PATH}/${RATIONS_PKG_CONTENTS}/${RATIONS_WIN_ARCH}")
endif()

# ---------------------------------------------------------------------------
# The macOS bundle's Info.plist.
#
# Written here because nothing else writes it. The SDK's smtg_target_set_bundle()
# does its plist work entirely inside if(XCODE) branches, and this project builds
# with Ninja, so under the SDK alone the bundle would carry whatever CMake's
# default template produces -- which knows nothing about VST3 and, in
# particular, nothing about CFBundlePackageType BNDL or NSHighResolutionCapable.
#
# configure_file() into the build tree rather than pointing
# MACOSX_BUNDLE_INFO_PLIST straight at the template: that property's own
# substitution only understands CMake's fixed MACOSX_BUNDLE_* variable names,
# and half of what this plist needs has no such name. Configuring it ourselves
# means every value comes from a variable named at its site.
#
# The two forms of the product name are D25's and the distinction is deliberate:
# CFBundleExecutable is IDENTITY and must match the binary CMake puts in
# Contents/MacOS, while CFBundleName is DISPLAY and is the string a user reads.
# stringPluginName in src/version.h is the same display string the DAW's plug-in
# list shows, so the two cannot disagree.
# ---------------------------------------------------------------------------
if(RATIONS_MACOS)
    set(RATIONS_BUNDLE_EXECUTABLE "NAMp-rations")
    set(RATIONS_BUNDLE_DISPLAY_NAME "NAMp Rations")
    set(RATIONS_BUNDLE_IDENTIFIER "com.rations.vst3.namp-rations")
    set(RATIONS_BUNDLE_VERSION "${PROJECT_VERSION}")
    set(RATIONS_BUNDLE_COPYRIGHT
        "MIT licence; NAM DSP core (C) Steven Atkinson")
    configure_file(
        "${RATIONS_ROOT}/packaging/mac/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/mac/Info.plist"
        @ONLY)
    set_target_properties(Rations PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/mac/Info.plist")
endif()

smtg_target_configure_version_file(Rations)

# Title (Michroma) and body (Roboto) fonts, loaded straight out of the bundle by
# FreeType — no system font installation needed.
smtg_target_add_plugin_resources(Rations
    RESOURCES ${RATIONS_FONT_PATHS}
    OUTPUT_SUBDIRECTORY fonts
)

# Panel art. Copied into the bundle rather than embedded, so it can be replaced
# without a rebuild and so the resource resolver finds it the same way in the
# bundle and in the offline render rig.
smtg_target_add_plugin_resources(Rations
    RESOURCES ${RATIONS_IMG_PATHS}
    OUTPUT_SUBDIRECTORY img
)

target_compile_features(Rations PUBLIC cxx_std_17)
target_compile_options(Rations PRIVATE -ffast-math -Wall)
if(RATIONS_MACOS)
    # target_compile_features' cxx_std_17 sets CXX_STANDARD and nothing else, so
    # macplugview.mm would compile at whatever the compiler's default Objective-C++
    # dialect happens to be. It uses C++17 (constexpr, nullptr-typed members,
    # if-with-initialiser), and a toolchain whose default moved would either fail
    # to build it or silently build it under different rules from the rest of the
    # bundle.
    set_target_properties(Rations PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
endif()
target_include_directories(Rations PRIVATE ${RATIONS_ROOT}/src)

target_link_libraries(Rations
    PRIVATE
        sdk
        RationsGfx
        ${RATIONS_PLATFORM_GUI_LIBS}
)
rations_link_whole_dsp(Rations)

# The visibility preset above covers every object we compile; it cannot cover
# the SDK's own archives, which are built in the SDK's scope at default
# visibility and would still put ~800 Steinberg symbols into this bundle's
# dynamic table. --exclude-libs binds every symbol coming out of a static
# archive locally, whatever visibility it was compiled with, which leaves the
# three entry points (compiled into this target, and individually marked
# default) as the bundle's entire public surface. Two plug-ins built against
# different SDK versions in one host is the same interposition hazard as the
# NAM registry, one layer down. --whole-archive still applies: it decides which
# objects are pulled in, and the DSP core's self-registering architectures are
# registered from .init_array, which needs no exported symbol.
#
# ld64 has no --exclude-libs, and does not need one: it takes an explicit
# allow-list instead, which is the stricter of the two forms -- a deny-all on
# archives leaves anything compiled INTO the target exported if its visibility
# says so, while an allow-list names the entire public surface outright. The
# file used is the SDK's own macexport.exp, which holds exactly the three
# entry points macmain.cpp defines (_GetPluginFactory, _bundleEntry,
# _bundleExit), so the mac bundle's exports are the same three the Windows one
# gates on and the Linux one asserts.
if(RATIONS_MACOS)
    target_link_options(Rations PRIVATE
        "-Wl,-exported_symbols_list,${VST3_SDK_DIR}/public.sdk/source/main/macexport.exp")
else()
    target_link_options(Rations PRIVATE -Wl,--exclude-libs,ALL)
endif()

# ---------------------------------------------------------------------------
# panelrender — offline render of each editor page to a PNG, plus an audit of
# every art asset and both fonts. Links RationsGfx only: no X11, no host, no
# VST3 objects, so the graphics stack and the layout can be checked long before
# the editor exists, and a missing asset fails the build gate instead of
# silently degrading to a flat rectangle at run time.
#
# On Windows this is the FIRST gate, and the reason it is worth having: it
# proves cairo, FreeType, PNG decode, NanoSVG and the whole of src/gfx work in
# the cross build before a single line of window code exists.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# rations_offline — headless host for the built bundle. Loads it the way a DAW
# does and renders a signal through the whole chain, which is the only thing
# that proves the BUNDLED bank resolves and the DSP is actually in circuit.
# ---------------------------------------------------------------------------
if(RATIONS_WINDOWS)
    set(RATIONS_MODULE_LOADER
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
elseif(RATIONS_MACOS)
    # Objective-C++: it loads the bundle through CFBundle and reads
    # moduleinfo.json with NSBundle. enable_language(OBJCXX) is done just after
    # project(), and CMake picks the OBJCXX compiler off the .mm extension
    # without any per-target property.
    set(RATIONS_MODULE_LOADER
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
else()
    set(RATIONS_MODULE_LOADER
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()

# Two things module_mac.mm needs that its Linux and Windows siblings do not, both
# taken from the SDK's own validator target rather than guessed
# (public.sdk/samples/vst-hosting/validator/CMakeLists.txt:96-115, 135-141):
#
#   -fobjc-arc  the file is written for Automatic Reference Counting. Without the
#               flag it compiles under manual retain/release, where its ARC-managed
#               NSBundle and NSString locals are simply leaked rather than rejected
#               — a silent miscompilation, not an error.
#   Cocoa       the framework the SDK links for exactly this file when the generator
#               is not Xcode, which is our case.
#
# set_source_files_properties is directory-scoped, so this one call covers every
# tool below that names ${RATIONS_MODULE_LOADER}. RATIONS_MODULE_LOADER_LIBS is
# empty on the other two platforms and disappears from their link lines.
set(RATIONS_MODULE_LOADER_LIBS "")
if(RATIONS_MACOS)
    set_source_files_properties(${RATIONS_MODULE_LOADER} PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    find_library(RATIONS_COCOA_FRAMEWORK Cocoa REQUIRED)
    set(RATIONS_MODULE_LOADER_LIBS ${RATIONS_COCOA_FRAMEWORK})
endif()

add_executable(rations_offline
    ${RATIONS_ROOT}/tools/rations_offline.cpp
    ${RATIONS_MODULE_LOADER}
    # For MemoryStream, which --save-state / --load-state need and which sdk_hosting
    # does not carry. Same reason rations_ircheck names it.
    ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
)
# The nlohmann header is here so the offline proof can read a capture's stated loudness straight
# out of the .nam and assert that Normalized moves the output by exactly the amount that number
# says it should. Header-only, and nothing else of the DSP is linked into this tool.
target_include_directories(rations_offline PRIVATE ${RATIONS_ROOT}/src
    ${NAM_CORE_DIR}/Dependencies/nlohmann)
target_link_libraries(rations_offline PRIVATE sdk_hosting ${RATIONS_MODULE_LOADER_LIBS})
target_compile_features(rations_offline PRIVATE cxx_std_17)
target_compile_options(rations_offline PRIVATE -Wall)
add_dependencies(rations_offline Rations)

# ---------------------------------------------------------------------------
# rations_midicheck - the offline proof for the MIDI learn table and the channel
# switching it drives. Drives the BUILT BUNDLE, because the three routes a
# footswitch's messages take into a VST3 plug-in are not alike and only two of
# them are MIDI-shaped by the time they arrive: a CC and a Program Change come
# in as parameter changes, and only a Note On comes in as an event. Testing the
# table in isolation would test the one part that was never in doubt.
#
# The phase's real gate is the author's own footswitch with the editor closed;
# this covers everything between the pedal and the audio.
# ---------------------------------------------------------------------------
add_executable(rations_midicheck
    ${RATIONS_ROOT}/tools/rations_midicheck.cpp
    ${RATIONS_MODULE_LOADER}
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/eventlist.cpp
    ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
)
target_include_directories(rations_midicheck PRIVATE ${RATIONS_ROOT}/src)
target_link_libraries(rations_midicheck PRIVATE sdk_hosting ${RATIONS_MODULE_LOADER_LIBS})
target_compile_features(rations_midicheck PRIVATE cxx_std_17)
target_compile_options(rations_midicheck PRIVATE -Wall)
add_dependencies(rations_midicheck Rations)

# ---------------------------------------------------------------------------
# rations_ircheck — the offline proof for the cabinet's two IR slots (D10).
# Drives the BUILT BUNDLE, loading IRs through the same IConnectionPoint
# messages the editor sends and turning the blend through the same parameter
# queue a DAW uses, because the two promises being checked are about the
# plug-in's behaviour and not about an expression: that a one-IR user gets
# bit-identical audio whatever the dial does, and that the middle of the dial
# neither bumps nor digs a hole. Which curve achieves the second depends on
# how correlated the two files are, so it is measured rather than argued.
# ---------------------------------------------------------------------------
add_executable(rations_ircheck
    ${RATIONS_ROOT}/tools/rations_ircheck.cpp
    ${RATIONS_MODULE_LOADER}
    # For MemoryStream, which the save/reload check needs and which sdk_hosting
    # does not carry.
    ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
)
target_include_directories(rations_ircheck PRIVATE ${RATIONS_ROOT}/src)
target_link_libraries(rations_ircheck PRIVATE sdk_hosting ${RATIONS_MODULE_LOADER_LIBS})
target_compile_features(rations_ircheck PRIVATE cxx_std_17)
target_compile_options(rations_ircheck PRIVATE -Wall)
add_dependencies(rations_ircheck Rations)

# ---------------------------------------------------------------------------
# rations_fadecheck — the offline proof that the gain crossfade is exact rather
# than merely smooth. Links the DSP core directly: no VST3, no plug-in, no
# host, so what it measures is the arithmetic and nothing around it.
# ---------------------------------------------------------------------------
add_executable(rations_fadecheck
    ${RATIONS_ROOT}/tools/rations_fadecheck.cpp
    ${RATIONS_ROOT}/src/capturesource.cpp
)
target_include_directories(rations_fadecheck PRIVATE ${RATIONS_ROOT}/src)
# --whole-archive for the same reason the plug-in needs it: each network
# architecture registers itself through a file-scope static initialiser, and the
# linker drops those object files from a normal static link because nothing
# references them by name. The symptom is a clean build that fails at run time
# with "No config parser registered for architecture: WaveNet".
rations_link_whole_dsp(rations_fadecheck)
target_compile_features(rations_fadecheck PRIVATE cxx_std_20)
target_compile_options(rations_fadecheck PRIVATE -Wall)

# ---------------------------------------------------------------------------
# rations_jackcheck — the LIVE gate for the channel switch, and the measurement
# that settles kCatchupRatio. Loads the built bundle the way a DAW does and
# drives it from a real JACK process callback, stomping kChannelId through a
# parameter-change queue. Linux only, and only when JACK's development headers
# are present: it is a measurement rig, not part of the plug-in, and its absence
# must not stop the plug-in from building.
# ---------------------------------------------------------------------------
# RATIONS_LINUX rather than NOT RATIONS_WINDOWS, which is what this said while
# there were two platforms and which would now silently offer to build a JACK
# client and an X11 standalone on macOS. JACK does exist there; it is not what a
# mac user has, and standalone/ is X11 and Linux::IRunLoop throughout.
if(RATIONS_LINUX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RATIONS_JACK QUIET jack)
    endif()
    if(RATIONS_JACK_FOUND)
        add_executable(rations_jackcheck
            ${RATIONS_ROOT}/tools/rations_jackcheck.cpp
            ${RATIONS_MODULE_LOADER}
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
            ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
        )
        target_include_directories(rations_jackcheck
            PRIVATE ${RATIONS_ROOT}/src ${RATIONS_JACK_INCLUDE_DIRS})
        target_link_libraries(rations_jackcheck PRIVATE sdk_hosting ${RATIONS_JACK_LIBRARIES})
        target_link_directories(rations_jackcheck PRIVATE ${RATIONS_JACK_LIBRARY_DIRS})
        target_compile_features(rations_jackcheck PRIVATE cxx_std_17)
        target_compile_options(rations_jackcheck PRIVATE -Wall)
        add_dependencies(rations_jackcheck Rations)

        # -------------------------------------------------------------------
        # namp-rations-standalone — the amp without a DAW: one X window, the plug-in's
        # own editor embedded in it, a Linux::IRunLoop the editor registers with,
        # and a JACK client driving the processor. A footswitch reaches it on a
        # JACK MIDI port, and what the session was playing is kept in
        # ~/.config/Rations/standalone.state.
        #
        # It is a HOST: the bundle is loaded as a module rather than linked in, so
        # the editor's dladdr-based resource lookup (src/platform/respath.cpp)
        # resolves Contents/Resources exactly as it does inside a DAW, and there is
        # no second compilation of the plug-in to keep in step with the first.
        # Nothing of src/ is compiled here; the include path is for rationsids.h's
        # parameter IDs alone.
        #
        # It shares the JACK probe above deliberately - one answer to "is JACK
        # here", not two - and, like the gate, its absence must never stop the
        # plug-in from building.
        # -------------------------------------------------------------------
        add_executable(namp-rations-standalone
            ${RATIONS_ROOT}/standalone/main.cpp
            ${RATIONS_ROOT}/standalone/runloop.h
            ${RATIONS_ROOT}/standalone/runloop.cpp
            ${RATIONS_ROOT}/standalone/jackclient.h
            ${RATIONS_ROOT}/standalone/jackclient.cpp
            ${RATIONS_ROOT}/standalone/midiroute.h
            ${RATIONS_ROOT}/standalone/midiroute.cpp
            ${RATIONS_MODULE_LOADER}
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
            # The event list carries Note On to the plug-in - the one of the three
            # footswitch messages that is still MIDI when it arrives. MemoryStream
            # is what the settings file is read and written through. Neither is in
            # sdk_hosting, which is why rations_midicheck names them both too.
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/eventlist.cpp
            ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
            ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
        )
        target_include_directories(namp-rations-standalone
            PRIVATE ${RATIONS_ROOT}/src ${RATIONS_JACK_INCLUDE_DIRS})
        target_link_libraries(namp-rations-standalone
            PRIVATE sdk_hosting ${RATIONS_JACK_LIBRARIES} ${RATIONS_PLATFORM_GUI_LIBS})
        target_link_directories(namp-rations-standalone PRIVATE ${RATIONS_JACK_LIBRARY_DIRS})
        target_compile_features(namp-rations-standalone PRIVATE cxx_std_17)
        target_compile_options(namp-rations-standalone PRIVATE -Wall)
        add_dependencies(namp-rations-standalone Rations)
    else()
        message(STATUS "jack not found - rations_jackcheck (the live switch gate) and "
                       "namp-rations-standalone will not be built")
    endif()
endif()

# ---------------------------------------------------------------------------
# rations_switchcheck — the offline proof that a CHANNEL change is exact rather
# than merely smooth. The crossfade proof next door covers a knob turn, where
# the incoming capture is already running at zero weight before it is needed;
# this one covers a footswitch stomp, where it is not, and where the whole
# design rests on replaying the input ring into a model that has no history at
# all. Drives the shipped ChannelRack, not a paraphrase of it.
# ---------------------------------------------------------------------------
add_executable(rations_switchcheck
    ${RATIONS_ROOT}/tools/rations_switchcheck.cpp
    ${RATIONS_ROOT}/src/capturesource.cpp
    ${RATIONS_ROOT}/src/modelbank.cpp
    ${RATIONS_ROOT}/src/crossfadeengine.cpp
    ${RATIONS_ROOT}/src/channelrack.cpp
    # For utf8ToPath, which modelbank.cpp uses to keep an unrepresentable path
    # from throwing out of the worker thread.
    ${RATIONS_ROOT}/src/platform/respath.cpp
    ${RATIONS_ROOT}/src/gfx/resourcestore.cpp
)
target_include_directories(rations_switchcheck PRIVATE ${RATIONS_ROOT}/src ${VST3_SDK_DIR})
rations_link_whole_dsp(rations_switchcheck)
target_compile_features(rations_switchcheck PRIVATE cxx_std_20)
target_compile_options(rations_switchcheck PRIVATE -Wall)

# ---------------------------------------------------------------------------
# rations_racecheck - the ThreadSanitizer gate for the prime worker.
#
# Background priming is the first genuine concurrent access to the DSP objects
# in this tree, and RULES.md requires the handover to be proved by a sanitizer
# rather than by reading. Built normally it is a useful stress test; built with
# -DRATIONS_ENABLE_TSAN=ON in its own build directory it is the gate.
#
# TSan instruments the whole program, so this cannot share a build directory
# with anything else, and it is deliberately built at -O1 rather than -Ofast:
# TSan needs the accesses it is watching to actually happen.
# ---------------------------------------------------------------------------
option(RATIONS_ENABLE_TSAN "Build rations_racecheck with ThreadSanitizer" OFF)

add_executable(rations_racecheck
    ${RATIONS_ROOT}/tools/rations_racecheck.cpp
    ${RATIONS_ROOT}/src/capturesource.cpp
    ${RATIONS_ROOT}/src/modelbank.cpp
    ${RATIONS_ROOT}/src/crossfadeengine.cpp
    ${RATIONS_ROOT}/src/channelrack.cpp
    ${RATIONS_ROOT}/src/platform/respath.cpp
    ${RATIONS_ROOT}/src/gfx/resourcestore.cpp
)
target_include_directories(rations_racecheck PRIVATE ${RATIONS_ROOT}/src ${VST3_SDK_DIR})
rations_link_whole_dsp(rations_racecheck)
target_compile_features(rations_racecheck PRIVATE cxx_std_20)
target_compile_options(rations_racecheck PRIVATE -Wall)
# TSan is a Linux gate and always was. MinGW ships no ThreadSanitizer runtime,
# so asking for it in a cross build fails at link with an unresolved
# __tsan_init rather than saying what is wrong. Refuse it here instead, and say
# so, rather than letting the option look as though it did something.
if(RATIONS_ENABLE_TSAN AND RATIONS_WINDOWS)
    message(WARNING
        "[Rations] RATIONS_ENABLE_TSAN is ignored for the Windows cross build: MinGW has no "
        "ThreadSanitizer runtime. Run the race gate natively on Linux.")
elseif(RATIONS_ENABLE_TSAN)
    target_compile_options(rations_racecheck PRIVATE -fsanitize=thread -g -O1)
    target_link_options(rations_racecheck PRIVATE -fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# rations_rtcheck — counts allocations on the audio path using the DSP core's
# own malloc/free interception harness. Built without optimisation and with
# assertions live, as that harness requires.
#
# LINUX ONLY, and it has to be. The harness intercepts allocation by resolving
# the real malloc through dlsym(RTLD_NEXT, ...) — see the DSP core's
# tools/test/allocation_tracking.h — which is an ELF/glibc mechanism with no PE
# equivalent, and that file is in a read-only upstream tree we do not patch. So
# the no-allocation-on-the-RT-path gate that RULES.md requires is proved on
# Linux and inherited by the other two: the audio path is the same source, and
# the platform layer adds nothing to it.
#
# macOS is excluded for the same reason Windows is, and had to be named rather
# than left to fall through the old NOT RATIONS_WINDOWS: dlsym(RTLD_NEXT, ...)
# compiles and links on Darwin, so mac would have built this target and run a
# harness that does not intercept anything there -- dyld interposition is
# DYLD_INSERT_LIBRARIES plus a __DATA,__interpose section, which that upstream
# file does not implement. A gate that passes because it measured nothing is
# worse than one that is absent.
# ---------------------------------------------------------------------------
if(RATIONS_LINUX)
add_executable(rations_rtcheck
    ${RATIONS_ROOT}/tools/rations_rtcheck.cpp
    ${RATIONS_ROOT}/src/irblend.cpp
    ${RATIONS_ROOT}/src/midilearn.cpp
    ${RATIONS_ROOT}/src/capturesource.cpp
    ${RATIONS_ROOT}/src/modelbank.cpp
    ${RATIONS_ROOT}/src/crossfadeengine.cpp
    ${RATIONS_ROOT}/src/channelrack.cpp
    ${RATIONS_ROOT}/src/pedals/pedalchain.cpp
    # For utf8ToPath, which modelbank.cpp uses to keep an unrepresentable path
    # from throwing out of the worker thread. Linked in rather than taken from
    # RationsGfx because this target deliberately links no graphics at all — it
    # is measuring allocations on the audio path.
    ${RATIONS_ROOT}/src/platform/respath.cpp
    ${RATIONS_ROOT}/src/gfx/resourcestore.cpp
    ${NAM_CORE_DIR}/tools/test/allocation_tracking.cpp
)
# The SDK is on the include path here for headers only - nothing from it is
# linked and no VST3 object is created. midilearn.h names ParamID, because what
# a learned pedal performs is a parameter, and a counted path that could not see
# that header would be counting a copy of the matcher rather than the one that
# ships.
target_include_directories(rations_rtcheck
    PRIVATE ${RATIONS_ROOT}/src ${NAM_CORE_DIR}/tools ${VST3_SDK_DIR})
rations_link_whole_dsp(rations_rtcheck)
target_compile_features(rations_rtcheck PRIVATE cxx_std_20)
# The interception harness needs real calls to operator new, so the optimiser
# must not be allowed to elide them, and NDEBUG must not disable the checks.
target_compile_options(rations_rtcheck PRIVATE -Wall -O0 -UNDEBUG)
endif()

# ---------------------------------------------------------------------------
# rations_pedalcheck — measures the pedalboard's DSP against the circuit it
# claims to model. Links the pedal sources directly rather than driving the
# bundle: the PRE chain is upstream of the amp models, so anything measured at
# the plug-in's output has four neural captures and a cabinet in front of it.
# ---------------------------------------------------------------------------
add_executable(rations_pedalcheck ${RATIONS_ROOT}/tools/rations_pedalcheck.cpp)
target_include_directories(rations_pedalcheck
    PRIVATE ${RATIONS_ROOT}/src ${RATIONS_ROOT}/deps ${AUDIO_DSP_TOOLS_DIR} ${VST3_SDK_DIR})
target_compile_features(rations_pedalcheck PRIVATE cxx_std_17)
target_compile_options(rations_pedalcheck PRIVATE -Wall -Wextra -O2)

# midilearn.cpp comes along because the settings page's text is not decoration:
# what a binding reads as is what the audit has to measure, so the tool formats
# it with the same function the panel does rather than with a mock string.
add_executable(panelrender ${RATIONS_ROOT}/tools/panelrender.cpp
                           ${RATIONS_ROOT}/src/midilearn.cpp)
target_link_libraries(panelrender PRIVATE RationsGfx)
target_include_directories(panelrender PRIVATE ${VST3_SDK_DIR})
target_compile_options(panelrender PRIVATE -Wall)
