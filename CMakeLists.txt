# NAMp — two products in one tree, built one at a time.
#
#   products/rations   NAMp Rations: the four-channel amp head as a VST3 bundle, an LV2 bundle,
#                      and a minimal JACK standalone that hosts its own plug-in.
#   products/rack      NAMp Rack: a VST3/LV2 plug-in HOST with the same four-channel amp built in
#                      as the chain anchor, and no pedalboard, because the rack hosts pedals
#                      instead of containing them.
#
# They are the same amp. The rack began as a copy of the other tree and the two were kept in step
# by hand for twenty-six commits, which failed silently at least once — see the Output Mode
# default, fixed in one tree months before it reached the other. Putting them in one repository is
# what makes an amp fix land once instead of twice.
#
# ONE PRODUCT PER BUILD DIRECTORY, and that is structural rather than a convenience.
# smtg_add_vst3plugin writes its bundle to ${CMAKE_BINARY_DIR}/VST3/Release, so two products
# configured into one binary directory would write two bundles to one path. Selecting here makes
# that collision unrepresentable rather than something a later check has to catch:
#
#   cmake -S . -B build                          # rations, the default
#   cmake -S . -B build-rack -DNAMP_PRODUCT=rack
#
# The default is deliberate: every script, CI path and habit that predates this file says
# "cmake -S . -B build" and means the amp, and all of them keep working unchanged.
#
# This file is a DISPATCHER and holds no build logic of its own. Each product's CMakeLists.txt is
# still the whole of its build, unchanged in every respect but where it looks for its own root —
# which both trees already routed through a single variable (RATIONS_ROOT, NAMPRACK_ROOT), set in
# exactly one place each, with no use of CMAKE_SOURCE_DIR anywhere. That is why this costs five
# lines per product rather than a rewrite.
#
# add_subdirectory() rather than include(), which was tried first and does not work. A top-level
# file must contain a LITERAL project() call; without one CMake invents project(Project) on line 1
# and detects a compiler THERE, ahead of the CMAKE_OSX_* variables below, which is exactly the
# ordering those variables exist to prevent. Hoisting them here and calling project() here fixes
# that -- and then the product file has to run as a subdirectory anyway, because under include()
# its relative source paths would resolve against this directory instead of its own.

cmake_minimum_required(VERSION 3.25.0)

set(NAMP_PRODUCT "rations" CACHE STRING "Which product to build: rations or rack")
set_property(CACHE NAMP_PRODUCT PROPERTY STRINGS rations rack)

# The repository root, which is where the four shared dependency submodules live. Each product
# resolves them from here rather than from beside itself: there is one vst3sdk, one
# NeuralAmpModelerCore, one AudioDSPTools and one eigen in this tree, at the pins both trees
# already agreed on, and a second copy of any of them is a way for the two products to differ.
set(NAMP_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# The shared amp. Sources only -- core/ defines no targets and is not a subdirectory of the build:
# each product compiles these files into targets of its own, with its own compile options. That is
# forced by three measured facts rather than chosen: the two products' gfx libraries do not have the
# same membership, the rack compiles the amp twice with different link treatment, and -ffast-math is
# on for some consumers and asserted OFF for others. A single prebuilt core library could satisfy
# none of the three.
#
# A product's own src/ comes FIRST on the include path and core/ second, so a header that exists in
# both resolves to the product's. That is how "which product am I in" stays an include-path fact
# instead of an #ifdef: core/gfx/widgets.h includes "canvas.h" and gets whichever canvas.h belongs
# to the product being built.
set(NAMP_CORE_DIR "${NAMP_REPO_ROOT}/core")

# The shared amp's include path, appended wherever a product's own src/ is already on it. Both
# products had their own copy of this list until stage 6 and the two were byte-identical, which is
# the shape a thing takes just before it drifts.
#
# It belongs at the dispatcher because it is a fact about the repository's shape and not about
# either product: every entry is built from NAMP_CORE_DIR, which is set three lines up, and the
# value does not depend on which product is being configured. It is not in cmake/nampcore.cmake
# because that file defines functions and nothing else.
#
# The subdirectory entries are not decoration. A quoted #include is resolved against the including
# FILE's own directory first, so core/gfx/canvas.cpp reaching a sibling "resourcestore.h" stopped
# being a sibling lookup the moment the two files landed in different trees. Each entry restores
# exactly one such pair and no more: the set was derived by resolving every quoted include in both
# products before and after the move and requiring each one to land on the same file as before.
#
# Neither product has a ${NAMP_PRODUCT_DIR}/src/* entry any more -- that was the scaffolding stage
# 3 put up, keeping a sibling lookup working while a byte-identical .cpp sat in core/ and its .h
# did not, and it came down in stage 4 as src/gfx and src/platform emptied of shared files.
#
# The two copies of this comment disagreed about that last point, and merging them is what exposed
# it: the rack's copy said "products/rations still carries one, because it alone has a macplugview"
# and rations' copy, thirty lines from the truth of the matter, said it carries none. Rations is
# right, and its reasoning is the part worth keeping: macplugview.mm reaches its own header as a
# sibling, and core's plugview.h reaches it as "platform/macplugview.h" through the product's own
# src/ entry, so the one file that neither product shares needs no entry of its own.
set(NAMP_CORE_INCLUDES
    ${NAMP_CORE_DIR}
    ${NAMP_CORE_DIR}/platform
    ${NAMP_CORE_DIR}/tools
    ${NAMP_CORE_DIR}/standalone
    ${NAMP_CORE_DIR}/deps/tonestack)

set(NAMP_PRODUCT_DIR "${NAMP_REPO_ROOT}/products/${NAMP_PRODUCT}")
if(NOT EXISTS "${NAMP_PRODUCT_DIR}/CMakeLists.txt")
    file(GLOB _namp_products RELATIVE "${NAMP_REPO_ROOT}/products" "${NAMP_REPO_ROOT}/products/*")
    message(FATAL_ERROR
        "NAMP_PRODUCT is '${NAMP_PRODUCT}', and there is no products/${NAMP_PRODUCT}/CMakeLists.txt.\n"
        "  Available: ${_namp_products}\n"
        "  Configure one product per build directory, for example:\n"
        "    cmake -S . -B build                          (rations)\n"
        "    cmake -S . -B build-rack -DNAMP_PRODUCT=rack")
endif()

# macOS build shape, set BEFORE project() because that is when it is read, and set HERE because
# this is the only file where "before project()" can be said at all. Both variables are consumed
# during compiler detection and by every try_compile the SDK's own checks run, so a value set in
# a product's own CMakeLists -- which a subdirectory reaches only after this file has already
# detected a compiler -- would reach the targets and NOT the probes. That is a build configured
# against one deployment target and checked against another, and it fails as a link warning about
# mismatched minimum versions rather than as anything that names the cause.
#
# Cache entries without FORCE, so a -D on the command line (which is how CI selects the slice)
# still wins. Both are inert on Linux and Windows: CMake ignores them off Apple, which is why they
# need no if(APPLE) guard -- APPLE is not defined until project() has run.
#
# 11.0 is the floor for arm64 and the oldest macOS any current DAW supports. The x86_64 slice
# could reach further back; it does not, because two slices built against two different SDKs is a
# difference nobody would think to look for when one of them misbehaves.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# ONE slice per configure. The universal binary is made by lipo joining two builds afterwards
# rather than by asking one compiler for both, and the reason is the gates: a fat binary built on
# an arm64 host cannot RUN its x86_64 half without assuming Rosetta 2 is installed, so every proof
# would silently only ever cover one architecture. Building each slice on a host of its own
# architecture means every gate runs on the ISA it gates. Defaulted to the host and validated in
# the product's own platform split.
set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}"
    CACHE STRING "macOS architecture slice to build")

