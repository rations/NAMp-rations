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
# variables a product sets for itself -- NAM_CORE_DIR, AUDIO_DSP_TOOLS_DIR, EIGEN_DIR and
# NAMP_CORE_INCLUDES all come from the dependency probes in the product file that calls in here.
#
# Platform is spelled WIN32 and APPLE rather than through either product's own three-way split:
# rations sets RATIONS_WINDOWS / RATIONS_MACOS / RATIONS_LINUX and the rack sets nothing at all, so
# a function that named either product's variables would be a function only that product could call.

# ---------------------------------------------------------------------------
# namp_source(<out> <relative path>)
#
# The include path's rule, made usable by a source list: resolve a repository-relative path against
# this product first and the shared core second -- the same order, for the same reason, as
# NAMP_CORE_INCLUDES in each product's own file.
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
# is. RULES.md's clamp rule asks for exactly this and asks for it to be enforced by the build
# rather than by review: -ffinite-math-only gives the compiler permission to assume the answer to
# an isfinite() test and delete it, so a target whose whole job is to stop a NaN loses that job
# silently, with nothing failing and nothing to read in a diff.
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
