// ScanCache implementation. See scancache.h for the file format and the untrusted-input contract.

#include "scancache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace NAMp::host
{

namespace
{

// Every one of these is a bound on untrusted input, checked before the value is used.
constexpr size_t kMaxCacheBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxLineBytes = 16u * 1024u;
constexpr size_t kMaxLines = 100000;
constexpr size_t kMaxPathLength = 4096;

constexpr char kHeader[] = "#NAMPCACHE 1";

} // namespace

//------------------------------------------------------------------------
bool pathIsSafe(const std::string &path)
{
    if (path.empty() || path.size() > kMaxPathLength)
        return false;
#if defined(_WIN32)
    // "C:\\..." or "\\\\server\\share\\...", and nothing else. A drive-RELATIVE path ("C:foo")
    // and a root-relative one ("\\foo") are both rejected, because what each resolves to depends on
    // the process's current directory or current drive at the moment it is used — and these strings
    // are read back out of a cache file written by an earlier run, where neither was the same.
    //
    // Hand-written rather than std::filesystem::path::is_absolute(), and that is the same decision
    // the resource-path helper documents at length: constructing a path from a narrow string throws
    // std::filesystem::filesystem_error on libstdc++/MinGW when the bytes are not valid UTF-8, and
    // every string reaching this function came out of an untrusted file.
    const bool driveAbsolute =
        path.size() >= 3 &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    const bool uncAbsolute = path.size() >= 2 && (path[0] == '\\' || path[0] == '/') &&
                             (path[1] == '\\' || path[1] == '/');
    if (!driveAbsolute && !uncAbsolute)
        return false;
#else
    if (path[0] != '/')
        return false;
#endif
    if (path.find("..") != std::string::npos)
        return false;
    // A tab or newline in a path would split or terminate a cache row.
    if (path.find('\t') != std::string::npos || path.find('\n') != std::string::npos)
        return false;
    return true;
}

//------------------------------------------------------------------------
std::string escapeField(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

//------------------------------------------------------------------------
bool unescapeField(const std::string &in, std::string &out)
{
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\') {
            out += in[i];
            continue;
        }
        // A backslash must be followed by something; a trailing lone backslash is malformed.
        if (i + 1 >= in.size())
            return false;
        switch (in[++i]) {
            case '\\':
                out += '\\';
                break;
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            default:
                return false;
        }
    }
    return true;
}

//------------------------------------------------------------------------
void ScanCache::load(const std::string &path)
{
    mEntries.clear();
    if (path.empty())
        return;

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return; // no cache yet: a cold start, not a failure
    if (size > kMaxCacheBytes) {
        std::fprintf(stderr, "namp-rack: plug-in cache is implausibly large, ignoring it\n");
        return;
    }

    std::ifstream in(path);
    if (!in)
        return;

    std::string line;
    if (!std::getline(in, line) || line != kHeader) {
        // Unknown or missing version. Discard the lot rather than guess: a cache is regenerable.
        return;
    }

    size_t lineCount = 0;
    while (std::getline(in, line)) {
        if (++lineCount > kMaxLines) {
            std::fprintf(stderr, "namp-rack: plug-in cache has too many rows, ignoring the rest\n");
            break;
        }
        if (line.size() > kMaxLineBytes)
            continue;

        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos)
            continue;
        const size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos)
            continue;

        std::string entryPath;
        std::string payload;
        if (!unescapeField(line.substr(0, t1), entryPath))
            continue;
        if (!unescapeField(line.substr(t2 + 1), payload))
            continue;
        if (!pathIsSafe(entryPath))
            continue;

        // Negative values are normal here: file_time_type does not count from the unix epoch.
        // Only kNoMTime is rejected, because it is the "could not read" sentinel and must never
        // match a live stamp.
        const std::string mtimeField = line.substr(t1 + 1, t2 - t1 - 1);
        errno = 0;
        char *end = nullptr;
        const long long mtime = std::strtoll(mtimeField.c_str(), &end, 10);
        if (errno != 0 || !end || *end != 0 || mtime == kNoMTime)
            continue;

        Entry &e = mEntries[entryPath];
        e.mtime = static_cast<int64_t>(mtime);
        // An empty payload is the remembered negative result, so it contributes no line.
        if (!payload.empty())
            e.lines.push_back(payload);
    }
}