# A literal project() call, which CMake requires of a top-level file: without one it pretends
# there is a project(Project) on line 1 and runs compiler detection THERE -- ahead of the two
# variables above, which is precisely the ordering they exist to avoid. Each product calls
# project() again with its own name and version, which is what moduleinfo.json, the makedist
# scripts and the installer read.
project(NAMp LANGUAGES C CXX)

# Every product writes its binaries where a single-product build always wrote them, so that every
# script, gate and habit that names build/rations_offline or build/lv2 keeps working. Without
# this, add_subdirectory() would bury them under build/products/<name>/ and the only thing that
# had changed would be a path nobody chose to change.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Platform split. Set here, after project(), because WIN32 and APPLE do not exist before it, and
# set ONCE for whichever product is being configured.
#
# THREE values, not two. This was rations' block, named RATIONS_WINDOWS / RATIONS_MACOS /
# RATIONS_LINUX, and its own comment records why the third value exists: it used to be
# RATIONS_WINDOWS alone, and "not Windows" silently meant Linux -- true while there were two
# platforms, a trap the moment there were three, because every if(NOT RATIONS_WINDOWS) then had to
# be re-read to find out whether it meant "Linux" (the JACK standalone, the rtcheck allocation
# harness, cairo-xlib) or genuinely "anything but Windows" (-fPIC, the module loader's else arm).
# Naming all three makes each of those a decision at its site instead of an inherited default.
#
# The rack had none of this and tests WIN32 and APPLE directly, which is why cmake/nampcore.cmake
# spells platform that way: a function naming either product's variables would have been a function
# only that product could call. Both products can now be given the same answer, and a shared
# function may ask for it.
set(NAMP_WINDOWS OFF)
set(NAMP_MACOS OFF)
set(NAMP_LINUX OFF)
if(WIN32)
    set(NAMP_WINDOWS ON)
elseif(APPLE)
    set(NAMP_MACOS ON)
else()
    set(NAMP_LINUX ON)
endif()

# The shared vocabulary: namp_source() and the fast-math assertion, plus the recipes each product
# uses to build the amp's graphics and DSP libraries out of core/. Included HERE rather than from
# each product, so both products meet one definition of each and a third product would inherit them
# by existing. It defines functions only -- no targets, no variables, nothing that depends on which
# product is being configured.
include("${NAMP_REPO_ROOT}/cmake/nampcore.cmake")

message(STATUS "")
message(STATUS "[NAMp] product: ${NAMP_PRODUCT}  (products/${NAMP_PRODUCT})")

add_subdirectory("${NAMP_PRODUCT_DIR}" "${CMAKE_BINARY_DIR}/product-${NAMP_PRODUCT}")
