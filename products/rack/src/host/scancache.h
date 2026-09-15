// ScanCache — remembering what a plug-in scan found, so it happens once and not every launch.
//
// Scanning a binary plug-in means loading third-party code, which is slow and occasionally fatal.
// The cache is keyed by path plus modification time, so an unchanged plug-in is never probed twice,
// and a plug-in that produced nothing is remembered as having produced nothing — otherwise every
// launch re-probes the same broken file.
//
// FILE FORMAT (one header line, then one line per catalogue row):
//
//   #NAMPCACHE 1
//   <escaped path>\t<mtime>\t<escaped payload>
//
// A path yielding several classes gets several lines. An EMPTY payload field is the remembered
// negative result. Escaping is backslash, newline and tab only, so a payload can itself contain
// tabs and the row still splits into exactly three fields.
//
// The file is UNTRUSTED INPUT. It lives in a user-writable cache directory and
// nothing stops it being edited, truncated or filled with garbage. Every limit below is enforced
// before the value is used, an unparseable row is skipped rather than fatal, and an unknown header
// version discards the whole file. That last choice is free: a cache is regenerable, so discarding
// removes an entire class of format-drift bug at the cost of one rescan.
//
// Only entries marked seen during a scan are written back, which is how an uninstalled plug-in
// leaves the cache without anyone having to notice it went.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace NAMp::host
{

//------------------------------------------------------------------------
class ScanCache
{
public:
    // Reads the cache if it is there. A missing file is not an error — it is a cold start. A
    // malformed file is not an error either; it is discarded and reported as a cold start.
    void load(const std::string &path);

    // Writes every entry marked seen. Returns false if the file could not be written, which is
    // worth one warning and nothing more: the next launch simply rescans.
    bool save(const std::string &path) const;

    // Cache lookup. Returns true only when the path is known AND its mtime is unchanged, in which
    // case `lines` is filled with the remembered payloads (possibly none, for a negative result).
    // A hit marks the entry seen.
    bool hit(const std::string &path, int64_t mtime, std::vector<std::string> &lines);

    // Record a fresh scan result, replacing whatever was there, and mark it seen.
    void store(const std::string &path, int64_t mtime, const std::vector<std::string> &lines);

    size_t entryCount() const
    {
        return mEntries.size();
    }

private:
    struct Entry {
        int64_t mtime = 0;
        std::vector<std::string> lines;
        bool seen = false;
    };

    std::map<std::string, Entry> mEntries;
};

//------------------------------------------------------------------------
// "Could not read a timestamp." Deliberately not 0: std::filesystem::file_time_type does NOT count
// from the unix epoch — libstdc++ uses a clock whose epoch is in the future, so ordinary files have
// large NEGATIVE counts and 0 would both be a plausible real value and sort above every real one.
constexpr int64_t kNoMTime = INT64_MIN;

//------------------------------------------------------------------------
// Newest modification time anywhere under `path`, in whatever units file_time_type counts in. The
// value is only ever compared against a previously stored one, so its epoch does not matter — but
// its SIGN does, which is why kNoMTime exists.
//
// A .vst3 is a bundle DIRECTORY, so a plain stat of the top level would miss an updated binary
// inside it; the walk is depth-limited and does not follow symlinks. Returns kNoMTime when the path
// cannot be read, which never compares equal to a stored stamp and therefore forces a rescan.
int64_t newestMTime(const std::string &path, int depth = 8);

// Where the cache lives: $XDG_CACHE_HOME/NAMp-Rack/plugincache, else
// $HOME/.cache/NAMp-Rack/plugincache, and on Windows %LOCALAPPDATA%\NAMp-Rack\plugincache.
// Deliberately not beside the standalone's settings file — a cache is regenerable and losing it
// costs one rescan, so it does not belong with configuration. Returns empty if no variable
// resolves.
std::string defaultCachePath();

//------------------------------------------------------------------------
// Escaping used by the cache lines, exposed for the scan helper's own output handling and for
// tests. escape() is total; unescape() rejects a trailing lone backslash.
std::string escapeField(const std::string &in);
bool unescapeField(const std::string &in, std::string &out);

//------------------------------------------------------------------------
// Reject a path before anything is loaded from it. Absolute, no "..", no tab or newline (either
// would corrupt a cache line), and bounded length.
//
// "Absolute" is per platform and is not the same test: a leading '/' on POSIX, and on Windows a
// drive-absolute "C:\..." or a UNC "\\server\share\...". A Windows path that merely starts with a
// separator, or with a drive letter and no separator, is REJECTED — what it resolves to depends on
// the current directory or current drive of whichever process reads it, and these strings come back
// out of files written by an earlier run.
bool pathIsSafe(const std::string &path);

} // namespace NAMp::host
