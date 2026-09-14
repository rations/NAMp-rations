# NAMp-Rack — an LV2/VST3 plug-in host with a four-channel Neural Amp Modeler amp head built in.
#
# No plug-in or GUI framework: the amp is a raw Steinberg IPlugView painted with Cairo/FreeType,
# and the rack hosts other people's plug-ins around it in one serial chain. The DSP core is the
# MIT-licensed NeuralAmpModelerCore + AudioDSPTools by Steven Atkinson, with Eigen (MPL2) as the
# linear-algebra backend.
#
# PLATFORMS. Linux is the whole product: the standalone host, LV2 and VST3 hosting, JACK audio.
# Windows hosts VST3 only and takes its audio from WASAPI — the LV2 stack (lilv/suil/serd/sord/
# sratom/zix) is Linux-shaped here, so it sits behind its own switch rather than being ifdef'd
# through the source. That split is structural, not a convenience.
#
# BUILD STATE: scaffolding phase. This file resolves and validates every dependency and declares
# the build's shape; the targets themselves arrive with the source in the next phase. Configuring
# it is the check that the tree a fresh clone gets is one the build can actually find its way
# around.

cmake_minimum_required(VERSION 3.25.0)

project(namp-rack
    VERSION 0.1.0
    DESCRIPTION "NAMp-Rack — plug-in host with a four-channel NAM amp head"
    LANGUAGES C CXX
)

set(NAMPRACK_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# The shared amp's include path, appended wherever this product's own src/ is already on it.
#
# ORDER IS THE MECHANISM: the product's src/ stays FIRST and core/ follows, so a header that exists
# in both resolves to this product's copy. That is what lets core/gfx/widgets.h say #include
# "canvas.h" and get the canvas.h belonging to whichever product is being built, with no #ifdef and
# nothing in core/ knowing which product it is in.
#
# The subdirectory entries are not decoration. A quoted #include is resolved against the including
# FILE's own directory first, so core/gfx/canvas.cpp reaching a sibling "resourcestore.h" stopped
# being a sibling lookup the moment the two files landed in different trees. Each entry below is
# the directory that restores exactly one such pair, and no more: the set was derived by resolving
# every quoted include in both products before and after the move and requiring each one to land on
# the same file it landed on before.
#
# There is no ${NAMPRACK_ROOT}/src/* entry here any more, and that is the scaffolding stage 3 put
# up coming down on schedule. It existed to keep a sibling lookup working while a byte-identical
# .cpp sat in core/ and its .h did not; both src/gfx and src/platform emptied out as those pairs
# unified, and the entries went with the directories. products/rations still carries one, because
# it alone has a macplugview.
set(NAMP_CORE_INCLUDES
    ${NAMP_CORE_DIR}
    ${NAMP_CORE_DIR}/platform
    ${NAMP_CORE_DIR}/tools
    ${NAMP_CORE_DIR}/standalone
    ${NAMP_CORE_DIR}/deps/tonestack)


if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# LV2 hosting is Linux-only by construction, so the default follows the platform rather than being
# a preference anyone has to remember to set.
if(UNIX AND NOT APPLE AND NOT WIN32)
    set(_namprack_lv2_default ON)
else()
    set(_namprack_lv2_default OFF)
endif()

option(NAMPRACK_BUILD_LV2_HOST "Host LV2 plug-ins (Linux only)" ${_namprack_lv2_default})
option(NAMPRACK_BUILD_TOOLS    "Build the offline proof and render tools" ON)

# The amp is also built as a VST3 bundle that is NOT part of the shipped product. It exists so the
# SDK validator has something to validate, which is by a wide margin the cheapest gate on the amp;
# the release script does not package it. Turning this off costs that gate and leaves the offline
# proofs as the amp's only check.
option(NAMPRACK_BUILD_VST3_BUNDLE "Build the amp as a VST3 bundle for validator coverage" ON)

# ASIO is the Windows audio backend and ships by default, so this is ON rather than opt-in: it is
# what the hardware vendors' drivers expose and what Windows guitar players actually run, and a
# standalone that can only reach the shared-mode mixer is a worse product than the plug-in this
# project already ships. WASAPI is the RUNTIME fallback for a machine with no vendor ASIO driver,
# never a substitute for shipping ASIO.
#
# THE LICENCE IS A CONDITION ON PUBLISHING, NOT ON BUILDING. The SDK is dual-licensed and this
# project is on the proprietary arm — the GPLv3 arm is closed, because no GPLv3 code may enter this
# include graph. Under that arm the SDK costs nothing and is non-royalty-bearing, but it must never
# be redistributed, which is why it reaches the build only through the untracked dependency sysroot
# and why NOTICE carries the trademark notice the agreement fixes. Turning this OFF produces a
# WASAPI-only standalone that needs no agreement with anyone; leaving it ON needs the countersigned
# agreement before a binary is published, and nothing before a binary is built.
if(WIN32)
    # WAS ASIO ASKED FOR, OR IS IT MERELY THE DEFAULT? The two deserve different answers when the
    # SDK turns out to be missing, and this is the only moment they can still be told apart: a -D on
    # the command line has already defined the variable by the time control reaches here, and the
    # default has not. Recorded once and then left alone, because on a reconfigure of an existing
    # build directory the cache holds a value either way, and reading that as "explicit" would make
    # every second configure lie about what the first one was asked to do.
    if(NOT DEFINED NAMPRACK_WIN_ASIO_EXPLICIT)
        if(DEFINED NAMPRACK_WIN_ASIO)
            set(NAMPRACK_WIN_ASIO_EXPLICIT ON CACHE INTERNAL
                "NAMPRACK_WIN_ASIO was set on the command line, not defaulted")
        else()
            set(NAMPRACK_WIN_ASIO_EXPLICIT OFF CACHE INTERNAL
                "NAMPRACK_WIN_ASIO is on by default, not by request")
        endif()
    endif()
    option(NAMPRACK_WIN_ASIO "Host ASIO drivers in the Windows standalone (Steinberg ASIO SDK)" ON)
else()
    # Declared on every platform so a script may set it without a platform test, and forced off
    # where it cannot mean anything: no header outside the Windows audio backend includes it, and
    # the Linux build must not be able to reach it at all.
    set(NAMPRACK_WIN_ASIO OFF)
endif()

if(NAMPRACK_BUILD_LV2_HOST AND WIN32)
    message(FATAL_ERROR
        "NAMPRACK_BUILD_LV2_HOST is ON but the target is Windows. LV2 hosting needs lilv, suil "
        "and jalv's event buffer, none of which is part of the Windows build; Windows hosts VST3 "
        "only. Configure with -DNAMPRACK_BUILD_LV2_HOST=OFF.")
endif()

# ---------------------------------------------------------------------------
# Dependency source trees.
#
# Each defaults to the in-tree pinned directory and can be pointed at an external checkout
# instead, which is how development against pristine reference trees works without initialising
# submodules. Each defaults to the submodule path at the REPOSITORY ROOT -- one copy shared by
# every product in this tree, at the pins both products already agreed on. These trees are
# read-only: a local edit inside one is silently reverted by the next update and leaves a fresh
# clone unable to reproduce the build.
# ---------------------------------------------------------------------------
set(NAM_CORE_DIR "${NAMP_REPO_ROOT}/NeuralAmpModelerCore"
    CACHE PATH "NeuralAmpModelerCore source tree (v0.5.5 or newer)")
set(AUDIO_DSP_TOOLS_DIR "${NAMP_REPO_ROOT}/AudioDSPTools"
    CACHE PATH "AudioDSPTools source tree")
set(EIGEN_DIR "${NAMP_REPO_ROOT}/eigen"
    CACHE PATH "Eigen source tree (the directory containing Eigen/)")

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
        message(FATAL_ERROR
            "expected dependency header not found: ${_probe}\n"
            "The tree is present but is not the one this build expects. Check the path, and see "
            "the NAM core version requirement below.")
    endif()
endforeach()

# The NAM core must be v0.5.5 or newer. The amp builds submodels directly through the public
# parse_model_config_json()/create_dsp() pair and reads DSP::GetPrewarmSamples(); in v0.5.3 that
# accessor is named PrewarmSamples(), sits in a protected section, and NAM/linear.h,
# DspLoadOptions and ScopedPrewarmOnResetDefault do not exist. Both versions are commonly sitting
# side by side on a development machine, and pointing this at the wrong one produces code that
# reads correctly and does not compile — or, worse, compiles against different semantics.
#
# Probe for what ACTUALLY differs. NAM/model_config.h is NOT a discriminator: it is present in
# v0.5.3 too, which was measured on two v0.5.3 checkouts rather than assumed. The two reliable
# markers are NAM/linear.h and the GetPrewarmSamples rename.
if(NOT EXISTS "${NAM_CORE_DIR}/NAM/linear.h")
    message(FATAL_ERROR
        "NAM_CORE_DIR ('${NAM_CORE_DIR}') has no NAM/linear.h, so it is older than v0.5.5. "
        "This build requires the public parse_model_config_json()/create_dsp() pair, "
        "DspLoadOptions, and a public DSP::GetPrewarmSamples(). Point -DNAM_CORE_DIR at a "
        "v0.5.5-or-newer checkout.")
endif()
file(STRINGS "${NAM_CORE_DIR}/NAM/dsp.h" _namprack_prewarm REGEX "GetPrewarmSamples")
if(NOT _namprack_prewarm)
    message(FATAL_ERROR
        "NAM_CORE_DIR ('${NAM_CORE_DIR}') declares no GetPrewarmSamples() in NAM/dsp.h - it has "
        "the older protected PrewarmSamples() instead, so this is a pre-0.5.5 tree. The "
        "crossfade and channel-switch engines need the prewarm length to prime an incoming "
        "model; without it a channel switch is a click. Point -DNAM_CORE_DIR at a "
        "v0.5.5-or-newer tree.")
endif()

# ---------------------------------------------------------------------------
# VST 3 SDK — used on BOTH sides here, unlike in either parent project: the plug-in-side classes
# build the amp, and the hosting classes (Module, PlugProvider, ParameterChanges, EventList,
# HostClasses) run the rack.
# ---------------------------------------------------------------------------
if(NOT TARGET sdk)
    set(VST3_SDK_DIR "${NAMP_REPO_ROOT}/vst3sdk"
        CACHE PATH "VST 3 SDK source tree")
    if(NOT EXISTS ${VST3_SDK_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "VST 3 SDK not found at '${VST3_SDK_DIR}' - run "
            "'git submodule update --init --recursive' or set -DVST3_SDK_DIR=<path>")
    endif()

    # Headless SDK: core libraries plus the validator (part of the hosting examples), no VSTGUI
    # (the editor is drawn directly), no plug-in examples.
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "editor is drawn directly" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "validator" FORCE)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "plug-in examples not needed" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "run manually instead" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "install manually instead" FORCE)

    # Defaults ON, and its Windows implementation runs a freshly built moduleinfotool.exe as a
    # POST_BUILD step — a Windows binary when cross-compiling, so it cannot execute on the build
    # host. The file is optional (Module::getModuleInfoPath returns an empty optional when it is
    # absent, and the validator does not require it), and the release script generates it under
    # Wine instead, where the same binary does run.
    if(CMAKE_CROSSCOMPILING)
        set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "generated under Wine instead" FORCE)
    endif()

    add_subdirectory(${VST3_SDK_DIR} vst3sdk)

    # The SDK's plugin helper functions read these directory-scoped variables
    # (smtg_target_add_library_main silently adds no module entry point without
    # public_sdk_SOURCE_DIR); the SDK sets them only in its own scope.
    set(public_sdk_SOURCE_DIR "${VST3_SDK_DIR}/public.sdk")
    set(pluginterfaces_SOURCE_DIR "${VST3_SDK_DIR}/pluginterfaces")

    # public.sdk/source/vst/utility/dataexchange.cpp does not compile with MinGW. It reaches
    # alignedalloc.h, which picks its allocator on `#elif defined(_MSC_VER)` -> _aligned_malloc,
    # `#else` -> std::aligned_alloc. MinGW is neither: it defines no _MSC_VER, and its msvcrt has
    # no C11 aligned_alloc (verified — the CRT offers _aligned_malloc and __mingw_aligned_malloc
    # only), so the else branch fails to compile. The test is really "is this the Microsoft CRT",
    # not "is this the Microsoft compiler", which is an upstream bug we must not fix in place: the
    # SDK is a pinned dependency and a local edit would be reverted by the next update, leaving a
    # fresh clone unable to build for Windows.
    #
    # Dropping the file instead is safe because nothing here uses the feature it implements:
    # DataExchangeHandler is the plug-in side of VST 3.7.9's IDataExchangeHandler, and no source
    # in this tree mentions it. vstinitiids.cpp still defines the IIDs, so the interface itself
    # remains declared.
    if(WIN32)
        get_target_property(_sdk_sources sdk SOURCES)
        list(FILTER _sdk_sources EXCLUDE REGEX "utility/dataexchange\\.cpp$")
        set_target_properties(sdk PROPERTIES SOURCES "${_sdk_sources}")
        message(STATUS "[NAMp-Rack] dropped dataexchange.cpp from the SDK "
                       "(unused, and MinGW has no std::aligned_alloc)")

        # The SDK's Windows command-line tools define wmain, not main. MinGW only wires wmain up
        # to the CRT entry point when -municode is on the LINK line; without it the linker falls
        # back to libmingw32's crtexewin.o, whose main() calls WinMain, and the build dies on an
        # undefined WinMain. MSVC needs none of this, which is why upstream does not set it.
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
# smtg_target_make_plugin_package() adds a POST_BUILD step that copies PlugIn.ico and desktop.ini
# into the bundle and then runs "attrib +s" on all three paths. attrib is a cmd.exe built-in, not a
# program, so it cannot exist on a Linux build host and the step fails the build outright. The SDK
# offers no switch for it: SMTG_PACKAGE_ICON_PATH is forced back to the bundled Steinberg logo
# inside smtg_add_vst3plugin_with_pkgname() whenever it does not name an existing file, so it cannot
# be pointed at nothing.
#
# Redefining the function here — before smtg_add_vst3plugin() is called, so this is the definition
# that call resolves to — replaces it without touching the pinned submodule. What is lost is only
# the Explorer folder icon, which is cosmetic, is Steinberg's logo rather than ours, and does not
# survive a ZIP in any case: the +s attribute is not carried by the archive format.
# ---------------------------------------------------------------------------
if(CMAKE_CROSSCOMPILING AND WIN32)
    function(smtg_target_add_folder_icon target icon)
        # Deliberately empty. See above.
    endfunction()
