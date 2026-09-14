# Cross-compile NAMp-Rack for 64-bit Windows with MinGW-w64, from Linux.
#
# Usage:
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
#         -DNAMPRACK_BUILD_LV2_HOST=OFF
#
# LV2 hosting is off on Windows by construction — lilv, suil and jalv's event
# buffer are Linux-shaped here — and CMakeLists.txt refuses the combination
# loudly rather than ifdef'ing its way around it.
#
# The graphics dependencies (cairo, pixman, freetype, libpng, zlib) are NOT
# packaged for MinGW by Debian — the only mingw library in the archive is
# libz-mingw-w64 — so they are built from source into a private sysroot by
# scripts/build-win-deps.sh. Point NAMPRACK_WIN_SYSROOT at that prefix, or let
# it default to the location that script installs to. The ASIO SDK reaches the
# build through the same sysroot and through nothing else, which is a licence
# condition rather than a layout preference: Steinberg's agreement forbids
# redistributing the SDK, and the sysroot is untracked build output.
#
# THREAD MODEL. The POSIX-threads MinGW variant is required, not preferred:
# ModelBank is a std::thread with a std::mutex and a std::condition_variable,
# and Debian's win32-threads variant is built without _GLIBCXX_HAS_GTHREADS, so
# those types do not exist there. Verified with:
#     x86_64-w64-mingw32-g++ -v   =>   Thread model: posix
#
# 64-BIT ONLY, AND THE DOOR STAYS SHUT. ASIO's IASIO vtable uses MSVC's thiscall
# convention, which GCC does not implement — the reason RtAudio carries an
# IASIOThiscallResolver at all. That resolver is guarded `&& !defined(_WIN64)`
# in both its header and its implementation, so on a single-calling-convention
# Win64 target the entire problem compiles out. Adding an i686 triple here
# brings it back.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(NAMPRACK_WIN_TRIPLE "x86_64-w64-mingw32"
    CACHE STRING "MinGW-w64 target triple")

# Built by scripts/build-win-deps.sh. Kept outside the repository: it is build
# output, it is large, and it is shared by every build directory.
set(NAMPRACK_WIN_SYSROOT "$ENV{HOME}/third_party/win-deps/sysroot"
    CACHE PATH "Prefix holding the MinGW builds of cairo/pixman/freetype/libpng/zlib and the ASIO SDK")

set(CMAKE_C_COMPILER   ${NAMPRACK_WIN_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${NAMPRACK_WIN_TRIPLE}-g++)
set(CMAKE_RC_COMPILER  ${NAMPRACK_WIN_TRIPLE}-windres)
set(CMAKE_AR           ${NAMPRACK_WIN_TRIPLE}-ar)
set(CMAKE_RANLIB       ${NAMPRACK_WIN_TRIPLE}-ranlib)
set(CMAKE_STRIP        ${NAMPRACK_WIN_TRIPLE}-strip)

# Look for headers and libraries in the sysroot and the cross toolchain only;
# find programs on the build host, so cmake/pkg-config/ninja still resolve.
set(CMAKE_FIND_ROOT_PATH "${NAMPRACK_WIN_SYSROOT}" "/usr/${NAMPRACK_WIN_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must read the sysroot's .pc files and nothing from the host, or a
# configure would happily hand back /usr/lib/x86_64-linux-gnu flags.
set(ENV{PKG_CONFIG_LIBDIR} "${NAMPRACK_WIN_SYSROOT}/lib/pkgconfig")
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
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -Wl,--no-undefined")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static -Wl,--no-undefined")

# Run cross-built test/tool executables under Wine. This is what lets the
# offline render and the SDK's validator/moduleinfotool be driven from the
# build, and it is why a Windows VM is not needed for the normal loop.
#
# WINE IS A SMOKE TEST AND NOTHING MORE. It proves the binaries load, the
# validator passes and the editor draws. It has no ASIO driver at all, and its
# WASAPI is a shim over the build host's own sound server, so no audio timing or
# drop-out number may come from here — those come from a real Windows machine.
find_program(NAMPRACK_WINE_EXECUTABLE wine)
if(NAMPRACK_WINE_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${NAMPRACK_WINE_EXECUTABLE}")
endif()
