# Cross-compile either product for 64-bit Windows with MinGW-w64, from Linux.
#
# One file for both, since stage 7. Everything below is a property of the
# TOOLCHAIN — the triple, the sysroot, the link flags, the Wine emulator — and
# none of it ever differed between the two; they had two copies of it because
# they had been two repositories.
#
# Usage:
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake
#
#   cmake -S . -B build-win-rack -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
#         -DNAMP_PRODUCT=rack -DNAMPRACK_BUILD_LV2_HOST=OFF
#
# LV2 hosting is off on Windows by construction — lilv, suil and jalv's event
# buffer are Linux-shaped here — and the rack's CMakeLists.txt refuses the
# combination loudly rather than ifdef'ing its way around it. That option is the
# rack's; rations has no LV2 host to switch off.
#
# The graphics dependencies (cairo, pixman, freetype, libpng, zlib) are NOT
# packaged for MinGW by Debian — the only mingw library in the archive is
# libz-mingw-w64 — so they are built from source into a private sysroot by
# scripts/build-win-deps.sh — one script for both products, installing to one
# prefix, so the two link the same graphics stack rather than two builds of it.
# Point NAMP_WIN_SYSROOT at that prefix, or let it default to the location that
# script installs to. The ASIO SDK, which the rack's Windows audio backend
# hosts, reaches the build through the same sysroot and through nothing else —
# a licence condition rather than a layout preference: Steinberg's agreement
# forbids redistributing the SDK, and the sysroot is untracked build output.
#
# THREAD MODEL. The POSIX-threads MinGW variant is required, not preferred:
# ModelBank is a std::thread with a std::mutex and a std::condition_variable,
# and Debian's win32-threads variant is built without _GLIBCXX_HAS_GTHREADS, so
# those types do not exist there. Verified with:
#     x86_64-w64-mingw32-g++ -v   =>   Thread model: posix
#
# 64-BIT ONLY, AND THE DOOR STAYS SHUT. This binds both products, and the
# reason belongs to the rack. ASIO's IASIO vtable uses MSVC's thiscall
# convention, which GCC does not implement — the reason RtAudio carries an
# IASIOThiscallResolver at all. That resolver is guarded `&& !defined(_WIN64)`
# in both its header and its implementation, so on a single-calling-convention
# Win64 target the entire problem compiles out. Adding an i686 triple here
# brings it back.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(NAMP_WIN_TRIPLE "x86_64-w64-mingw32"
    CACHE STRING "MinGW-w64 target triple")

# Built by scripts/build-win-deps.sh. Kept outside the repository: it is build
# output, it is large, and it is shared by every build directory.
#
# The two old spellings are still honoured, for the reason stage 6 found the hard
# way: a renamed option that is simply dropped leaves -D<old name> reported once
# as "manually-specified variables were not used", after which the build succeeds
# against the WRONG prefix rather than failing. Anyone who has NAMPRACK_WIN_SYSROOT
# or RATIONS_WIN_SYSROOT in a shell or a note keeps working.
set(_namp_win_sysroot "$ENV{HOME}/third_party/win-deps/sysroot")
if(DEFINED NAMPRACK_WIN_SYSROOT)
    set(_namp_win_sysroot "${NAMPRACK_WIN_SYSROOT}")
elseif(DEFINED RATIONS_WIN_SYSROOT)
    set(_namp_win_sysroot "${RATIONS_WIN_SYSROOT}")
endif()
set(NAMP_WIN_SYSROOT "${_namp_win_sysroot}" CACHE PATH
    "Prefix holding the MinGW builds of cairo/pixman/freetype/libpng/zlib, and the ASIO SDK")

set(CMAKE_C_COMPILER   ${NAMP_WIN_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${NAMP_WIN_TRIPLE}-g++)
set(CMAKE_RC_COMPILER  ${NAMP_WIN_TRIPLE}-windres)
set(CMAKE_AR           ${NAMP_WIN_TRIPLE}-ar)
set(CMAKE_RANLIB       ${NAMP_WIN_TRIPLE}-ranlib)
set(CMAKE_STRIP        ${NAMP_WIN_TRIPLE}-strip)

# Look for headers and libraries in the sysroot and the cross toolchain only;
# find programs on the build host, so cmake/pkg-config/ninja still resolve.
set(CMAKE_FIND_ROOT_PATH "${NAMP_WIN_SYSROOT}" "/usr/${NAMP_WIN_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must read the sysroot's .pc files and nothing from the host, or a
# configure would happily hand back /usr/lib/x86_64-linux-gnu flags.
set(ENV{PKG_CONFIG_LIBDIR} "${NAMP_WIN_SYSROOT}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")

# Link the GCC runtime and libwinpthread IN, rather than depending on
# libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll beside the binary.
#
# For the plug-in this is a hard requirement, not a preference: the SDK loads a
# VST3 with a plain LoadLibraryW of the full path
# (public.sdk/source/vst/hosting/module_win32.cpp, loadAsPackage), and the
# default DLL search order does not include the loaded module's own directory,
# so a runtime DLL shipped inside the bundle would never be found. For the
# standalone and the offline tools it is what lets them run under Wine at all —
# without it they fail to start with no output and exit 53 (ERROR_BAD_NETPATH
# from the loader).
#
# Set as _INIT in the toolchain rather than with add_link_options() in
# CMakeLists so it also reaches the SDK's own validator and moduleinfotool,
# which are added by add_subdirectory() before any of our targets exist.
#
# --no-undefined mirrors what the SDK's SMTG_PlatformToolset.cmake asks for on
# MinGW; it is repeated here because that file sets it with a non-FORCE
# CACHE set(), which cannot overwrite a value already seeded from _INIT.
# --no-insert-timestamp makes the Windows build REPRODUCIBLE, and without it the
# Windows half of this project has no usable "the binaries did not move" gate at
# all. Measured before adding it: two builds of one tree with one toolchain, two
# minutes apart, differ -- and differ in exactly three bytes out of 6.3 million,
# at 137, 138 and 217, which are the PE header's TimeDateStamp and the checksum
# covering it. objdump -p reads that field back as the wall-clock minute each
# link happened. Nothing else moves.
#
# So the whole difference was a clock, and the Linux side's central verification
# method -- build it again, compare every byte, and attribute anything that moved
# -- simply did not exist over here. binutils inserts that stamp by default; this
# flag tells it not to, and the field becomes zero. It is also the reason the
# .vst3 carried a SECOND pair of moving bytes nine megabytes in: the same stamp
# again in the debug directory.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static -Wl,--no-insert-timestamp")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -Wl,--no-undefined -Wl,--no-insert-timestamp")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static -Wl,--no-undefined -Wl,--no-insert-timestamp")

# Run cross-built test/tool executables under Wine. This is what lets the
# offline render and the SDK's validator/moduleinfotool be driven from the
# build, and it is why a Windows VM is not needed for the normal loop.
#
# WINE IS A SMOKE TEST AND NOTHING MORE. It proves the binaries load, the
# validator passes and the editor draws. It has no ASIO driver at all, and its
# WASAPI is a shim over the build host's own sound server, so no audio timing or
# drop-out number may come from here — those come from a real Windows machine.
find_program(NAMP_WINE_EXECUTABLE wine)
if(NAMP_WINE_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${NAMP_WINE_EXECUTABLE}")
endif()