endif()

# ---------------------------------------------------------------------------
# Platform libraries
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

if(NOT WIN32)
    # The graphics stack and JACK are system shared libraries on Linux: linked at run time and not
    # redistributed. See NOTICE.
    pkg_check_modules(NAMPRACK_GFX REQUIRED IMPORTED_TARGET
        cairo freetype2 fontconfig x11)
    pkg_check_modules(NAMPRACK_JACK REQUIRED IMPORTED_TARGET jack)
endif()

# ---------------------------------------------------------------------------
# The ASIO SDK, from the dependency sysroot and from nowhere else.
#
# There is no library to find: the SDK is sources a host compiles into itself, so what is located
# here is an include root and four translation units. NAMPRACK_ASIO_SDK_DIR can point at another
# prefix's copy; the default is the one scripts/build-win-deps.sh installs.
#
# A MISSING SDK IS NEVER A SILENT WASAPI-ONLY BUILD, because falling back quietly would produce a
# standalone that looks finished, runs, and cannot reach the audio interface most of its users own —
# which is the failure this whole decision exists to prevent. What it is depends on who asked:
#
#   * ASKED FOR (-DNAMPRACK_WIN_ASIO=ON) — a configure error. The answer to "build the ASIO backend"
#     is never "here is a different product".
#   * DEFAULTED (nobody said anything) — a loud warning and a demotion to WASAPI-only for this
#     configure. The person has a checkout, no SDK, and no opinion yet; refusing to configure at all
#     tells them to go and accept a third party's download terms before they can compile a line of
#     an MIT project. They are not publishing anything, and the licence is a condition on publishing
#     rather than on building.
#
# The demotion sets a normal variable that shadows the cache entry for the rest of this configure
# and deliberately does NOT rewrite the cache. Writing OFF into the cache would make the fallback
# sticky, so that a later run of the dependency script would install the SDK and this build would go
# on quietly ignoring it — which is the silent WASAPI-only build the paragraph above rules out,
# arriving by a slower route. Left alone, the next configure re-tests and picks ASIO back up by
# itself the moment the SDK is there.
# ---------------------------------------------------------------------------
if(NAMPRACK_WIN_ASIO)
    set(NAMPRACK_ASIO_SDK_DIR "${NAMPRACK_WIN_SYSROOT}/include/asiosdk"
        CACHE PATH "The ASIO SDK's host side, as installed into the dependency sysroot")
    set(NAMPRACK_ASIO_SOURCES
        ${NAMPRACK_ASIO_SDK_DIR}/common/asio.cpp
        ${NAMPRACK_ASIO_SDK_DIR}/host/asiodrivers.cpp
        ${NAMPRACK_ASIO_SDK_DIR}/host/pc/asiolist.cpp)
    set(_asio_missing "")
    foreach(_asio_file
            ${NAMPRACK_ASIO_SDK_DIR}/common/asio.h
            ${NAMPRACK_ASIO_SDK_DIR}/common/asiosys.h
            ${NAMPRACK_ASIO_SDK_DIR}/common/iasiodrv.h
            ${NAMPRACK_ASIO_SDK_DIR}/host/asiodrivers.h
            ${NAMPRACK_ASIO_SDK_DIR}/host/ginclude.h
            ${NAMPRACK_ASIO_SDK_DIR}/host/pc/asiolist.h
            ${NAMPRACK_ASIO_SOURCES})
        if(NOT EXISTS "${_asio_file}")
            list(APPEND _asio_missing "${_asio_file}")
        endif()
    endforeach()
    if(_asio_missing)
        # Name one file rather than all nine. The list is the same nine every time the SDK is simply
        # absent, and a wall of paths buries the sentence that says what to do about it.
        list(GET _asio_missing 0 _asio_first_missing)
        set(_asio_where
            "  Run scripts/build-win-deps.sh, which fetches it into that prefix and verifies what "
            "arrived. It is never committed to this repository — Steinberg's licence forbids "
            "redistributing the SDK — so the sysroot is the only place it can come from.")
        if(NAMPRACK_WIN_ASIO_EXPLICIT)
            message(FATAL_ERROR
                "NAMPRACK_WIN_ASIO is ON but the ASIO SDK is not in the dependency sysroot: "
                "${_asio_first_missing} is missing.\n"
                ${_asio_where} "\n"
                "  To build the Windows standalone WITHOUT ASIO support, which is a WASAPI-only "
                "product and needs no agreement with anyone, configure with "
                "-DNAMPRACK_WIN_ASIO=OFF.")
        endif()
        message(WARNING
            "The ASIO SDK is not in the dependency sysroot (${_asio_first_missing} is missing), so "
            "this configure builds a WASAPI-ONLY standalone.\n"
            "  THIS IS NOT THE SHIPPED PRODUCT. ASIO is what Windows guitar players run and what "
            "the latency depends on; WASAPI is the runtime fallback for a machine with no vendor "
            "driver. A release binary is built with ASIO.\n"
            ${_asio_where} "\n"
            "  Nothing here needs an agreement with anyone: the licence is a condition on "
            "publishing a binary, never on building one. Pass -DNAMPRACK_WIN_ASIO=ON to make a "
            "missing SDK a hard error instead of this warning, or -DNAMPRACK_WIN_ASIO=OFF to ask "
            "for the WASAPI-only build deliberately and silence it.")
        # Shadows the cache entry for this configure only — see the header comment above.
        set(NAMPRACK_WIN_ASIO OFF)
        set(NAMPRACK_ASIO_DEMOTED ON)
    endif()
