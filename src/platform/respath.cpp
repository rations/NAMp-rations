// Resource-directory resolution. See respath.h.

#include "respath.h"

#include "gfx/resourcestore.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Rations
{

namespace
{

// std::filesystem rather than stat/GetFileAttributes: it is the same call on
// both platforms, and on Windows libstdc++ takes the wide path internally, so a
// bundle installed under a non-ASCII directory still resolves.
bool isDir(const std::string &path)
{
    if (path.empty())
        return false;
    std::filesystem::path p;
    if (!utf8ToPath(path, p))
        return false;
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}

// Both separators, because Windows hands back backslashes and the strings this
// builds are also concatenated with '/' elsewhere. A POSIX path never contains
// a backslash that is meant as a separator, so one form covers both.
std::string parentOf(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos || slash == 0)
        return std::string();
    return path.substr(0, slash);
}

// Address of a symbol in THIS module, so the platform can resolve it back to a
// file name. Taking the address of a local function is enough.
void marker()
{
}

#if defined(_WIN32)
// A wide Win32 path as UTF-8. std::filesystem::path is the conversion: unlike
// MSVC's STL, libstdc++ uses UTF-8 for a path's narrow form regardless of the
// active code page, so this is lossless for any path Windows can represent.
std::string widePathToUtf8(const std::wstring &w)
{
    return std::filesystem::path(w).string();
}

// GetModuleFileNameW truncates rather than failing when the buffer is too
// small, and only signals it through ERROR_INSUFFICIENT_BUFFER, so grow until
// it fits instead of trusting MAX_PATH.
std::string moduleFileName(HMODULE module)
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            return std::string();
        if (n < buf.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            buf.resize(n);
            return widePathToUtf8(buf);
        }
        if (buf.size() >= 32768) // the Win32 long-path ceiling
            return std::string();
        buf.resize(buf.size() * 2);
    }
}
#endif

// <bundle>/Contents/<arch>/<module> -> <bundle>/Contents/Resources
std::string fromModulePath()
{
    std::string modulePath;

#if defined(_WIN32)
    // The plug-in is a DLL, so "our own file" is not the executable: ask which
    // module contains this function. UNCHANGED_REFCOUNT because we only want
    // the path, not to pin the module in memory.
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&marker), &self) == 0 ||
        self == nullptr)
        return std::string();
    modulePath = moduleFileName(self);
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(&marker), &info) == 0 || !info.dli_fname)
        return std::string();
    modulePath = info.dli_fname;
#endif

    if (modulePath.empty())
        return std::string();

    const std::string archDir = parentOf(modulePath); // Contents/<arch>
    const std::string contents = parentOf(archDir);   // Contents
    if (contents.empty())
        return std::string();

    const std::string res = contents + "/Resources";
    return isDir(res) ? res : std::string();
}

// For the standalone: <dir of executable>/resources, and one level up, so it
// works both from a build tree and from an installed prefix.
std::string fromExecutablePath()
{
    std::string exePath;

#if defined(_WIN32)
    exePath = moduleFileName(nullptr); // nullptr = the running executable
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::string();
    buf[n] = '\0';
    exePath = buf;
#endif

    const std::string binDir = parentOf(exePath);
    if (binDir.empty())
        return std::string();

    const std::string candidates[] = {binDir + "/resources", parentOf(binDir) + "/resources"};
    for (const std::string &c : candidates)
        if (isDir(c))
            return c;
    return std::string();
}

std::string resolve()
{
    if (const char *env = std::getenv("RATIONS_RESOURCE_DIR")) {
        if (isDir(env))
            return std::string(env);
        fprintf(stderr, "Rations: RATIONS_RESOURCE_DIR=%s is not a directory (ignored)\n", env);
    }

    std::string dir = fromModulePath();
    if (!dir.empty())
        return dir;

    dir = fromExecutablePath();
    if (!dir.empty())
        return dir;

    // No directory is the NORMAL state for a binary that carries its art and fonts inside itself
    // (src/gfx/resourcestore.h), so it is only worth a warning when there is nothing to fall back
    // on either — which is the case that really does draw flat rectangles.
    if (embeddedResourceCount() == 0)
        fprintf(stderr, "Rations: could not locate the resource directory; "
                        "art and fonts will fall back\n");
    return std::string();
}

} // namespace

//------------------------------------------------------------------------
const std::string &resourceDir()
{
    static const std::string dir = resolve();
    return dir;
}

//------------------------------------------------------------------------
bool utf8ToPath(const std::string &s, std::filesystem::path &out)
{
    // The only exception this can raise is the encoding failure documented in
    // respath.h, but catching by type would mean naming a libstdc++ detail; the
    // contract here is simply that nothing escapes. On POSIX nothing is thrown
    // at all and the compiler folds this away.
    try {
        out = std::filesystem::path(s);
        return true;
    } catch (...) {
        out.clear();
        return false;
    }
}

//------------------------------------------------------------------------
std::string pathBaseName(const std::string &path)
{
    std::filesystem::path p;
    if (!utf8ToPath(path, p))
        return path; // not expressible as a path here; show it as it came

    if (p.has_filename())
        return p.filename().string();

    // A trailing separator: "/foo/bar/" and "C:\" both have an empty
    // filename(). Step up once and take the name from there.
    const std::filesystem::path parent = p.parent_path();
    if (parent.has_filename())
        return parent.filename().string();

    return path; // a root: there is no shorter name for it than itself
}

//------------------------------------------------------------------------
bool readFileBytes(const std::string &path, std::vector<unsigned char> &out)
{
    out.clear();
    if (path.empty())
        return false;

    std::filesystem::path p;
    if (!utf8ToPath(path, p))
        return false;

    // The art is replaceable by whoever installs the plug-in, so its size is
    // not ours to trust: a resource file that is somehow a gigabyte must fail
    // rather than be allocated. The largest thing shipped is a few hundred KB.
    constexpr std::uintmax_t kMaxResourceBytes = 64u * 1024u * 1024u;

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec || size > kMaxResourceBytes)
        return false;

    // Constructed from the path rather than from path.c_str(): on Windows that
    // is what reaches the wide CRT call. See the note in respath.h.
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return false;

    out.resize(static_cast<size_t>(size));
    if (size == 0)
        return true;
    in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
    // gcount(), not just the stream state: a short read is a failure here even
    // though it does not set failbit until the next operation.
    if (static_cast<std::uintmax_t>(in.gcount()) != size) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace Rations
