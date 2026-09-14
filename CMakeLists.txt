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
# include() rather than add_subdirectory(), because the product file must run in THIS scope: it
# sets CMAKE_OSX_DEPLOYMENT_TARGET and CMAKE_OSX_ARCHITECTURES before its own project() call, and
# those are read during compiler detection and by every try_compile the SDK's checks run. A
# subdirectory would call project() in a scope of its own, after this file had already configured
# a compiler, and the macOS slice would be probed against one deployment target and compiled
# against another. The comment at that site in products/rations/CMakeLists.txt says the same thing
# from the other end.

cmake_minimum_required(VERSION 3.25.0)

set(NAMP_PRODUCT "rations" CACHE STRING "Which product to build: rations or rack")
set_property(CACHE NAMP_PRODUCT PROPERTY STRINGS rations rack)

# The repository root, which is where the four shared dependency submodules live. Each product
# resolves them from here rather than from beside itself: there is one vst3sdk, one
# NeuralAmpModelerCore, one AudioDSPTools and one eigen in this tree, at the pins both trees
# already agreed on, and a second copy of any of them is a way for the two products to differ.
set(NAMP_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

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

message(STATUS "")
message(STATUS "[NAMp] product: ${NAMP_PRODUCT}  (products/${NAMP_PRODUCT})")

add_subdirectory("${NAMP_PRODUCT_DIR}" "${CMAKE_BINARY_DIR}/product-${NAMP_PRODUCT}")