endif()

# The SDK's bundle loader, which is per-platform. Named once here rather than at each consumer:
# the standalone and the three bundle-driving tools all need it, and picking it in two places is
# how they end up disagreeing.
if(WIN32)
    set(NAMPRACK_MODULE_LOADER_SRC
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
else()
    set(NAMPRACK_MODULE_LOADER_SRC
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()

if(NAMPRACK_BUILD_LV2_HOST)
    # LV2 hosting. lilv reads the RDF world, suil instantiates a plug-in's own UI, and the lv2
    # headers are the vocabulary both speak.
    #
    # The SYSTEM libraries are linked, not the local reference sources, and that is a decision
    # rather than a default. There is real version skew between the two — the reference tree runs
    # ahead of the packaged libraries by four minor versions on lilv — so one had to be picked
    # rather than straddled. Picking the system copy is safe because the delta was MEASURED, not
    # assumed: the two lilv headers declare an identical function set except for
    # lilv_state_get_bundle_path, which is newer-only and is not used here; suil's public header
    # is byte-identical; and every LV2 URI define this code uses is the same in both, the only
    # differences being include-guard renames and the Gtk4UI/Qt6UI additions of a later lv2. The
    # reference sources stay the read-only authority for semantics and for jalv's implementation
    # patterns, in the same role the pristine checkout plays for the VST3 SDK. Re-measure if
    # either side moves.
    #
    # Prefer lilv_file_uri_parse over the deprecated lilv_uri_to_path.
    pkg_check_modules(LV2HOST REQUIRED IMPORTED_TARGET
        lv2>=1.18.0 lilv-0>=0.24.0 suil-0>=0.10.0)
endif()

# ---------------------------------------------------------------------------
# Visibility, and one GCC flag the presets cannot reach
# ---------------------------------------------------------------------------
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# The presets above cannot reach the standard library, and on GCC 12 that is not hypothetical:
# libstdc++ declares `namespace std` with _GLIBCXX_VISIBILITY(default), an explicit attribute, and
# an explicit attribute beats -fvisibility=hidden. So std::__detail::__to_chars_10_impl<T>::__digits
# — a function-local static in an inline template — reaches the dynamic table with STB_GNU_UNIQUE
# binding no matter what the presets say, and the packaging script's unique-symbol gate rejects the
# result.
#
# -fno-gnu-unique makes GCC emit STB_GLOBAL for those statics instead. That removes the binding
# itself rather than hiding one instance of it, which is the property that matters here: a unique
# symbol is resolved through a table dlopen's RTLD_LOCAL does not scope. That is a far sharper
# concern in this project than in the one it was written for, because this binary dlopen's other
# people's plug-ins — two copies of one symbol resolving to each other across that boundary is a
# crash with nothing in the backtrace to explain it.
#
# GCC 13 reworked __to_chars_10_impl and the statics went away, so a newer toolchain never produces
# them; the flag is a no-op there rather than a behaviour change. Clang never emits STB_GNU_UNIQUE
# and does not take the flag.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fno-gnu-unique)
endif()

# ---------------------------------------------------------------------------
# The graphics library: the hand-painted editor's Cairo/FreeType canvas.
# ---------------------------------------------------------------------------
if(WIN32)
    # fontconfig is deliberately absent, and so is cairo-xlib. There is not one Fc* call in this
    # tree — FontStack loads its two TTFs by path, and falls back to the copies linked into the
    # binary — and cairo makes fontconfig optional on Windows, with cairo-ft.h guarding its
    # fontconfig include behind CAIRO_HAS_FC_FONT. The Windows editor composes into the same
    # offscreen image surface the X11 one does and blits it through a DIB section, so cairo needs
    # its image backend, its FreeType font backend and PNG, and nothing else.
    #
    # These come from scripts/build-win-deps.sh; Debian packages no MinGW build of any of them.
    # cmake/toolchain-mingw-w64.cmake points pkg-config at that prefix through PKG_CONFIG_LIBDIR.
    #
    # --static is required, and not merely for completeness. Everything in that sysroot is a static
    # archive, and GNU ld resolves archives left to right in one pass: pkg-config's shared Libs:
    # line orders them "-lz -lpng16", which is backwards for static linking and fails with
    # undefined references to crc32/inflate/deflate out of libpng16.a. The --static line repeats
    # -lz after -lpng16, which is exactly what the one-pass resolver needs.
    set(PKG_CONFIG_ARGN --static)
    pkg_check_modules(CAIROFT REQUIRED IMPORTED_TARGET cairo cairo-ft freetype2)

    # cairo.h declares every entry point __declspec(dllimport) unless this is defined, and cairo's
    # own .pc file does not carry it even for a static build — so without it every cairo call fails
    # to link as "undefined reference to __imp_cairo_*". It belongs on the imported target rather
    # than on ours, because it describes how cairo must be USED, and it has to reach every
    # translation unit that includes a cairo header.
    target_compile_definitions(PkgConfig::CAIROFT INTERFACE CAIRO_WIN32_STATIC_BUILD)

    # What the window layer needs beyond cairo: GDI for the DIB blit, user32 for the window and its
    # messages, msimg32 for AlphaBlend.
    set(NAMPRACK_PLATFORM_GUI_LIBS user32 gdi32 msimg32)
    set(NAMPRACK_PLATFORM_VIEW_SOURCES
        ${NAMP_CORE_DIR}/platform/win32plugview.h
        ${NAMP_CORE_DIR}/platform/win32plugview.cpp)
else()
    pkg_check_modules(CAIROFT REQUIRED IMPORTED_TARGET cairo cairo-ft freetype2 fontconfig)
    pkg_check_modules(CAIROX11 REQUIRED IMPORTED_TARGET cairo-xlib x11)
    set(NAMPRACK_PLATFORM_GUI_LIBS PkgConfig::CAIROX11)
    set(NAMPRACK_PLATFORM_VIEW_SOURCES
        ${NAMP_CORE_DIR}/platform/x11plugview.h
        ${NAMP_CORE_DIR}/platform/x11plugview.cpp)
endif()

add_library(NampRackGfx STATIC
    ${NAMP_CORE_DIR}/gfx/canvas.cpp
    ${NAMP_CORE_DIR}/gfx/fontstack.cpp
    ${NAMP_CORE_DIR}/gfx/image.cpp
    ${NAMP_CORE_DIR}/gfx/svg.cpp
    ${NAMP_CORE_DIR}/gfx/resourcestore.h
    ${NAMP_CORE_DIR}/gfx/resourcestore.cpp
    ${NAMP_CORE_DIR}/platform/respath.cpp
    # The in-plug-in file browser lives here rather than with the plug-in because it is drawn with
    # Canvas and nothing else, and the rack needs the same one to choose a plug-in search folder.
    # Two browsers would drift, and the one the user already knows is the one the rack should open.
    ${NAMP_CORE_DIR}/filebrowser.h
    ${NAMP_CORE_DIR}/filebrowser.cpp
)
target_include_directories(NampRackGfx PUBLIC ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES})
target_link_libraries(NampRackGfx PUBLIC PkgConfig::CAIROFT ${CMAKE_DL_LIBS})
target_compile_features(NampRackGfx PUBLIC cxx_std_17)
set_target_properties(NampRackGfx PROPERTIES POSITION_INDEPENDENT_CODE ON)
# -Wall for our own code; the vendored NanoSVG headers are compiled as-is.
target_compile_options(NampRackGfx PRIVATE -Wall)

# ---------------------------------------------------------------------------
# The DSP: the NAM core, AudioDSPTools and the tone stack.
# ---------------------------------------------------------------------------
add_library(NampRackDsp STATIC
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
    ${NAMP_CORE_DIR}/deps/tonestack/ToneStack.cpp
)

# What it takes to COMPILE against those headers, held on an INTERFACE library separate from the
# archive. Keeping the two apart is what lets a consumer that links the archive with
# --whole-archive still receive the include paths, the definitions and the language level: the
# whole-archive spelling puts a FILE on the link line rather than naming a target, and every usage
# requirement attached to the target would otherwise be silently dropped. The first symptom is a
# missing NAM/dsp.h, which merely fails to build. The second is worse and would fail nothing:
# without NAM_ENABLE_A2_FAST the A2 fast path is compiled out, and that path is the whole basis of
# the click-free gain crossfade and the instant channel switch.
add_library(NampRackDspHeaders INTERFACE)
target_compile_definitions(NampRackDspHeaders INTERFACE NAM_ENABLE_A2_FAST)
target_include_directories(NampRackDspHeaders INTERFACE
    ${NAM_CORE_DIR}
    ${NAM_CORE_DIR}/NAM
    ${NAM_CORE_DIR}/Dependencies/nlohmann
    ${AUDIO_DSP_TOOLS_DIR}/dsp
    # And its parent, so a header outside that directory can say "dsp/dsp.h" and mean
    # AudioDSPTools' rather than NAM's. Both trees have a dsp.h and NAM's include dir is listed
    # first, so a bare "dsp.h" reaches the wrong one — with an error about DSP_SAMPLE that says
    # nothing about which of two identically-named headers arrived.
    ${AUDIO_DSP_TOOLS_DIR}
    ${EIGEN_DIR}
    ${NAMPRACK_ROOT}/deps/tonestack
)

# C++20: NAM's slimmable wavenet uses std::atomic<std::shared_ptr>, a C++20 library feature. The
# plug-in and the host stay at the SDK's C++17 baseline.
target_compile_features(NampRackDspHeaders INTERFACE cxx_std_20)

# AudioDSPTools' ResamplingContainer.h declares, at class scope,
#
#     using LanczosResampler = LanczosResampler<T, NCHANS, A>;
#
# which redeclares a name already used in that scope to mean something else. [basic.scope.class]p2
# forbade that, and GCC enforces it as a permerror — an error only -fpermissive downgrades. GCC 14
# implemented P1787R6, which deleted the rule, so the same header compiles clean there and the
# diagnostic is a pre-14 artefact rather than a defect in the code.
#
# INTERFACE because it belongs to the header, not to this library: the AudioDSPTools include path
# above is published here, so every consumer that includes the resampler meets the same permerror
# and each would otherwise have to rediscover the flag. Whoever publishes a header publishes what
# compiling it takes.
#
# Version-gated rather than unconditional, so the loosened checking disappears on a compiler that
# does not need it instead of quietly outliving its reason. The upstream tree is pinned, so
# patching the line there would be undone by the next update.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    target_compile_options(NampRackDspHeaders INTERFACE -fpermissive)
endif()

target_link_libraries(NampRackDsp PUBLIC NampRackDspHeaders)
if(WIN32)
    # -fPIC has no meaning for PE and MinGW warns that it is being ignored; code in a Windows DLL
    # is relocated by the loader, not by position-independent codegen.
    target_compile_options(NampRackDsp PRIVATE -ffast-math -Wno-unused-parameter)
else()
    target_compile_options(NampRackDsp PRIVATE -ffast-math -fPIC -Wno-unused-parameter)
endif()

# Link the DSP archive WHOLE. Each network architecture registers itself through a file-scope
# static initialiser, and the linker drops those object files from a normal static link because
# nothing references them by name. The symptom is a clean build that fails at run time with
# "No config parser registered for architecture: WaveNet".
function(namprack_link_whole_dsp target)
    target_link_libraries(${target}
        PRIVATE -Wl,--whole-archive NampRackDsp -Wl,--no-whole-archive)
    target_link_libraries(${target} PRIVATE NampRackDspHeaders)
endfunction()

# ---------------------------------------------------------------------------
# The amp, as a VST3 bundle.
#
# NOT A SHIPPED PRODUCT. The product is the standalone host; this target exists so the SDK
# validator has something to validate, which is by a wide margin the cheapest gate on the amp, and
# the release script does not package it. See NAMPRACK_BUILD_VST3_BUNDLE.
# ---------------------------------------------------------------------------
set(NAMPRACK_PLUGIN_SOURCES
    src/rationsids.h
    src/version.h
    src/geometry.h
    src/rationsprocessor.h
    src/rationsprocessor.cpp
    src/rationscontroller.h
    src/rationscontroller.cpp
    # The state blob's string reads, shared by the two functions above that decode one.
    ${NAMP_CORE_DIR}/statestream.h
    src/rationsfactory.cpp
    ${NAMP_CORE_DIR}/engineconfig.h
    ${NAMP_CORE_DIR}/spscqueue.h
    ${NAMP_CORE_DIR}/bank.h
    ${NAMP_CORE_DIR}/nativeresampler.h
    ${NAMP_CORE_DIR}/capturesource.h
    ${NAMP_CORE_DIR}/capturesource.cpp
    ${NAMP_CORE_DIR}/modelbank.h
    ${NAMP_CORE_DIR}/modelbank.cpp
    ${NAMP_CORE_DIR}/crossfadeengine.h
    ${NAMP_CORE_DIR}/crossfadeengine.cpp
    ${NAMP_CORE_DIR}/channelrack.h
    ${NAMP_CORE_DIR}/channelrack.cpp
    ${NAMP_CORE_DIR}/irblend.h
    ${NAMP_CORE_DIR}/irblend.cpp
    src/midilearn.h
    ${NAMP_CORE_DIR}/midilearn.cpp
    src/rationsview.h
    src/rationsview.cpp
    ${NAMP_CORE_DIR}/platform/plugview.h
    ${NAMPRACK_PLATFORM_VIEW_SOURCES}
)

# ---------------------------------------------------------------------------
# Art and fonts, named once.
#
# ONE list, three consumers: the validator bundle copies these into Contents/Resources, the embed
# generator turns the same files into the standalone's built-in fallback set, and panelrender's
# audit fails if one is missing. Naming them once is what keeps a layer that is drawn from ever
# being absent from the binary that draws it.
#
# The *-base.png files in the same directory are the untrimmed source exports that gui/ trims these
# out of. They are not listed because they are not shipped.
# ---------------------------------------------------------------------------
set(NAMPRACK_FONT_FILES
    fonts/Michroma-Regular.ttf
    fonts/Roboto-Regular.ttf
)
set(NAMPRACK_IMG_FILES
    img/head.png
    img/cabinet.png
    img/namp-badge.png
    img/dial.png
    img/led_on.png
    img/led_off.png
    img/switch_up_ring.png
    img/switch_down_ring.png
    img/meter_track.png
    img/Folder.svg
    img/File.svg
    img/Cross.svg
    img/ArrowLeft.svg
    img/ArrowRight.svg
    img/SlimmableIcon.svg
    # Not drawn by the amp's editor. It is the per-node settings icon in the rack strip, which is
    # a later phase; it ships now because it is a committed asset of this project and leaving it
    # out would make landing the rack a packaging change as well as a code one.
    img/Gear.svg
)
set(NAMPRACK_RESOURCE_FILES ${NAMPRACK_FONT_FILES} ${NAMPRACK_IMG_FILES})

set(NAMPRACK_FONT_PATHS "")
foreach(_res IN LISTS NAMPRACK_FONT_FILES)
    list(APPEND NAMPRACK_FONT_PATHS "${NAMPRACK_ROOT}/resources/${_res}")
endforeach()
set(NAMPRACK_IMG_PATHS "")
foreach(_res IN LISTS NAMPRACK_IMG_FILES)
    list(APPEND NAMPRACK_IMG_PATHS "${NAMPRACK_ROOT}/resources/${_res}")
endforeach()

# ---------------------------------------------------------------------------
# The amp's own code, as a library.
#
# It is a library rather than a source list belonging to the bundle because the standalone links it
# directly. The alternative — having the standalone dlopen an INSTALLED bundle, which is what the
# amp's own parent standalone did — makes a self-contained product impossible: nothing in ~/.vst3,
# nothing to run. Linking the same sources into both targets means the standalone is one file and
# can never load a DIFFERENT, older amp than the one it was just built from.
#
# Compiled twice rather than shared as objects on purpose: the bundle and the standalone want
# different link-time treatment (the bundle re-exports three entry points and hides everything
# else), and these are a few thousand lines.
# ---------------------------------------------------------------------------
add_library(NampRackPlugin STATIC ${NAMPRACK_PLUGIN_SOURCES})
target_include_directories(NampRackPlugin PUBLIC ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES})
target_link_libraries(NampRackPlugin PUBLIC sdk NampRackGfx ${NAMPRACK_PLATFORM_GUI_LIBS})
target_compile_features(NampRackPlugin PUBLIC cxx_std_17)
target_compile_options(NampRackPlugin PRIVATE -ffast-math -Wall)
set_target_properties(NampRackPlugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
namprack_link_whole_dsp(NampRackPlugin)

if(NAMPRACK_BUILD_VST3_BUNDLE)
    smtg_add_vst3plugin(NampRackAmp PACKAGE_NAME "NAMp-Rack-Amp"
                        SOURCES_LIST ${NAMPRACK_PLUGIN_SOURCES})
    smtg_target_configure_version_file(NampRackAmp)
    target_compile_features(NampRackAmp PUBLIC cxx_std_17)
    target_compile_options(NampRackAmp PRIVATE -ffast-math -Wall)
    target_include_directories(NampRackAmp PRIVATE ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES})
    namprack_link_whole_dsp(NampRackAmp)
    target_link_libraries(NampRackAmp PRIVATE sdk NampRackGfx ${NAMPRACK_PLATFORM_GUI_LIBS})

    # The bundle carries its art in Contents/Resources rather than embedded, so it can be replaced
    # without a rebuild and so respath.cpp resolves it the same way here and in the offline render
    # rig. The standalone has no bundle to look in and carries the same files inside itself.
    smtg_target_add_plugin_resources(NampRackAmp
        RESOURCES ${NAMPRACK_FONT_PATHS}
        OUTPUT_SUBDIRECTORY fonts
    )
    smtg_target_add_plugin_resources(NampRackAmp
        RESOURCES ${NAMPRACK_IMG_PATHS}
        OUTPUT_SUBDIRECTORY img
    )
