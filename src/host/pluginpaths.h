// PluginPaths — the extra directories the user has told this host to look in, and the file they
// persist to.
//
// ONE LIST, NOT ONE PER FORMAT. The parent project keeps a separate editable path list per format
// and makes the user pick the format from a combo box before adding a folder. That choice cannot be
// made wrong in a useful way: a directory either contains `.vst3` bundles or it does not, and the
// LV2 loader only recognises a `.lv2` directory carrying a manifest. So a root is added once and
// every scanner looks in it — one fewer decision to get wrong, and a folder holding both kinds
// works with no special handling at all.
//
// THESE ARE ADDITIONS, NEVER REPLACEMENTS. The VST3 SDK's own module enumeration and lilv's
// standard search path both still run exactly as before; an empty list means the behaviour is
// byte-for-byte what it was. That is why nothing here ever sets `LILV_OPTION_LV2_PATH`, which is
// documented to OVERRIDE the environment and the built-in default ("lilv will only look inside the
// given path", lilv.h) — using it would mean reconstructing lilv's own default from the outside and
// being silently wrong on any distribution that built it differently.
//
// CONFIG, NOT CACHE. This lives beside standalone.state under $XDG_CONFIG_HOME, unlike the scan
// cache: losing the cache costs one rescan, losing this loses something the user typed.
//
// UNTRUSTED ON THE WAY IN. The file is bounded in total size, per line, and in
// entry count, every path is validated by pathIsSafe() before it can reach a scanner, and a line
// that does not parse is skipped with the rest of the file still honoured.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace NAMp::host
{

//------------------------------------------------------------------------
class PluginPaths
{
public:
    // Caps. A search root list is a handful of directories in every real use; these exist so a
    // corrupt or hostile file cannot make the host allocate without bound before it has even drawn
    // a window.
    static constexpr size_t kMaxRoots = 64;
    static constexpr size_t kMaxFileBytes = 256u * 1024u;
    static constexpr size_t kMaxLineBytes = 4096;

    // $XDG_CONFIG_HOME/NAMp-Rack/pluginpaths, else $HOME/.config/NAMp-Rack/pluginpaths, and on
    // Windows %LOCALAPPDATA%\NAMp-Rack\pluginpaths. Empty when no variable resolves, in which case
    // the list is usable for the session and simply not persisted.
    static std::string defaultFile();

    // Reads the file if it is there. A missing file is a first run, not an error; a malformed one
    // is discarded down to the rows that do parse. Returns false only when the file existed and
    // could not be read or carried an unknown version, which is worth one warning.
    bool load(const std::string &file);

    // Writes the list, creating the directory if need be. Returns false on any failure.
    bool save(const std::string &file) const;

    // Adds a root. Returns false and sets `error` when the path is not absolute, contains "..", a
    // tab or a newline, is not a directory, is already listed, or the list is full — the caller
    // shows that string rather than inventing its own, so the reason a folder was refused is the
    // reason it was actually refused.
    bool add(const std::string &dir, std::string &error);

    // Removes a root. False if it was not listed.
    bool remove(const std::string &dir);

    const std::vector<std::string> &roots() const
    {
        return mRoots;
    }
    bool empty() const
    {
        return mRoots.empty();
    }

private:
    std::vector<std::string> mRoots;
};

} // namespace NAMp::host
