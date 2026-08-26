# Cross-compile Rations for 64-bit Windows with MinGW-w64, from Linux.
#
# Usage:
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake
#
# The graphics dependencies (cairo, pixman, freetype, libpng, zlib) are NOT
# packaged for MinGW by Debian — the only mingw library in the archive is
# libz-mingw-w64 — so they are built from source into a private sysroot by
# scripts/build-win-deps.sh. Point RATIONS_WIN_SYSROOT at that prefix, or let it
# default to the location that script installs to.
#
# THREAD MODEL. The POSIX-threads MinGW variant is required, not preferred:
# ModelBank is a std::thread with a std::mutex and a std::condition_variable,
# and Debian's win32-threads variant is built without _GLIBCXX_HAS_GTHREADS, so
# those types do not exist there. Verified with:
#     x86_64-w64-mingw32-g++ -v   =>   Thread model: posix

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(RATIONS_WIN_TRIPLE "x86_64-w64-mingw32"
    CACHE STRING "MinGW-w64 target triple")

# Built by scripts/build-win-deps.sh. Kept outside the repository: it is build
# output, it is large, and it is shared by every build directory.
set(RATIONS_WIN_SYSROOT "$ENV{HOME}/third_party/win-deps/sysroot"
    CACHE PATH "Prefix holding the MinGW builds of cairo/pixman/freetype/libpng/zlib")

set(CMAKE_C_COMPILER   ${RATIONS_WIN_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${RATIONS_WIN_TRIPLE}-g++)
set(CMAKE_RC_COMPILER  ${RATIONS_WIN_TRIPLE}-windres)
set(CMAKE_AR           ${RATIONS_WIN_TRIPLE}-ar)
set(CMAKE_RANLIB       ${RATIONS_WIN_TRIPLE}-ranlib)
set(CMAKE_STRIP        ${RATIONS_WIN_TRIPLE}-strip)

# Look for headers and libraries in the sysroot and the cross toolchain only;
# find programs on the build host, so cmake/pkg-config/ninja still resolve.
set(CMAKE_FIND_ROOT_PATH "${RATIONS_WIN_SYSROOT}" "/usr/${RATIONS_WIN_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must read the sysroot's .pc files and nothing from the host, or a
# configure would happily hand back /usr/lib/x86_64-linux-gnu flags.
set(ENV{PKG_CONFIG_LIBDIR} "${RATIONS_WIN_SYSROOT}/lib/pkgconfig")
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
# offline tools it is what lets them run under Wine at all — without it they
# fail to start with no output and exit 53 (ERROR_BAD_NETPATH from the loader).
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
find_program(RATIONS_WINE_EXECUTABLE wine)
if(RATIONS_WINE_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${RATIONS_WINE_EXECUTABLE}")
endif()