endif()

# ---------------------------------------------------------------------------
# NampRackResources — the art and fonts as byte arrays, for a binary with no resource directory
# beside it.
#
# ONLY THE STANDALONE LINKS THIS. The plug-in must not: it already carries the same files in
# Contents/Resources, where a user can replace them, and linking both would put two copies of every
# layer in one bundle. See core/gfx/resourcestore.h for the disk-first lookup order that makes the
# built-in set a fallback rather than a lock-in.
#
# The generated file depends on the resource files themselves, so replacing a layer rebuilds it and
# nothing else.
# ---------------------------------------------------------------------------
set(NAMPRACK_EMBED_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/namprackembeddedresources.cpp")
add_custom_command(
    OUTPUT "${NAMPRACK_EMBED_CPP}"
    COMMAND ${CMAKE_COMMAND}
            "-DNAMPRACK_EMBED_ROOT=${NAMPRACK_ROOT}/resources"
            "-DNAMPRACK_EMBED_FILES=${NAMPRACK_RESOURCE_FILES}"
            "-DNAMPRACK_EMBED_OUTPUT=${NAMPRACK_EMBED_CPP}"
            -P "${NAMPRACK_ROOT}/cmake/embedresources.cmake"
    DEPENDS ${NAMPRACK_FONT_PATHS} ${NAMPRACK_IMG_PATHS}
            "${NAMPRACK_ROOT}/cmake/embedresources.cmake"
    COMMENT "Embedding the resource directory into the standalone"
    VERBATIM
)
add_library(NampRackResources STATIC "${NAMPRACK_EMBED_CPP}")
# NampRackGfx for the include path and for installEmbeddedResources(), which the generated file
# calls.
target_link_libraries(NampRackResources PUBLIC NampRackGfx)
target_compile_features(NampRackResources PRIVATE cxx_std_17)
set_target_properties(NampRackResources PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# NampRackJalv — jalv's event-buffer helper, vendored under deps/jalv (ISC).
#
# It is the one piece of jalv that is a library and is not shipped as one, so it is copied rather
# than depended on. Compiled as C, never reformatted; deps/jalv/namp_jalv_compat.h records the
# lines that had to change and why, and NOTICE carries the licence.
# ---------------------------------------------------------------------------
if(NAMPRACK_BUILD_LV2_HOST)
    add_library(NampRackJalv STATIC ${NAMPRACK_ROOT}/deps/jalv/lv2_evbuf.c)
    target_include_directories(NampRackJalv PUBLIC ${NAMPRACK_ROOT}/deps/jalv)
    target_link_libraries(NampRackJalv PUBLIC PkgConfig::LV2HOST)
    set_target_properties(NampRackJalv PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
endif()

# ---------------------------------------------------------------------------
# NampRackHost — the format-agnostic plug-in host: backends, scanning and the chain engine.
# Everything needed to run OTHER people's plug-ins around the amp.
#
# LINKS NO X11, NO CAIRO AND NO JACK, deliberately. A hosted editor's parent window travels through
# this layer as an opaque uintptr_t and its IPlugView as a void*, so nothing here needs Xlib. That
# is what lets namp_hostcheck link this library and nothing else, which in turn is what keeps the
# claim honest instead of merely intended.
#
# THE LV2 SPLIT IS A BUILD-GRAPH FACT, NOT AN INTERFACE ONE. LV2 hosting is lilv + suil + jalv's
# event buffer, none of which is part of the Windows build; VST3 hosting, the chain engine, the
# catalogue and the out-of-process scanner build everywhere. PluginBackend is format-agnostic and
# needs no change for this — the two guarded call sites are createBackend() in chainbuilder.cpp and
# the LV2 pass of Catalog::rescan(). PluginFormat::Lv2 itself stays in the enum on every platform,
# so a rack saved where LV2 IS hosted comes back as a named placeholder that writes itself out
# again verbatim, rather than as a node silently dropped from the middle of someone's chain.
#
# hostiids.cpp is deliberately NOT part of this library — see the note at the top of that file.
# It is listed by each host EXECUTABLE instead.
# ---------------------------------------------------------------------------
set(NAMPRACK_HOST_SOURCES
    ${NAMPRACK_ROOT}/src/host/format.h
    ${NAMPRACK_ROOT}/src/host/pluginref.h
    ${NAMPRACK_ROOT}/src/host/pluginbackend.h
    ${NAMPRACK_ROOT}/src/host/inampfileloader.h
    ${NAMPRACK_ROOT}/src/host/rtdenormal.h
    ${NAMPRACK_ROOT}/src/host/hostapp.h
    ${NAMPRACK_ROOT}/src/host/hostapp.cpp
    ${NAMPRACK_ROOT}/src/host/diagnostics.h
    ${NAMPRACK_ROOT}/src/host/diagnostics.cpp
    ${NAMPRACK_ROOT}/src/host/spscbytequeue.h
    ${NAMPRACK_ROOT}/src/host/backend_vst3.h
    ${NAMPRACK_ROOT}/src/host/backend_vst3.cpp
    ${NAMPRACK_ROOT}/src/host/chainmodel.h
    ${NAMPRACK_ROOT}/src/host/chainengine.h
    ${NAMPRACK_ROOT}/src/host/chainengine.cpp
    ${NAMPRACK_ROOT}/src/host/chainbuilder.h
    ${NAMPRACK_ROOT}/src/host/chainbuilder.cpp
    ${NAMPRACK_ROOT}/src/host/rackpreset.h
    ${NAMPRACK_ROOT}/src/host/rackpreset.cpp
    ${NAMPRACK_ROOT}/src/host/scancache.h
    ${NAMPRACK_ROOT}/src/host/scancache.cpp
    ${NAMPRACK_ROOT}/src/host/pluginpaths.h
    ${NAMPRACK_ROOT}/src/host/pluginpaths.cpp
    ${NAMPRACK_ROOT}/src/host/catalog.h
    ${NAMPRACK_ROOT}/src/host/catalog.cpp
    ${NAMPRACK_ROOT}/src/host/scanchild.h
    ${NAMPRACK_ROOT}/src/host/scanchild.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/processdata.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/eventlist.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
    ${NAMPRACK_MODULE_LOADER_SRC}
)

if(NAMPRACK_BUILD_LV2_HOST)
    list(APPEND NAMPRACK_HOST_SOURCES
        ${NAMPRACK_ROOT}/src/host/lv2urid.h
        ${NAMPRACK_ROOT}/src/host/lv2urid.cpp
        ${NAMPRACK_ROOT}/src/host/lv2worker.h
        ${NAMPRACK_ROOT}/src/host/lv2worker.cpp
        ${NAMPRACK_ROOT}/src/host/lv2world.h
        ${NAMPRACK_ROOT}/src/host/lv2world.cpp
        ${NAMPRACK_ROOT}/src/host/backend_lv2.h
        ${NAMPRACK_ROOT}/src/host/backend_lv2.cpp
    )
endif()

add_library(NampRackHost STATIC ${NAMPRACK_HOST_SOURCES})
target_include_directories(NampRackHost PUBLIC ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES} ${NAMPRACK_ROOT}/src/host)
target_link_libraries(NampRackHost PUBLIC sdk_hosting ${CMAKE_DL_LIBS})
target_compile_features(NampRackHost PUBLIC cxx_std_17)
target_compile_options(NampRackHost PRIVATE -Wall)
set_target_properties(NampRackHost PROPERTIES POSITION_INDEPENDENT_CODE ON)

# PUBLIC, not PRIVATE: the guarded declarations are in headers this library exports, so a consumer
# that sees a different value than the library was built with would disagree with it about which
# functions exist.
if(NAMPRACK_BUILD_LV2_HOST)
    target_compile_definitions(NampRackHost PUBLIC NAMPRACK_HAVE_LV2=1)
    target_link_libraries(NampRackHost PUBLIC NampRackJalv PkgConfig::LV2HOST)
else()
    target_compile_definitions(NampRackHost PUBLIC NAMPRACK_HAVE_LV2=0)
endif()

# NO -ffast-math ON THIS TARGET, and the absence is asserted rather than left to review.
# clampChainOutput() in chainengine.cpp is built out of an std::isfinite() test, and
# -ffinite-math-only gives the compiler permission to assume that test's answer and delete it — at
# which point a hosted plug-in emitting NaN reaches the speakers and nothing in the tree says so.
get_target_property(_namprack_host_flags NampRackHost COMPILE_OPTIONS)
if(_namprack_host_flags MATCHES "ffast-math|ffinite-math-only")
    message(FATAL_ERROR
        "NampRackHost carries ${_namprack_host_flags}. The end-of-chain NaN clamp in "
        "chainengine.cpp is an std::isfinite() test, and -ffinite-math-only lets the compiler "
        "delete it. Whatever added that flag has to exclude this target.")
endif()

# ---------------------------------------------------------------------------
# NampRackRack — the rack strip's model and its drawing. Links NampRackGfx (cairo/freetype) and
# NampRackHost (the chain it displays) and NOTHING ELSE: no X11, no JACK, no windowing. That is
# what lets rackrender drive the whole interface offline with no plug-in installed, and it is the
# same separation the amp's own editor already has between what draws and what embeds.
# ---------------------------------------------------------------------------
add_library(NampRackRack STATIC
    ${NAMPRACK_ROOT}/src/rack/genericpanel.h
    ${NAMPRACK_ROOT}/src/rack/genericpanel.cpp
    ${NAMPRACK_ROOT}/src/rack/rackgeometry.h
    ${NAMPRACK_ROOT}/src/rack/rackmodel.h
    ${NAMPRACK_ROOT}/src/rack/rackmodel.cpp
    ${NAMPRACK_ROOT}/src/rack/rackview.h
    ${NAMPRACK_ROOT}/src/rack/rackview.cpp
)
target_include_directories(NampRackRack PUBLIC ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES})
target_link_libraries(NampRackRack PUBLIC NampRackGfx NampRackHost)
target_compile_features(NampRackRack PUBLIC cxx_std_17)
set_target_properties(NampRackRack PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(NampRackRack PRIVATE -Wall)

# ---------------------------------------------------------------------------
# NampRackWinAudio — the Windows audio backends and MIDI input.
#
# ITS OWN LIBRARY, AND BUILT BEFORE THE WINDOWS STANDALONE EXISTS TO LINK IT. Two reasons. The first
# is that a file in no target is a file that rots: these compile against two SDKs and one of them is
# not even in the repository, so they need to be built by every Windows configure rather than
# discovered to be broken when the standalone is finally wired up. The second is the same one that
# keeps NampRackHost free of X11 — a library that links no windowing and no VST3 plug-in code cannot
# quietly grow a dependency on either.
#
# NO -ffast-math ON THIS TARGET, and the absence is asserted rather than reviewed, exactly as it is
# on NampRackHost and for the same reason one layer further down. pcmsamples.h's float-to-integer
# conversion is the LAST place a NaN can be stopped before a float becomes a voltage, and it stops one
# by testing positively for the in-range case; -ffinite-math-only tells the compiler NaN cannot
# happen, which licenses it to treat that fallthrough as unreachable. The amp's own library IS built
# with -ffast-math, which is why this has to be a separate target rather than a few more files in it.
# ---------------------------------------------------------------------------
if(WIN32)
    set(NAMPRACK_WIN_AUDIO_SOURCES
        ${NAMPRACK_ROOT}/standalone/audiobackend.h
        ${NAMPRACK_ROOT}/standalone/pcmsamples.h
        ${NAMPRACK_ROOT}/standalone/winmmmidi.h
        ${NAMPRACK_ROOT}/standalone/winmmmidi.cpp
        # WASAPI is the fallback and is ALWAYS built: it is what a machine with no vendor ASIO driver
        # gets, so there is no configuration in which the Windows standalone can do without it.
        ${NAMPRACK_ROOT}/standalone/wasapibackend.h
        ${NAMPRACK_ROOT}/standalone/wasapibackend.cpp
    )
    if(NAMPRACK_WIN_ASIO)
        list(APPEND NAMPRACK_WIN_AUDIO_SOURCES
            ${NAMPRACK_ROOT}/standalone/asiobackend.h
            ${NAMPRACK_ROOT}/standalone/asiobackend.cpp
            # The SDK's own host side, compiled from the sysroot. Never reformatted and never
            # patched — it built clean under MinGW as it stands, which was measured rather than
            # assumed before anything was written against it.
            ${NAMPRACK_ASIO_SOURCES})
    endif()

    add_library(NampRackWinAudio STATIC ${NAMPRACK_WIN_AUDIO_SOURCES})
    target_include_directories(NampRackWinAudio
        PUBLIC ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES} ${NAMPRACK_ROOT}/standalone)
    # winmm for the MIDI input, ole32 for the COM the audio endpoints are, avrt for the audio
    # thread's scheduling class, ksuser for the KSDATAFORMAT subtype GUIDs a WAVEFORMATEXTENSIBLE
    # names, uuid for the interface IIDs __uuidof resolves to.
    target_link_libraries(NampRackWinAudio
        PUBLIC NampRackHost sdk_hosting winmm ole32 avrt ksuser uuid)
    target_compile_features(NampRackWinAudio PUBLIC cxx_std_17)
    # -Wall on our files only; the SDK's sources are upstream text compiled as they are.
    target_compile_options(NampRackWinAudio PRIVATE -Wall)

    if(NAMPRACK_WIN_ASIO)
        target_compile_definitions(NampRackWinAudio PUBLIC NAMPRACK_HAVE_ASIO=1)
        # Two kinds of include, so two kinds of path. OUR files name the SDK's headers as
        # "asiosdk/common/asio.h", which says where they came from and is a reminder at every use
        # site that this tree does not contain them — that needs the directory ABOVE the SDK's own.
        # The SDK's own sources include each other by bare name across its three directories, so
        # those are on the path too: host/asiodrivers.h includes "asiolist.h", which is in host/pc.
        get_filename_component(NAMPRACK_ASIO_SDK_PARENT "${NAMPRACK_ASIO_SDK_DIR}" DIRECTORY)
        target_include_directories(NampRackWinAudio PRIVATE
            ${NAMPRACK_ASIO_SDK_PARENT}
            ${NAMPRACK_ASIO_SDK_DIR}
            ${NAMPRACK_ASIO_SDK_DIR}/common
            ${NAMPRACK_ASIO_SDK_DIR}/host
            ${NAMPRACK_ASIO_SDK_DIR}/host/pc)
    else()
        target_compile_definitions(NampRackWinAudio PUBLIC NAMPRACK_HAVE_ASIO=0)
    endif()

    get_target_property(_namprack_winaudio_flags NampRackWinAudio COMPILE_OPTIONS)
    if(_namprack_winaudio_flags MATCHES "ffast-math|ffinite-math-only")
        message(FATAL_ERROR
            "NampRackWinAudio carries ${_namprack_winaudio_flags}. pcmsamples.h's conversion is "
            "what stops a NaN from becoming a full-scale square wave at the device, and "
            "-ffinite-math-only lets the compiler delete the test that does it. Whatever added that "
            "flag has to exclude this target.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tools: the offline proofs and the panel render.
# ---------------------------------------------------------------------------
if(NAMPRACK_BUILD_TOOLS)
    set(NAMPRACK_MODULE_LOADER ${NAMPRACK_MODULE_LOADER_SRC})

    # The three tools that drive the BUILT BUNDLE through the SDK's own hosting classes, rather
    # than linking the DSP directly. That is the point of them: they answer questions about the
    # product a host loads, not about the sources.
    foreach(_tool rations_offline rations_ircheck rations_midicheck)
        namp_source(_tool_src tools/${_tool}.cpp)
        add_executable(${_tool}
            ${_tool_src}
            ${NAMPRACK_MODULE_LOADER}
            # For MemoryStream, which the state round-trips need and which sdk_hosting does not
            # carry.
            ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
            ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
        )
        # The nlohmann header is here so the offline proof can read a capture's stated loudness
        # straight out of the .nam and assert that Normalized moves the output by exactly the
        # amount that number says it should. Header-only; nothing else of the DSP is linked in.
        target_include_directories(${_tool} PRIVATE ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES}
            ${NAM_CORE_DIR}/Dependencies/nlohmann)
        target_link_libraries(${_tool} PRIVATE sdk_hosting)
        target_compile_features(${_tool} PRIVATE cxx_std_17)
        target_compile_options(${_tool} PRIVATE -Wall)
        if(NAMPRACK_BUILD_VST3_BUNDLE)
            add_dependencies(${_tool} NampRackAmp)
        endif()
    endforeach()

    # The crossfade proof: one bank, one dial, and the convergence assertion.
    add_executable(rations_fadecheck
        ${NAMPRACK_ROOT}/tools/rations_fadecheck.cpp
        ${NAMP_CORE_DIR}/capturesource.cpp
    )
    target_include_directories(rations_fadecheck PRIVATE ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES})
    namprack_link_whole_dsp(rations_fadecheck)
    target_compile_features(rations_fadecheck PRIVATE cxx_std_20)
    target_compile_options(rations_fadecheck PRIVATE -Wall)

    # The channel-switch proof and the race gate: the same engine sources, driven differently.
    foreach(_tool rations_switchcheck rations_racecheck)
        namp_source(_tool_src tools/${_tool}.cpp)
        add_executable(${_tool}
            ${_tool_src}
            ${NAMP_CORE_DIR}/capturesource.cpp
            ${NAMP_CORE_DIR}/modelbank.cpp
            ${NAMP_CORE_DIR}/crossfadeengine.cpp
            ${NAMP_CORE_DIR}/channelrack.cpp
            # For utf8ToPath, which modelbank.cpp uses to keep an unrepresentable path from
            # throwing out of the worker thread.
            ${NAMP_CORE_DIR}/platform/respath.cpp
            ${NAMP_CORE_DIR}/gfx/resourcestore.cpp
        )
        target_include_directories(${_tool} PRIVATE ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES} ${VST3_SDK_DIR})
        namprack_link_whole_dsp(${_tool})
        target_compile_features(${_tool} PRIVATE cxx_std_20)
        target_compile_options(${_tool} PRIVATE -Wall)
    endforeach()


    # The allocation gate. Deliberately links NO graphics at all — it is measuring allocations on
    # the audio path, and a Cairo dependency would put an allocator on the link line that has no
    # business being counted.
    #
    # NOT BUILT FOR WINDOWS, and unlike namp_chaincheck there is nothing left of it if the harness is
    # dropped: counting allocations is its entire job. The harness interposes malloc through
    # dlsym(RTLD_NEXT) and its header includes <dlfcn.h>, and MinGW has neither, so this is a
    # development-machine gate in the same sense ThreadSanitizer and live JACK are. The amp's audio
    # path is platform-independent — the same channelrack, the same crossfade, the same chunk loop —
    # so what this measures on Linux is what runs on Windows.
    if(NOT WIN32)
    add_executable(rations_rtcheck
        ${NAMPRACK_ROOT}/tools/rations_rtcheck.cpp
        ${NAMP_CORE_DIR}/irblend.cpp
        ${NAMP_CORE_DIR}/midilearn.cpp
        ${NAMP_CORE_DIR}/capturesource.cpp
        ${NAMP_CORE_DIR}/modelbank.cpp
        ${NAMP_CORE_DIR}/crossfadeengine.cpp
        ${NAMP_CORE_DIR}/channelrack.cpp
        ${NAMP_CORE_DIR}/platform/respath.cpp
        ${NAMP_CORE_DIR}/gfx/resourcestore.cpp
        ${NAM_CORE_DIR}/tools/test/allocation_tracking.cpp
    )
    # The SDK is on the include path for HEADERS only — nothing from it is linked and no VST3
    # object is created. midilearn.h names ParamID, because what a learned row performs is a
    # parameter, and a counted path that could not see that header would be counting a copy of the
    # matcher rather than the one that ships.
    target_include_directories(rations_rtcheck
        PRIVATE ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES} ${NAM_CORE_DIR}/tools ${VST3_SDK_DIR})
    namprack_link_whole_dsp(rations_rtcheck)
    target_compile_features(rations_rtcheck PRIVATE cxx_std_20)
    # -O0 and -UNDEBUG, and both are load-bearing rather than cautious. The harness works by
    # replacing global operator new, so it can only count calls that are actually MADE — and C++14
    # explicitly permits the optimiser to elide a new/delete pair it can prove is redundant, which
    # at -O3 is exactly what happens to the harness's own self-test. The result is a checker that
    # reports zero allocations because it can no longer see any, which is the most dangerous
    # possible failure for a gate: it looks like a pass. -UNDEBUG keeps the asserts that would
    # otherwise be compiled out of the very code being measured.
    #
    # This costs nothing that matters. The tool is measuring WHETHER an allocation happens, not how
    # fast the audio path is, and the DSP it drives is in NampRackDsp and is still built at -O3.
    target_compile_options(rations_rtcheck PRIVATE -Wall -O0 -UNDEBUG)
    endif()

    # ---------------------------------------------------------------------------
    # namp_audiocheck — the gate on the Windows audio path's arithmetic, run on LINUX.
    #
    # Built on every platform on purpose. The sample-format conversion the Windows backends do is
    # pure arithmetic in a header that includes nothing of ASIO's, and no driver exists on the
    # development machine to test it against — so the only way to check it before it reaches a user
    # is to check it here. It links nothing at all: one header and the standard library, so a failure
    # is the arithmetic and cannot be anything else.
    # ---------------------------------------------------------------------------
    add_executable(namp_audiocheck ${NAMPRACK_ROOT}/tools/namp_audiocheck.cpp)
    target_include_directories(namp_audiocheck PRIVATE ${NAMPRACK_ROOT}/standalone)
    target_compile_features(namp_audiocheck PRIVATE cxx_std_17)
    target_compile_options(namp_audiocheck PRIVATE -Wall -Wextra)
    # Same rule as NampRackWinAudio, and here it is the gate itself that would be defeated: with
    # -ffinite-math-only the compiler may assume the NaN case away in the very code under test.
    get_target_property(_namprack_audiocheck_flags namp_audiocheck COMPILE_OPTIONS)
    if(_namprack_audiocheck_flags MATCHES "ffast-math|ffinite-math-only")
        message(FATAL_ERROR "namp_audiocheck must not be built with fast math: it is the gate on "
                            "what happens to a NaN.")
    endif()

    # The offline panel render: every page drawn without a host, which is the cheapest UI
    # regression test in the tree and the gate on every layout change.
    add_executable(panelrender
        ${NAMPRACK_ROOT}/tools/panelrender.cpp
        ${NAMP_CORE_DIR}/midilearn.cpp)
    target_link_libraries(panelrender PRIVATE NampRackGfx)
    target_include_directories(panelrender PRIVATE ${VST3_SDK_DIR})
    target_compile_options(panelrender PRIVATE -Wall)

    # ---------------------------------------------------------------------------
    # rackrender — the rack's offline render, the counterpart to panelrender. It builds a chain out
    # of nothing (a ChainBuilder with no engine and no plug-ins loaded), drives the real RackView
    # through it, and writes a PNG per view. That is the phase gate for the rack UI: the interface
    # can be judged, and its layout regression-checked, with no X server, no JACK and no plug-in
    # installed.
    # ---------------------------------------------------------------------------
    add_executable(rackrender
        ${NAMPRACK_ROOT}/tools/rackrender.cpp
        ${NAMPRACK_ROOT}/src/host/hostiids.cpp
        ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
    )
    target_link_libraries(rackrender PRIVATE NampRackRack)
    target_compile_features(rackrender PRIVATE cxx_std_17)
    target_compile_options(rackrender PRIVATE -Wall)

    # ---------------------------------------------------------------------------
    # namp_hostcheck — the host phase gate. Links NampRackHost and NOTHING ELSE, which is not a
    # convenience: it is what keeps that library honest about carrying no X11, no Cairo and no
    # JACK. A dependency that crept in would fail to link here rather than being discovered in a
    # DAW.
    #
    # It calls runScanChildIfRequested() like every host binary here, so it probes bundles out of
    # process by re-execing itself. That makes it a gate on the isolation path too, and not only on
    # the catalogue.
    # ---------------------------------------------------------------------------
    add_executable(namp_hostcheck
        ${NAMPRACK_ROOT}/tools/namp_hostcheck.cpp
        ${NAMPRACK_ROOT}/src/host/hostiids.cpp
        ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
    )
    target_link_libraries(namp_hostcheck PRIVATE NampRackHost)
    target_compile_features(namp_hostcheck PRIVATE cxx_std_17)
    target_compile_options(namp_hostcheck PRIVATE -Wall)

    # ---------------------------------------------------------------------------
    # namp_chaincheck — the chain phase gate. --routing checks the compiled routing numerically
    # against synthetic backends, so it needs no plug-in installed, no window server and no audio
    # device; --rt counts allocations through a chain of real VST3 plug-ins using the DSP core's
    # own malloc interception.
    #
    # allocation_tracking.cpp is compiled in directly rather than linked: it overrides the global
    # allocator, which only means anything in a final binary.
    # ---------------------------------------------------------------------------
    #
    # allocation_tracking.cpp is left OUT of the ThreadSanitizer build. Both it and the sanitizer
    # interpose the process allocator, and the pair segfaults during startup with nothing printed —
    # measured, not guessed. The tool compiles either way and says which build it is; see the note
    # at the top of namp_chaincheck.cpp.
    #
    # It is left out of the WINDOWS build for a different reason with the same consequence: the
    # harness interposes malloc through dlsym(RTLD_NEXT) and its header includes <dlfcn.h>, and MinGW
    # has neither. The allocation gate is a development-machine gate, beside ThreadSanitizer and live
    # JACK; what the Windows build of this tool is for is --routing and the preset round-trip, which
    # is a Windows gate in its own right. The tool says which build it is either way.
    set(NAMPRACK_CHAINCHECK_SRC
        ${NAMPRACK_ROOT}/tools/namp_chaincheck.cpp
        ${NAMPRACK_ROOT}/src/host/hostiids.cpp
        ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
    )
    if(NOT NAMPRACK_ENABLE_TSAN AND NOT WIN32)
        list(APPEND NAMPRACK_CHAINCHECK_SRC ${NAM_CORE_DIR}/tools/test/allocation_tracking.cpp)
    endif()
    add_executable(namp_chaincheck ${NAMPRACK_CHAINCHECK_SRC})
    target_link_libraries(namp_chaincheck PRIVATE NampRackHost)
    target_include_directories(namp_chaincheck PRIVATE ${NAM_CORE_DIR}/tools)
    target_compile_features(namp_chaincheck PRIVATE cxx_std_17)
    # -O0 for the same reason rations_rtcheck needs it: C++14 onwards lets the optimiser elide a
    # new/delete pair outright, which would silently disarm the deliberate allocation this tool
    # makes to prove its own harness is armed. Only this file is affected — NampRackHost stays
    # optimised, and the interception is at the malloc symbol, so what is measured is the release
    # build of the chain.
    target_compile_options(namp_chaincheck PRIVATE -Wall -O0 -UNDEBUG)

    # ThreadSanitizer is a Linux gate and always was. MinGW ships no TSan runtime, so asking for
    # it in a cross build fails at link with an unresolved __tsan_init rather than saying what is
    # wrong. Refuse it here instead, and say so, rather than letting the option look as though it
    # did something.
    option(NAMPRACK_ENABLE_TSAN "Build the race gate under ThreadSanitizer" OFF)
    if(NAMPRACK_ENABLE_TSAN AND WIN32)
        message(WARNING
            "[NAMp-Rack] NAMPRACK_ENABLE_TSAN is ignored for the Windows cross build: MinGW has "
            "no ThreadSanitizer runtime. Run the race gate natively on Linux.")
    elseif(NAMPRACK_ENABLE_TSAN)
        # Two race gates, not one. rations_racecheck is the AMP's — the channel-switch handshake
        # between the audio thread and the priming worker. namp_chaincheck is the HOST's: an LV2
        # plug-in's worker extension runs on a thread of ours, and a retired chain snapshot crosses
        # from the audio thread to the builder through a lock-free queue. Different code, different
        # threads, and a run of one says nothing about the other.
        #
        # The whole of NampRackHost is instrumented rather than just the tool, because the races
        # worth finding are inside the library. A partly-instrumented binary reports what it can
        # see, which is the shape of a clean run for the wrong reason.
        foreach(_tsan rations_racecheck namp_chaincheck NampRackHost)
            target_compile_options(${_tsan} PRIVATE -fsanitize=thread -g -O1)
        endforeach()
        foreach(_tsan rations_racecheck namp_chaincheck)
            target_link_options(${_tsan} PRIVATE -fsanitize=thread)
        endforeach()
        target_compile_definitions(namp_chaincheck PRIVATE NAMPRACK_ALLOC_HARNESS=0)
    endif()
