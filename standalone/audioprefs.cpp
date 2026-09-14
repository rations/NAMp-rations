// AudioPrefs implementation. See audioprefs.h.

#include "audioprefs.h"

#include "host/scancache.h" // escapeField / unescapeField, shared with the scan cache and the paths

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace Rations
{

namespace
{

constexpr char kHeader[] = "#NAMPAUDIO 1";

// One key per line, the key and its value separated by a tab, in the same escaped form the scan
// cache and the search-path list use. A key this build does not know is SKIPPED rather than
// refused: that is what lets a later version add a field without a file written by this one
// becoming unreadable, and it costs nothing here because every field is optional anyway.
bool splitLine(const std::string &line, std::string &key, std::string &value)
{
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
        return false;
    key = line.substr(0, tab);
    const std::string rawValue = line.substr(tab + 1);
    if (key.empty() || rawValue.size() > AudioPrefs::kMaxValueBytes)
        return false;
    return NAMp::host::unescapeField(rawValue, value);
}

} // namespace

//------------------------------------------------------------------------
std::string AudioPrefs::defaultFile()
{
#if defined(_WIN32)
    // %LOCALAPPDATA%, for the reason spelled out at defaultCachePath() in scancache.cpp: it is the
    // per-user, per-machine directory, and the name of an interface means nothing on another
    // machine.
    if (const char *local = std::getenv("LOCALAPPDATA"); local && local[0])
        return std::string(local) + "\\NAMp-Rack\\audio";
    return {};
#else
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0])
        return std::string(xdg) + "/NAMp-Rack/audio";
    if (const char *home = std::getenv("HOME"); home && home[0])
        return std::string(home) + "/.config/NAMp-Rack/audio";
    return {};
#endif
}

//------------------------------------------------------------------------
bool AudioPrefs::load(const std::string &file)
{
    *this = AudioPrefs();
    if (file.empty())
        return false;

    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec)
        return true; // a first run is not a failure

    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > kMaxFileBytes) {
        std::fprintf(stderr, "namp-rack: %s is missing or too large to be an audio setting\n",
                     file.c_str());
        return false;
    }

    std::ifstream in(file);
    if (!in)
        return false;

    std::string line;
    if (!std::getline(in, line) || line != kHeader) {
        // Same rule as the scan cache and the search-path list: an unknown version is discarded
        // whole rather than migrated. The cost is that the user picks their interface once more.
        std::fprintf(stderr, "namp-rack: %s is not a version this build understands; ignoring it\n",
                     file.c_str());
        return false;
    }

    while (std::getline(in, line)) {
        if (line.size() > kMaxLineBytes)
            continue;
        std::string key;
        std::string value;
        if (!splitLine(line, key, value))
            continue;

        if (key == "backend")
            backend = value;
        else if (key == "asio-driver")
            asioDriver = value;
        else if (key == "capture")
            captureDevice = value;
        else if (key == "render")
            renderDevice = value;
        else if (key == "exclusive")
            exclusive = value != "0";
        // Anything else: a key from a later version, skipped.
    }
    return true;
}

//------------------------------------------------------------------------
bool AudioPrefs::save(const std::string &file) const
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
    // where a valid one was — the same discipline the search-path list and saveRack() use.
    const std::string tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        out << kHeader << '\n';
        out << "backend\t" << NAMp::host::escapeField(backend) << '\n';
        out << "asio-driver\t" << NAMp::host::escapeField(asioDriver) << '\n';
        out << "capture\t" << NAMp::host::escapeField(captureDevice) << '\n';
        out << "render\t" << NAMp::host::escapeField(renderDevice) << '\n';
        out << "exclusive\t" << (exclusive ? "1" : "0") << '\n';
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

} // namespace Rations
