# The shared CMake vocabulary: the functions both products use to build the same amp.
#
# core/ holds the shared SOURCES and defines no targets, and that is forced by three measured
# facts rather than chosen. The two products' graphics libraries do not have the same membership
# (the rack compiles the file browser into its gfx library, rations into the plug-in itself); the
# rack compiles the amp TWICE, once as a static library for its standalone and once for its VST3
# bundle, with different link treatment; and -ffast-math is on for some consumers of these files
# and asserted OFF for others. A single prebuilt core library could satisfy none of the three.
#
# So what is shared is the file lists and the recipes, and each product still owns its own targets
# and their compile options entirely. This file is the recipes.
#
# Included by the root dispatcher, so every function here is defined before either product's
# CMakeLists runs. Functions are evaluated in the CALLER's scope, which is why they may name
# variables they never set: NAM_CORE_DIR, AUDIO_DSP_TOOLS_DIR and EIGEN_DIR come from the
# dependency probes in the product file that calls in here, and NAMP_CORE_DIR, NAMP_PRODUCT_DIR
# and NAMP_CORE_INCLUDES from the dispatcher, which is the one place that knows the repository's
# shape.
#
# Platform is spelled WIN32 and APPLE rather than through either product's own three-way split:
# rations sets RATIONS_WINDOWS / RATIONS_MACOS / RATIONS_LINUX and the rack sets nothing at all, so
# a function that named either product's variables would be a function only that product could call.

# ---------------------------------------------------------------------------
# namp_source(<out> <relative path>)
#
# The include path's rule, made usable by a source list: resolve a repository-relative path against
# this product first and the shared core second -- the same order, for the same reason, as the
# product's own src/ ahead of NAMP_CORE_INCLUDES.
#
# It exists because a source list can hold a path built from a loop variable, and such a path cannot
# be repointed file by file. products/rack builds its offline proofs in two foreach loops, and each
# loop now names some tools that are shared and some that are not; without this, keeping them in one
# loop would mean deciding by hand which is which and writing the answer down twice.
#
# Only the top-level directories that core/ mirrors exactly -- tools/, standalone/, deps/ -- can be
# resolved this way. core/ deliberately does NOT mirror src/: a product's src/foo.h is core/foo.h,
# because core/ IS a source root rather than a directory inside one. Source paths under src/ are
# written out in full and say which tree they mean.
# ---------------------------------------------------------------------------
function(namp_source out rel)
    if(EXISTS "${NAMP_PRODUCT_DIR}/${rel}")
        set(${out} "${NAMP_PRODUCT_DIR}/${rel}" PARENT_SCOPE)
    elseif(EXISTS "${NAMP_CORE_DIR}/${rel}")
        set(${out} "${NAMP_CORE_DIR}/${rel}" PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "namp_source: '${rel}' is in neither products/${NAMP_PRODUCT}/ nor core/.")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# namp_assert_no_fast_math(<target> <why>)
#
# Assert that a target is NOT built with fast math, and say at the failure site what breaks if it
# is. The end-of-chain clamp that stops a hosted plug-in's NaN reaching a speaker is built out of
# an isfinite() test, and -ffinite-math-only gives the compiler permission to assume that test's
# answer and delete it: a target whose whole job is to stop a NaN loses that job silently, with
# nothing failing and nothing to read in a diff. So the flag's ABSENCE is asserted by the build
# rather than left to review, which is what this function is for.
#
# Three places are checked, not one. The three hand-written copies this replaces each read only the
# target's own COMPILE_OPTIONS, which is the narrowest of the three ways the flag can arrive:
#
#   1. the target's own COMPILE_OPTIONS -- target_compile_options() on it, PRIVATE or PUBLIC;
#   2. the DIRECTORY's COMPILE_OPTIONS -- add_compile_options(), which reaches every target in the
#      product file and appears in none of their COMPILE_OPTIONS properties;
#   3. CMAKE_CXX_FLAGS and the per-configuration flags, which is how a flag arrives from the
#      command line or from a toolchain file.
#
# Plus one level of linked targets' INTERFACE_COMPILE_OPTIONS, because a PUBLIC option on a library
# reaches its consumers and is invisible in the consumer's own properties. One level rather than
# the full graph: the targets here link a handful of our own libraries directly, and an unbounded
# walk would report a flag on an imported system target as if it were ours.
# ---------------------------------------------------------------------------
function(namp_assert_no_fast_math target why)
    set(_found "")

    get_target_property(_opts ${target} COMPILE_OPTIONS)
    if(_opts AND _opts MATCHES "ffast-math|ffinite-math-only")
        list(APPEND _found "target_compile_options(${target}): ${_opts}")
    endif()

    get_directory_property(_dir_opts COMPILE_OPTIONS)
    if(_dir_opts AND _dir_opts MATCHES "ffast-math|ffinite-math-only")
        list(APPEND _found "add_compile_options() in this directory: ${_dir_opts}")
    endif()

    foreach(_var CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE} CMAKE_CXX_FLAGS_RELEASE)
        if(${_var} MATCHES "ffast-math|ffinite-math-only")
            list(APPEND _found "${_var}: ${${_var}}")
        endif()
    endforeach()

    get_target_property(_links ${target} LINK_LIBRARIES)
    if(_links)
        foreach(_dep ${_links})
            if(TARGET ${_dep})
                get_target_property(_dep_opts ${_dep} INTERFACE_COMPILE_OPTIONS)
                if(_dep_opts AND _dep_opts MATCHES "ffast-math|ffinite-math-only")
                    list(APPEND _found "${_dep}'s INTERFACE_COMPILE_OPTIONS: ${_dep_opts}")
                endif()
            endif()
        endforeach()
    endif()

    if(_found)
        list(JOIN _found "\n    " _where)
        message(FATAL_ERROR
            "${target} is built with fast math, and it must not be.\n"
            "  ${why}\n"
            "  It arrives from:\n    ${_where}\n"
            "  Whatever added that flag has to exclude this target.")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# namp_core_sources(<out> <set>)