endif()

# ---------------------------------------------------------------------------
# namp-rack — the product. The amp without a DAW: one window, its own editor embedded in it, a run
# loop the editor registers with, and an audio backend driving the processor.
#
# ONE FILE, NO INSTALLATION. The amp is linked in (NampRackPlugin) and its art and fonts are linked
# in (NampRackResources), so this runs on a machine with nothing installed and can never silently
# run an older installed build than the one just made. A bundle path can still be given on the
# command line to drive an external build deliberately.
#
# BUILT ON BOTH PLATFORMS FROM ONE main.cpp, and that is the point of the three seams below it.
# Everything platform-specific is behind eventloop.h (the run loop), nativewindow.h (windows) and
# nativeaudio.h (the device); main.cpp includes those three and names no platform at all. Duplicating
# it per platform was the alternative and was rejected: its platform-neutral half is the chain
# builder, the preset handling, the UI tick and the feedback pump, and two copies would mean
# implementing every later feature twice and having one of them go stale.
#
# NOT BUILT WITHOUT AN AUDIO BACKEND, and that is a hard dependency rather than a degradation: the
# standalone with no audio is the editor and nothing else, which is what panelrender already covers
# offline and covers better. On Linux that backend is JACK; on Windows it is ASIO and WASAPI, which
# are always present because they are the operating system's.
# ---------------------------------------------------------------------------
set(NAMPRACK_STANDALONE_SOURCES
    ${NAMPRACK_ROOT}/standalone/main.cpp
    # The host-side interface IIDs, exactly once per final binary — see hostiids.cpp.
    ${NAMPRACK_ROOT}/src/host/hostiids.cpp
    ${NAMPRACK_ROOT}/standalone/audiobackend.h
    ${NAMPRACK_ROOT}/standalone/nativeaudio.h
    # Which device the user chose, and the file it survives a restart in.
    ${NAMPRACK_ROOT}/standalone/audioprefs.h
    ${NAMPRACK_ROOT}/standalone/audioprefs.cpp
    # The run loop and the windows, each a seam over a platform pair. Only one half of each pair is
    # compiled; both halves are built by this machine's own two configures, which is what keeps the
    # platform nobody is looking at today from rotting.
    ${NAMPRACK_ROOT}/standalone/eventloop.h
    ${NAMPRACK_ROOT}/standalone/windowevent.h
    ${NAMPRACK_ROOT}/standalone/nativewindow.h
    ${NAMPRACK_ROOT}/standalone/editorframe.h
    ${NAMPRACK_ROOT}/standalone/editorframe.cpp
    # The rack strip's window and the two kinds of window a hosted plug-in's editor can need:
    # its own, or the generic parameter panel when it has no editor this host can show.
    ${NAMPRACK_ROOT}/standalone/plugframe.h
    ${NAMPRACK_ROOT}/standalone/plugframe.cpp
    ${NAMPRACK_ROOT}/standalone/rackwindow.h
    ${NAMPRACK_ROOT}/standalone/rackwindow.cpp
    ${NAMPRACK_ROOT}/standalone/pluginwindow.h
    ${NAMPRACK_ROOT}/standalone/pluginwindow.cpp
    ${NAMPRACK_ROOT}/standalone/panelwindow.h
    ${NAMPRACK_ROOT}/standalone/panelwindow.cpp
    ${NAMP_CORE_DIR}/standalone/midiroute.h
    ${NAMPRACK_ROOT}/standalone/midiroute.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${NAMPRACK_MODULE_LOADER_SRC}
    ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
)
if(WIN32)
    list(APPEND NAMPRACK_STANDALONE_SOURCES
        ${NAMPRACK_ROOT}/standalone/win32eventloop.h
        ${NAMPRACK_ROOT}/standalone/win32eventloop.cpp
        ${NAMPRACK_ROOT}/standalone/win32window.h
        ${NAMPRACK_ROOT}/standalone/win32window.cpp
        # Which of the two Windows backends drives the device, decided at run time.
        ${NAMPRACK_ROOT}/standalone/winaudio.h
        ${NAMPRACK_ROOT}/standalone/winaudio.cpp)
