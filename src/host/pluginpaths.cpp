// PluginPaths implementation. See pluginpaths.h.

#include "pluginpaths.h"
#include "scancache.h" // escapeField / unescapeField / pathIsSafe, shared with the scan cache

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace NAMp::host
{

namespace
{

constexpr char kHeader[] = "#NAMPPATHS 1";

} // namespace

//------------------------------------------------------------------------
std::string PluginPaths::defaultFile()
{
#if defined(_WIN32)
    // %LOCALAPPDATA%, for the reason spelled out at defaultCachePath() in scancache.cpp: it is the
    // per-user, per-machine directory, and a list of absolute search roots means nothing on another
    // machine. Windows keeps no separate config and cache roots, so the config/cache distinction
    // this file's header draws — losing the cache costs a rescan, losing this loses something the
    // user typed — survives only as the filename here.
    if (const char *local = std::getenv("LOCALAPPDATA"); local && local[0])
        return std::string(local) + "\\NAMp-Rack\\pluginpaths";
    return {};
#else
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0])
        return std::string(xdg) + "/NAMp-Rack/pluginpaths";
    if (const char *home = std::getenv("HOME"); home && home[0])
        return std::string(home) + "/.config/NAMp-Rack/pluginpaths";
    return {};
#endif
}

//------------------------------------------------------------------------
bool PluginPaths::load(const std::string &file)
{
    mRoots.clear();
    if (file.empty())
        return false;

    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec)
        return true; // a first run is not a failure

    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > kMaxFileBytes) {
        std::fprintf(stderr, "namp-rack: %s is missing or too large to be a plug-in path list\n",
                     file.c_str());
        return false;
    }

    std::ifstream in(file);
    if (!in)
        return false;

    std::string line;
    if (!std::getline(in, line) || line != kHeader) {
        // Same rule as the scan cache: an unknown version is discarded whole rather than migrated.
        // The cost is that the user re-adds their folders once; the alternative is guessing at the
        // meaning of a format this build has never seen.
        std::fprintf(stderr, "namp-rack: %s is not a version this build understands; ignoring it\n",
                     file.c_str());
        return false;
    }

    while (std::getline(in, line)) {
        if (line.empty() || line.size() > kMaxLineBytes)
            continue;
        std::string path;
        if (!unescapeField(line, path) || !pathIsSafe(path))
            continue;
        if (mRoots.size() >= kMaxRoots)
            break;
        // A duplicate in the file is dropped rather than rejected: it would only make one directory
        // be walked twice, and the scan de-duplicates anyway.
        bool already = false;
        for (const std::string &root : mRoots)
            already = already || root == path;
        if (!already)
            mRoots.push_back(std::move(path));
    }
    return true;
}

//------------------------------------------------------------------------
bool PluginPaths::save(const std::string &file) const
{
    if (file.empty())
        return false;

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(file).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return false;
    }

    // Written through a temporary and renamed, so an interrupted write cannot leave a half-file
    // where a valid one was — the same discipline saveRack() uses.
    const std::string tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        out << kHeader << '\n';
        for (const std::string &root : mRoots)
            out << escapeField(root) << '\n';
        if (!out.good())
            return false;
    }

    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
bool PluginPaths::add(const std::string &dir, std::string &error)
{
    if (!pathIsSafe(dir)) {
        error = "not an absolute path, or contains \"..\", a tab or a newline";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        error = "not a directory";
        return false;
    }

    // Trailing separators would make /usr/lib/vst3 and /usr/lib/vst3/ two different rows that walk
    // the same tree, so the stored form is canonical. Both separators are stripped on Windows,
    // where either is accepted in a path and a folder chosen in the browser can carry either.
    //
    // The loop stops at size 3 there rather than 1, so "C:\\" keeps the separator that is what
    // makes it absolute — pathIsSafe() would reject the "C:" it would otherwise be reduced to, and
    // the root of a drive is a legitimate thing to point a scan at.
    std::string canonical = dir;
#if defined(_WIN32)
    while (canonical.size() > 3 && (canonical.back() == '\\' || canonical.back() == '/'))
        canonical.pop_back();
#else
    while (canonical.size() > 1 && canonical.back() == '/')
        canonical.pop_back();
#endif

    for (const std::string &root : mRoots) {
        if (root == canonical) {
            error = "already in the list";
            return false;
        }
    }
    if (mRoots.size() >= kMaxRoots) {
        error = "the list is full";
        return false;
    }

    mRoots.push_back(std::move(canonical));
    return true;
}

//------------------------------------------------------------------------
bool PluginPaths::remove(const std::string &dir)
{
    for (auto it = mRoots.begin(); it != mRoots.end(); ++it) {
        if (*it == dir) {
            mRoots.erase(it);
            return true;
        }
    }
    return false;
}

} // namespace NAMp::host