#
# The source lists that are the same in both products, named once. Two sets, and only two, because
# those are the two that genuinely have one membership:
#
#   GFX  the graphics stack and the resource path resolver -- every file both products' graphics
#        libraries agree on. What they do NOT agree on is filebrowser.{h,cpp}: the rack compiles it
#        here because its rack strip opens the same browser to choose a plug-in folder, and rations
#        compiles it into the plug-in instead. That difference is passed to namp_add_gfx() as extra
#        sources rather than hidden behind an option here.
#   DSP  the NAM core, AudioDSPTools and the tone stack -- the compile list of the DSP archive.
#
# The amp's own engine sources -- capturesource, modelbank, crossfadeengine, channelrack, irblend,
# midilearn -- are deliberately NOT a set. Ten targets across the two products name some of them,
# and no two name the same subset: rations_fadecheck takes one file, rations_switchcheck takes four
# plus respath, rations_rtcheck takes six plus the pedal chain. A set with a flag per site is how a
# list stops describing what a target actually compiles, so each of those ten keeps saying so.
# ---------------------------------------------------------------------------
function(namp_core_sources out set)
    if("${set}" STREQUAL "GFX")
        set(${out}
            ${NAMP_CORE_DIR}/gfx/canvas.cpp
            ${NAMP_CORE_DIR}/gfx/fontstack.cpp
            ${NAMP_CORE_DIR}/gfx/image.cpp
            ${NAMP_CORE_DIR}/gfx/svg.cpp
            ${NAMP_CORE_DIR}/gfx/resourcestore.h
            ${NAMP_CORE_DIR}/gfx/resourcestore.cpp
            ${NAMP_CORE_DIR}/platform/respath.cpp
            PARENT_SCOPE)
    elseif("${set}" STREQUAL "DSP")
        # NAM_CORE_DIR, AUDIO_DSP_TOOLS_DIR and EIGEN_DIR are the caller's: the dependency probes
        # that set them run in the product file, and this function is evaluated in its scope.
        set(${out}
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
            PARENT_SCOPE)
    else()
        message(FATAL_ERROR "namp_core_sources: no such set '${set}'. It is GFX or DSP.")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# namp_add_dsp(<archive> <headers> [INCLUDE_DIRS <dir>...])