else()
    list(APPEND NAMPRACK_STANDALONE_SOURCES
        ${NAMPRACK_ROOT}/standalone/x11eventloop.h
        ${NAMPRACK_ROOT}/standalone/x11eventloop.cpp
        ${NAMPRACK_ROOT}/standalone/x11window.h
        ${NAMPRACK_ROOT}/standalone/x11window.cpp
        ${NAMPRACK_ROOT}/standalone/jackclient.h
        ${NAMPRACK_ROOT}/standalone/jackclient.cpp)
endif()

# usediids.cpp stays in the list above and nowhere else: it defines the SDK's interface IIDs and must
# appear exactly once in a final binary. The plug-in-side classes come in through NampRackPlugin,
# which does not carry them.
add_executable(namp-rack ${NAMPRACK_STANDALONE_SOURCES})
target_include_directories(namp-rack
    PRIVATE ${NAMPRACK_ROOT}/src ${NAMP_CORE_INCLUDES} ${NAMPRACK_ROOT}/standalone)
target_link_libraries(namp-rack
    PRIVATE
        NampRackPlugin
        NampRackResources
        NampRackRack
        NampRackHost
        sdk_hosting
        ${NAMPRACK_PLATFORM_GUI_LIBS}
)
if(WIN32)
    # NampRackWinAudio carries both backends, WinMM MIDI and the PCM conversion, and brings its own
    # winmm/ole32/avrt/ksuser/uuid with it.
    target_link_libraries(namp-rack PRIVATE NampRackWinAudio)
