# Rations — a four-channel Neural Amp Modeler amp head, a raw VST3 plug-in for Linux and Windows
# (no framework).
#
# The DSP core is the MIT-licensed NeuralAmpModelerCore + AudioDSPTools by Steven Atkinson, with
# Eigen (MPL2) as the linear-algebra backend. The editor is a Steinberg IPlugView embedded into
# the host's own window and painted with Cairo/FreeType — no plug-in or GUI framework.
#
# TWO products on Linux, ONE on Windows. The VST3 bundle is the plug-in and is all Windows ships;
# Linux adds rations-standalone, which hosts that same bundle on JACK so the amp can be played
# without a DAW. What is still absent, and is the real distinction from the parent NAMp, is the
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

project(smtg-rations
    VERSION 0.1.0
    DESCRIPTION "Rations four-channel neural amp head VST3"
)

set(RATIONS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

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
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------
# Platform split. One product on both platforms; the only OS-specific code is
# the single window class in src/platform, which arrives with the editor.
# ---------------------------------------------------------------------------
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
else()
    set(RATIONS_WINDOWS OFF)
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

target_compile_definitions(RationsDsp PUBLIC NAM_ENABLE_A2_FAST)

target_include_directories(RationsDsp PUBLIC
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
target_compile_features(RationsDsp PUBLIC cxx_std_20)
# -fPIC has no meaning for PE and MinGW warns that it is being ignored; code in
# a Windows DLL is relocated by the loader, not by position-independent codegen.
if(RATIONS_WINDOWS)
    target_compile_options(RationsDsp PRIVATE -ffast-math -Wno-unused-parameter)
else()
    target_compile_options(RationsDsp PRIVATE -ffast-math -fPIC -Wno-unused-parameter)
endif()

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
if(WIN32)
    set(RATIONS_PLATFORM_VIEW_SOURCES
        src/platform/win32plugview.h
        src/platform/win32plugview.cpp
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
# LINKS the plug-in's sources; rations-standalone LOADS the built bundle instead
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
    src/rationsfactory.cpp
    src/rationsview.h
    src/rationsview.cpp
    src/filebrowser.h
    src/filebrowser.cpp
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
    src/pedals/pedal.h
    src/pedals/boost.h
    src/pedals/chorus.h
    src/pedals/flanger.h
    src/pedals/delay.h
    src/pedals/reverb.h
    src/pedals/pedalchain.h
    src/pedals/pedalchain.cpp
    src/platform/plugview.h
    ${RATIONS_PLATFORM_VIEW_SOURCES}
)

smtg_add_vst3plugin(Rations ${RATIONS_PLUGIN_SOURCES})

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
# Rations.vst3/Contents/x86_64-win/Rations.vst3, which is what the SUFFIX/PREFIX
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
target_include_directories(Rations PRIVATE ${RATIONS_ROOT}/src)

target_link_libraries(Rations
    PRIVATE
        sdk
        RationsGfx
        ${RATIONS_PLATFORM_GUI_LIBS}
        -Wl,--whole-archive RationsDsp -Wl,--no-whole-archive
)

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
target_link_options(Rations PRIVATE -Wl,--exclude-libs,ALL)

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
else()
    set(RATIONS_MODULE_LOADER
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
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
target_link_libraries(rations_offline PRIVATE sdk_hosting)
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
target_link_libraries(rations_midicheck PRIVATE sdk_hosting)
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
target_link_libraries(rations_ircheck PRIVATE sdk_hosting)
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
target_link_libraries(rations_fadecheck
    PRIVATE -Wl,--whole-archive RationsDsp -Wl,--no-whole-archive)
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
if(NOT RATIONS_WINDOWS)
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
        # rations-standalone — the amp without a DAW: one X window, the plug-in's
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
        add_executable(rations-standalone
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
        target_include_directories(rations-standalone
            PRIVATE ${RATIONS_ROOT}/src ${RATIONS_JACK_INCLUDE_DIRS})
        target_link_libraries(rations-standalone
            PRIVATE sdk_hosting ${RATIONS_JACK_LIBRARIES} ${RATIONS_PLATFORM_GUI_LIBS})
        target_link_directories(rations-standalone PRIVATE ${RATIONS_JACK_LIBRARY_DIRS})
        target_compile_features(rations-standalone PRIVATE cxx_std_17)
        target_compile_options(rations-standalone PRIVATE -Wall)
        add_dependencies(rations-standalone Rations)
    else()
        message(STATUS "jack not found - rations_jackcheck (the live switch gate) and "
                       "rations-standalone will not be built")
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
target_link_libraries(rations_switchcheck
    PRIVATE -Wl,--whole-archive RationsDsp -Wl,--no-whole-archive)
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
target_link_libraries(rations_racecheck
    PRIVATE -Wl,--whole-archive RationsDsp -Wl,--no-whole-archive)
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
# Linux and inherited by Windows: the audio path is the same source, and the
# platform layer adds nothing to it.
# ---------------------------------------------------------------------------
if(NOT RATIONS_WINDOWS)
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
target_link_libraries(rations_rtcheck
    PRIVATE -Wl,--whole-archive RationsDsp -Wl,--no-whole-archive)
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