#
# The NAM DSP core as a static library, plus the INTERFACE library carrying what it takes to
# COMPILE against its headers. TWO targets, and they have to be two.
#
# Everywhere but macOS a consumer names the archive as an ordinary link item and CMake hands over
# its include paths, definitions and language level along with it. The mac arm of
# namp_link_whole_dsp() below cannot do that: ld64 spells whole-archive as a -force_load FLAG
# carrying a path, so the target is never named and every one of those usage requirements is
# silently dropped. The first symptom is a missing NAM/dsp.h, which merely fails to build. The
# second is worse and would fail nothing: without NAM_ENABLE_A2_FAST the A2 fast path is compiled
# out, and that path is the whole basis of the click-free gain crossfade and the instant channel
# switch. So the requirements live on an INTERFACE library both arms can name; it contributes no
# archive of its own, so naming it alongside -force_load cannot put the same objects on the link
# line twice.
#
# INCLUDE_DIRS is for what a product publishes on top. rations passes its own deps/, which is where
# the pedalboard's vendored DSP is reached as "bbm/..." and "wdl/..." so the provenance of each
# block stays visible at every include site; both trees are header-only, so there is nothing to add
# to the source list. The rack passes none, because it has no pedals.
#
# The pair of names is recorded so namp_link_whole_dsp() below needs only the consumer's name:
# there is exactly one DSP archive per configure, and asking every one of its thirteen call sites
# to repeat which one would be thirteen chances to name the wrong one.
# ---------------------------------------------------------------------------
function(namp_add_dsp archive headers)
    cmake_parse_arguments(arg "" "" "INCLUDE_DIRS" ${ARGN})

    namp_core_sources(_dsp_sources DSP)
    add_library(${archive} STATIC ${_dsp_sources})

    add_library(${headers} INTERFACE)
    target_compile_definitions(${headers} INTERFACE NAM_ENABLE_A2_FAST)
    target_include_directories(${headers} INTERFACE
        ${NAM_CORE_DIR}
        ${NAM_CORE_DIR}/NAM
        ${NAM_CORE_DIR}/Dependencies/nlohmann
        ${AUDIO_DSP_TOOLS_DIR}/dsp
        # And its parent, so a header outside that directory can say "dsp/dsp.h" and mean
        # AudioDSPTools' rather than NAM's. Both trees have a dsp.h, NAM's include dir is listed
        # first, and a bare "dsp.h" therefore reaches the wrong one -- with an error about
        # DSP_SAMPLE that says nothing about which of two identically-named headers arrived.
        ${AUDIO_DSP_TOOLS_DIR}
        ${EIGEN_DIR}
        # The tone stack, whose ToneStack.cpp is in the list above. Both products used to name a
        # deps/tonestack beside themselves here; neither directory has existed since the file moved
        # into core/, so both entries were dead and this is the live one.
        ${NAMP_CORE_DIR}/deps/tonestack
        ${arg_INCLUDE_DIRS})

    # C++20: NAM's slimmable wavenet uses std::atomic<std::shared_ptr>, a C++20 library feature.
    # The plug-in and the host stay at the SDK's C++17 baseline.
    target_compile_features(${headers} INTERFACE cxx_std_20)

    # AudioDSPTools' ResamplingContainer.h declares, at class scope,
    #
    #     using LanczosResampler = LanczosResampler<T, NCHANS, A>;
    #
    # which redeclares a name already used in that scope to mean something else.
    # [basic.scope.class]p2 forbade that, and GCC enforces it as a permerror -- an error only
    # -fpermissive downgrades. GCC 14 implemented P1787R6, which deleted the rule, so the same
    # header compiles clean there and the diagnostic is a pre-14 artefact rather than a defect in
    # the code. It is why this tree builds on GCC 14 and stops on GCC 12.
    #
    # INTERFACE because it belongs to the header, not to this library: the AudioDSPTools include
    # path above is published here, so every consumer that includes the resampler meets the same
    # permerror and each would otherwise have to rediscover the flag. Whoever publishes a header
    # publishes what compiling it takes.
    #
    # Version-gated rather than unconditional, so the loosened checking disappears on a compiler
    # that does not need it instead of quietly outliving its reason. The upstream tree is a pinned
    # submodule, so patching the line there would be undone by the next submodule update.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        target_compile_options(${headers} INTERFACE -fpermissive)
    endif()

    # The archive both compiles against those requirements and re-publishes them, so a target that
    # links it the ordinary way needs nothing else.
    target_link_libraries(${archive} PUBLIC ${headers})

    if(WIN32)
        # -fPIC has no meaning for PE and MinGW warns that it is being ignored; code in a Windows
        # DLL is relocated by the loader, not by position-independent codegen.
        target_compile_options(${archive} PRIVATE -ffast-math -Wno-unused-parameter)
    else()
        target_compile_options(${archive} PRIVATE -ffast-math -fPIC -Wno-unused-parameter)
    endif()

    set_property(GLOBAL PROPERTY NAMP_DSP_ARCHIVE ${archive})
    set_property(GLOBAL PROPERTY NAMP_DSP_HEADERS ${headers})
endfunction()