else()
    target_link_libraries(namp-rack PRIVATE PkgConfig::NAMPRACK_JACK)
endif()
target_compile_features(namp-rack PRIVATE cxx_std_17)
target_compile_options(namp-rack PRIVATE -Wall)

if(NOT WIN32)
    # Same reasoning as the bundle's --exclude-libs, one layer up. This binary contains the NAM DSP
    # core and the SDK's plug-in-side classes, and it will dlopen OTHER people's plug-ins —
    # including other NAM-based ones. Anything it exported would interpose on theirs, and two
    # copies of a Steinberg symbol in one process is a crash nobody can debug. It exports nothing,
    # so bind every static archive's symbols locally and keep the dynamic table empty.
    #
    # --whole-archive still applies where it is asked for: it decides which OBJECTS are pulled in,
    # and the DSP core's self-registering architectures are registered from .init_array, which
    # needs no exported symbol.
    #
    # WINDOWS NEEDS NO COUNTERPART, and the difference is real rather than an omission. ELF resolves
    # symbols through one process-wide namespace, which is what makes interposition possible at all;
    # PE binds each module to a named DLL at load time, so a plug-in cannot accidentally resolve a
    # Steinberg symbol against this executable. An .exe also publishes no import library for one to
    # bind to even deliberately.
    target_link_options(namp-rack PRIVATE -Wl,--exclude-libs,ALL)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "[NAMp-Rack] ${PROJECT_VERSION} — ${CMAKE_BUILD_TYPE}")