//------------------------------------------------------------------------
bool ScanCache::save(const std::string &path) const
{
    if (path.empty())
        return false;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;

    out << kHeader << '\n';
    for (const auto &[entryPath, entry] : mEntries) {
        // Only what this scan actually saw: an uninstalled plug-in simply is not written back.
        if (!entry.seen)
            continue;
        const std::string escapedPath = escapeField(entryPath);
        if (entry.lines.empty()) {
            // Remembered negative result.
            out << escapedPath << '\t' << entry.mtime << '\t' << '\n';
            continue;
        }
        for (const auto &line : entry.lines)
            out << escapedPath << '\t' << entry.mtime << '\t' << escapeField(line) << '\n';
    }
    return out.good();
}

//------------------------------------------------------------------------
bool ScanCache::hit(const std::string &path, int64_t mtime, std::vector<std::string> &lines)
{
    auto it = mEntries.find(path);
    // kNoMTime means "could not stat", which must never match a stored stamp.
    if (it == mEntries.end() || mtime == kNoMTime || it->second.mtime != mtime)
        return false;
    it->second.seen = true;
    lines = it->second.lines;
    return true;
}

//------------------------------------------------------------------------
void ScanCache::store(const std::string &path, int64_t mtime, const std::vector<std::string> &lines)
{
    Entry &e = mEntries[path];
    e.mtime = mtime;
    e.lines = lines;
    e.seen = true;
}

//------------------------------------------------------------------------
int64_t newestMTime(const std::string &path, int depth)
{
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec)
        return kNoMTime;

    // An unreadable entry yields kNoMTime, which is INT64_MIN and therefore loses every max()
    // below rather than winning it. Returning 0 here would be worse than useless: real stamps are
    // negative, so 0 would mask every genuine timestamp in the bundle.
    auto stamp = [](const std::filesystem::path &p) -> int64_t {
        std::error_code e;
        const auto t = std::filesystem::last_write_time(p, e);
        if (e)
            return kNoMTime;
        return static_cast<int64_t>(t.time_since_epoch().count());
    };

    if (!std::filesystem::is_directory(status))
        return stamp(path);

    int64_t newest = stamp(path);
    if (depth <= 0)
        return newest;

    // Not recursive_directory_iterator: the depth limit is what bounds a hostile or looping tree,
    // and symlinks are deliberately never followed.
    std::filesystem::directory_iterator it(
        path, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
        return newest;

    for (const auto &entry : it) {
        std::error_code e;
        const auto st = entry.symlink_status(e);
        if (e || std::filesystem::is_symlink(st))
            continue;
        const int64_t child = std::filesystem::is_directory(st)
                                  ? newestMTime(entry.path().string(), depth - 1)
                                  : stamp(entry.path());
        if (child > newest)
            newest = child;
    }
    return newest;
}

//------------------------------------------------------------------------
std::string defaultCachePath()
{
#if defined(_WIN32)
    // %LOCALAPPDATA%, which is where a Windows application's own derived state belongs: it is
    // per-user and per-machine, and it is the one that is NOT copied around by a roaming profile.
    // That matters for exactly this file — a scan cache is keyed on absolute paths and modification
    // times on THIS machine, so carrying it to another one could only ever produce wrong answers.
    // The same reasoning puts the plug-in index and the search-path list there; neither has a
    // meaning off the machine that wrote it.
    //
    // XDG is not consulted at all here. A Windows box that happens to have XDG_CACHE_HOME set has
    // it because some ported tool put it there, pointing at a POSIX-shaped path that is not where
    // this application's state goes.
    if (const char *local = std::getenv("LOCALAPPDATA"); local && local[0])
        return std::string(local) + "\\NAMp-Rack\\plugincache";
    return {};
#else
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && xdg[0])
        return std::string(xdg) + "/NAMp-Rack/plugincache";
    if (const char *home = std::getenv("HOME"); home && home[0])
        return std::string(home) + "/.cache/NAMp-Rack/plugincache";
    return {};
#endif
}

} // namespace NAMp::host