# ---------------------------------------------------------------------------
# namp_link_whole_dsp(<target>)
#
# Link the whole of the DSP archive, whatever the linker is called.
#
# Thirteen targets across the two products need every object out of that archive, not just the ones
# some symbol references: each network architecture -- WaveNet, LSTM, ConvNet, the slimmable
# container -- registers itself through a file-scope static initialiser, and a normal static link
# drops the object files holding them because nothing names them. The symptom is a clean build that
# fails at run time with "No config parser registered for architecture: WaveNet".
#
# GNU ld spells that --whole-archive/--no-whole-archive around the library; Apple's ld64 has
# neither flag and spells it -force_load with the archive's path. -force_load already pulls in every
# object, so the library must NOT also be named as an ordinary link item or ld64 reports every
# symbol twice -- which is why the mac arm uses add_dependencies for the build ordering CMake would
# otherwise infer from the link line.
#
# Not naming the target also loses everything CMake would have carried with it: include paths,
# NAM_ENABLE_A2_FAST and the C++20 level. The mac arm therefore names the INTERFACE library those
# requirements live on; it has no archive, so it cannot reintroduce the double-link this paragraph
# is about. The GNU arm does name the archive target, so the same requirements arrive with it and
# there is nothing further to add.
#
# This is the rations helper, which had the -force_load arm, and it is now the rack's too. The
# rack's own copy was Linux-only, which was correct for a Linux product and is exactly the kind of
# difference that would have had to be discovered rather than inherited the day the rack is built
# on a Mac.
# ---------------------------------------------------------------------------
function(namp_link_whole_dsp target)
    get_property(archive GLOBAL PROPERTY NAMP_DSP_ARCHIVE)
    get_property(headers GLOBAL PROPERTY NAMP_DSP_HEADERS)
    if(NOT archive)
        message(FATAL_ERROR
            "namp_link_whole_dsp(${target}) before namp_add_dsp(): there is no DSP archive yet.")
    endif()
    if(APPLE)
        target_link_libraries(${target} PRIVATE
            ${headers} "-Wl,-force_load,$<TARGET_FILE:${archive}>")
        add_dependencies(${target} ${archive})
    else()
        target_link_libraries(${target}
            PRIVATE -Wl,--whole-archive ${archive} -Wl,--no-whole-archive)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# namp_add_gfx(<target> CAIRO <imported target> [SOURCES <extra>...])
#
# The graphics stack: the canvas, the font stack, the image and SVG loaders, the resource store and
# the resource-path resolver. Every consumer that draws anything links this, and it links nothing
# of the amp, nothing of X11 and nothing of the plug-in -- which is what lets panelrender draw
# every page with no host and no window server.
#
# CAIRO is the imported target from the product's own pkg_check_modules(). It is a parameter rather
# than a fixed name because the two products spell the result differently and because the Windows
# arm of one of them hangs CAIRO_WIN32_STATIC_BUILD on that target: cairo.h declares every entry
# point __declspec(dllimport) without it, even for a static build its own .pc file does not
# advertise, and the symptom is "undefined reference to __imp_cairo_*" at every call. Naming the
# target here would take that with it.
#
# SOURCES is the membership difference that stops core/ being one prebuilt library. The rack
# compiles filebrowser.{h,cpp} in here because its rack strip opens the same browser to choose a
# plug-in folder; rations compiles it into the plug-in and the LV2 modules instead. Passing it as
# sources says which product does what at the site that decides, rather than behind an option.
#
# POSITION_INDEPENDENT_CODE because both products' real consumers are shared objects -- a VST3
# bundle and, on Linux, two LV2 modules -- and the archive is linked into all of them.
# ---------------------------------------------------------------------------
function(namp_add_gfx target)
    cmake_parse_arguments(arg "" "CAIRO" "SOURCES" ${ARGN})
    if(NOT arg_CAIRO)
        message(FATAL_ERROR "namp_add_gfx(${target}): CAIRO <imported target> is required.")
    endif()

    namp_core_sources(_gfx_sources GFX)
    add_library(${target} STATIC ${_gfx_sources} ${arg_SOURCES})
    target_include_directories(${target} PUBLIC ${NAMP_PRODUCT_DIR}/src ${NAMP_CORE_INCLUDES})
    target_link_libraries(${target} PUBLIC ${arg_CAIRO} ${CMAKE_DL_LIBS})
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # -Wall for our own code; the vendored NanoSVG headers are compiled as they are.
    target_compile_options(${target} PRIVATE -Wall)
endfunction()