message(STATUS "[NAMp-Rack]   VST3 SDK        ${VST3_SDK_DIR}")
message(STATUS "[NAMp-Rack]   NAM core        ${NAM_CORE_DIR}")
message(STATUS "[NAMp-Rack]   AudioDSPTools   ${AUDIO_DSP_TOOLS_DIR}")
message(STATUS "[NAMp-Rack]   Eigen           ${EIGEN_DIR}")
if(NAMPRACK_BUILD_LV2_HOST)
    message(STATUS "[NAMp-Rack]   hosting         VST3 + LV2 (lv2 ${LV2HOST_lv2_VERSION}, "
                   "lilv ${LV2HOST_lilv-0_VERSION}, suil ${LV2HOST_suil-0_VERSION})")
else()
    message(STATUS "[NAMp-Rack]   hosting         VST3 only")
endif()
if(WIN32)
    if(NAMPRACK_WIN_ASIO)
        message(STATUS "[NAMp-Rack]   audio           ASIO + WASAPI fallback, WinMM MIDI")
    elseif(NAMPRACK_ASIO_DEMOTED)
        # Distinguished from the line below on purpose: one of these was asked for and the other is
        # a checkout with no SDK in it. Reporting both as "NAMPRACK_WIN_ASIO=OFF" would send anyone
        # reading the log looking for a switch nobody set.
        message(STATUS "[NAMp-Rack]   audio           WASAPI only (no ASIO SDK in the sysroot), "
                       "WinMM MIDI")
    else()
        message(STATUS "[NAMp-Rack]   audio           WASAPI only (NAMPRACK_WIN_ASIO=OFF), "
                       "WinMM MIDI")
    endif()
else()
    message(STATUS "[NAMp-Rack]   audio           JACK")
endif()
message(STATUS "[NAMp-Rack]   validator bundle ${NAMPRACK_BUILD_VST3_BUNDLE} (never packaged)")
message(STATUS "[NAMp-Rack]   tools           ${NAMPRACK_BUILD_TOOLS}")
message(STATUS "[NAMp-Rack] amp + rack + standalone + tools.")
message(STATUS "")
