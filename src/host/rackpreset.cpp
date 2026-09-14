// RackPreset implementation. See rackpreset.h for the file format, the state-versus-parameters
// rule and the untrusted-input contract.

#include "rackpreset.h"
#include "scancache.h" // escapeField / unescapeField, already the tree's line-field convention

#include <algorithm>
#include <cmath>
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

constexpr char kHeaderPrefix[] = "#NAMPRACK ";

constexpr char kSectionPre[] = "PRE";
constexpr char kSectionPost[] = "POST";

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

//------------------------------------------------------------------------
// Reverse alphabet, built once. 0xff marks "not a base64 character", which is what makes rejecting
// a hostile blob a table lookup rather than a chain of range tests.
const unsigned char *b64Reverse()
{
    static unsigned char table[256];
    static const bool built = [] {
        std::memset(table, 0xff, sizeof(table));
        for (unsigned char i = 0; i < 64; ++i)
            table[static_cast<unsigned char>(kB64[i])] = i;
        return true;
    }();
    (void)built;
    return table;
}

//------------------------------------------------------------------------
// Split "key rest-of-line" at the first space. A line with no space is a key with an empty value,
// which every field below then rejects on its own terms.
void splitField(const std::string &line, std::string &key, std::string &value)
{
    const size_t space = line.find(' ');
    if (space == std::string::npos) {
        key = line;
        value.clear();
        return;
    }
    key = line.substr(0, space);
    value = line.substr(space + 1);
}

//------------------------------------------------------------------------
// strtod/strtol with the whole-field requirement the standard versions do not have: trailing
// rubbish means the field is malformed, not that the leading digits were what was meant.
bool parseDouble(const std::string &text, double &out)
{
    if (text.empty() || text.size() > 64)
        return false;
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || !end || *end != 0)
        return false;
    out = v;
    return true;
}

bool parseIndex(const std::string &text, long &out)
{
    if (text.empty() || text.size() > 20)
        return false;
    char *end = nullptr;
    errno = 0;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != 0 || v < 0)
        return false;
    out = v;
    return true;
}

//------------------------------------------------------------------------
std::string formatDouble(double v)
{
    // 17 significant digits is what round-trips an IEEE double exactly, which is what a preset
    // claiming to reproduce the same audio has to do.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

} // namespace

//========================================================================
// base64
//------------------------------------------------------------------------
std::string base64Encode(const uint8_t *data, size_t len)
{
    std::string out;
    if (!data || len == 0)
        return out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                                (static_cast<uint32_t>(data[i + 1]) << 8) |
                                static_cast<uint32_t>(data[i + 2]);
        out += kB64[(triple >> 18) & 0x3f];
        out += kB64[(triple >> 12) & 0x3f];
        out += kB64[(triple >> 6) & 0x3f];
        out += kB64[triple & 0x3f];
    }

    if (i + 1 == len) {
        const uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        out += kB64[(triple >> 18) & 0x3f];
        out += kB64[(triple >> 12) & 0x3f];
        out += "==";
    } else if (i + 2 == len) {
        const uint32_t triple =
            (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        out += kB64[(triple >> 18) & 0x3f];
        out += kB64[(triple >> 12) & 0x3f];
        out += kB64[(triple >> 6) & 0x3f];
        out += '=';
    }
    return out;
}

//------------------------------------------------------------------------
bool base64Decode(const std::string &text, std::vector<uint8_t> &out, size_t maxOut)
{
    out.clear();
    if (text.empty())
        return true;
    if (text.size() % 4 != 0)
        return false;

    // Checked before decoding rather than while appending, so a blob designed to be enormous is
    // refused without ever being materialised.
    const size_t groups = text.size() / 4;
    if (groups > maxOut / 3 + 1)
        return false;

    const unsigned char *reverse = b64Reverse();
    out.reserve(groups * 3);

    for (size_t g = 0; g < groups; ++g) {
        const char *quad = text.data() + g * 4;
        int pad = 0;
        uint32_t bits = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = quad[k];
            if (c == '=') {
                // Padding is only legal in the last group, and only in its last two positions.
                if (g + 1 != groups || k < 2)
                    return false;
                ++pad;
                bits <<= 6;
                continue;
            }
            if (pad != 0)
                return false; // a character after padding
            const unsigned char v = reverse[static_cast<unsigned char>(c)];
            if (v == 0xff)
                return false;
            bits = (bits << 6) | v;
        }

        out.push_back(static_cast<uint8_t>((bits >> 16) & 0xff));
        if (pad < 2)
            out.push_back(static_cast<uint8_t>((bits >> 8) & 0xff));
        if (pad < 1)
            out.push_back(static_cast<uint8_t>(bits & 0xff));
    }

    if (out.size() > maxOut) {
        out.clear();
        return false;
    }
    return true;
}

//========================================================================
// Text
//------------------------------------------------------------------------
std::string writeRackPreset(const RackPreset &preset)
{
    std::string out;
    out += kHeaderPrefix;
    out += std::to_string(kRackPresetVersion);
    out += '\n';

    for (const PresetNode &node : preset.nodes) {
        out += "node ";
        out += (node.section == ChainSection::Pre) ? kSectionPre : kSectionPost;
        out += '\n';

        out += "format ";
        out += node.formatTag.empty() ? "?" : node.formatTag;
        out += '\n';

        // The key carries a unit separator for VST3 (pluginref.h). It is left alone: it is not a
        // newline, so it cannot break the line structure, and escaping it would make the file less
        // readable rather than more.
        out += "key " + escapeField(node.ref.key) + '\n';
        out += "name " + escapeField(node.name) + '\n';
        out += "enabled ";
        out += node.enabled ? "1\n" : "0\n";
        out += "mix " + formatDouble(node.mix) + '\n';

        for (size_t i = 0; i < node.params.size(); ++i)
            out += "param " + std::to_string(i) + ' ' + formatDouble(node.params[i]) + '\n';

        if (!node.state.empty())
            out += "state " + base64Encode(node.state.data(), node.state.size()) + '\n';

        out += "end\n";
    }
    return out;
}

//------------------------------------------------------------------------
bool parseRackPreset(const char *text, size_t len, RackPreset &out, std::string &warnings)
{
    out.nodes.clear();
    warnings.clear();

    if (!text || len == 0)
        return false;
    if (len > kMaxPresetBytes) {
        warnings = "the file is implausibly large for a rack preset";
        return false;
    }

    auto warn = [&warnings](const std::string &message) {
        if (!warnings.empty())
            warnings += "; ";
        warnings += message;
    };

    size_t at = 0;
    auto nextLine = [&](std::string &line) {
        if (at >= len)
            return false;
        const char *start = text + at;
        const void *nl = std::memchr(start, '\n', len - at);
        const size_t count =
            nl ? static_cast<size_t>(static_cast<const char *>(nl) - start) : (len - at);
        at += nl ? count + 1 : count;
        // A line longer than the cap is not truncated and used, it is refused: a truncated base64
        // blob decodes to something, and something is worse than nothing here.
        if (count > kMaxPresetLine)
            return false;
        line.assign(start, count);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        return true;
    };

    std::string line;
    if (!nextLine(line) || line.compare(0, std::strlen(kHeaderPrefix), kHeaderPrefix) != 0) {
        warnings = "not a rack preset (no #NAMPRACK header)";
        return false;
    }
    long version = 0;
    if (!parseIndex(line.substr(std::strlen(kHeaderPrefix)), version) ||
        version != kRackPresetVersion) {
        // Unlike the scan cache, this is not regenerable, so an unreadable version says so instead
        // of quietly starting empty.
        warnings = "rack preset version " + line.substr(std::strlen(kHeaderPrefix)) +
                   " is not one this build can read";
        return false;
    }

    PresetNode node;
    bool inNode = false;
    // Counted separately from out.nodes.size() so a file full of malformed records still
    // terminates rather than being read to the end looking for a good one.
    size_t records = 0;

    while (nextLine(line)) {
        if (line.empty() || line[0] == '#')
            continue;

        std::string key, value;
        splitField(line, key, value);

        if (key == "node") {
            if (inNode)
                warn("a node record was not closed with 'end'");
            if (++records > static_cast<size_t>(kMaxChainNodes) * 2) {
                warn("more nodes than a chain can hold; the rest were dropped");
                break;
            }
            node = PresetNode();
            node.section = (value == kSectionPost) ? ChainSection::Post : ChainSection::Pre;
            if (value != kSectionPre && value != kSectionPost)
                warn("a node named an unknown section and was put before the amp");
            inNode = true;
            continue;
        }

        if (!inNode)
            continue; // a field outside a record: ignored, per the forward-compatibility rule

        if (key == "end") {
            inNode = false;
            if (node.ref.key.empty()) {
                warn("a node with no key was dropped");
                continue;
            }
            out.nodes.push_back(std::move(node));
            node = PresetNode();
            continue;
        }

        if (key == "format") {
            node.formatTag = value;
            node.formatKnown = formatFromTag(value.c_str(), node.ref.format);
        } else if (key == "key") {
            std::string decoded;
            if (unescapeField(value, decoded) && !decoded.empty() &&
                decoded.size() <= kMaxPresetNameLength)
                node.ref.key = std::move(decoded);
            else
                warn("a node's key was malformed and the node was dropped");
        } else if (key == "name") {
            std::string decoded;
            if (unescapeField(value, decoded) && decoded.size() <= kMaxPresetNameLength)
                node.name = std::move(decoded);
        } else if (key == "enabled") {
            node.enabled = (value != "0");
        } else if (key == "mix") {
            double v = 0.0;
            if (parseDouble(value, v))
                node.mix = static_cast<float>(std::clamp(v, 0.0, 1.0));
        } else if (key == "param") {
            std::string indexText, valueText;
            splitField(value, indexText, valueText);
            long index = 0;
            double v = 0.0;
            if (!parseIndex(indexText, index) || !parseDouble(valueText, v) ||
                static_cast<size_t>(index) >= kMaxPresetParams) {
                warn("a malformed parameter line was dropped");
            } else {
                // Indices are written in order but are not required to arrive that way; anything
                // never mentioned stays at the plug-in's own default.
                if (node.params.size() <= static_cast<size_t>(index))
                    node.params.resize(static_cast<size_t>(index) + 1, -1.0);
                node.params[static_cast<size_t>(index)] = std::clamp(v, 0.0, 1.0);
            }
        } else if (key == "state") {
            if (!base64Decode(value, node.state, kMaxPresetStateBytes)) {
                node.state.clear();
                warn("a node's saved state was malformed and was ignored");
            }
        }
        // Anything else is a field from a later build: ignored on purpose.
    }

    if (inNode)
        warn("the file ended inside a node record");
    return true;
}

//========================================================================
// The live chain
//------------------------------------------------------------------------
bool captureRack(const ChainBuilder &builder, const std::string &stateDir, RackPreset &out)
{
    out.nodes.clear();

    int nodeIndex = 0;
    for (const auto section : {ChainSection::Pre, ChainSection::Post}) {
        const int count = builder.count(section);
        for (int i = 0; i < count; ++i) {
            ChainNodeInfo info;
            if (!builder.nodeInfo(section, i, info))
                continue;

            PresetNode node;
            node.section = section;
            node.ref = info.ref;
            node.formatTag = formatTag(info.ref.format);
            node.formatKnown = true;
            node.name = info.name;
            node.enabled = info.enabled;
            node.mix = info.mix;

            if (PluginBackend *backend = builder.backend(section, i)) {
                // Before anything is read out of the plug-in. Until this call its own state does
                // not necessarily carry an edit the user has just made — see
                // PluginBackend::paramFlushToPlugin, which exists because of this exact line.
                backend->paramFlushToPlugin();

                const uint32_t params = backend->paramCount();
                const uint32_t capped =
                    params > kMaxPresetParams ? static_cast<uint32_t>(kMaxPresetParams) : params;
                node.params.reserve(capped);
                for (uint32_t p = 0; p < capped; ++p)
                    node.params.push_back(backend->paramGet(p));

                // Each node gets its own directory so two plug-ins that both save "ir.wav" cannot
                // overwrite each other's file.
                if (!stateDir.empty()) {
                    const std::string nodeDir = stateDir + "/n" + std::to_string(nodeIndex) + "-" +
                                                formatTag(info.ref.format);
                    std::error_code ec;
                    std::filesystem::create_directories(nodeDir, ec);
                    backend->stateSetDirectory(ec ? nullptr : nodeDir.c_str());
                }
                if (!backend->stateSave(node.state))
                    node.state.clear();
                backend->stateSetDirectory(nullptr);

                if (node.state.size() > kMaxPresetStateBytes) {
                    std::fprintf(
                        stderr,
                        "namp-rack: %s wants to save %zu bytes of state, which is more than a "
                        "rack preset carries; its parameters were saved instead\n",
                        info.name.c_str(), node.state.size());
                    node.state.clear();
                }
            }

            out.nodes.push_back(std::move(node));
            ++nodeIndex;
        }
    }
    return true;
}

//------------------------------------------------------------------------
bool applyRack(ChainBuilder &builder, const RackPreset &preset, const std::string &stateDir,
               ApplyReport &report)
{
    report = ApplyReport();

    // Emptied by removal rather than by clear(): remove() buries each instance for the collector,
    // which is what makes this safe to call while the audio thread is running a snapshot that
    // still names them. clear() destroys them on the spot and is for shutdown only.
    for (const auto section : {ChainSection::Pre, ChainSection::Post}) {
        while (builder.count(section) > 0)
            builder.remove(section, 0);
    }

    int nodeIndex = 0;
    for (const PresetNode &node : preset.nodes) {
        const std::string label = node.name.empty() ? node.ref.key : node.name;

        if (!node.formatKnown || !node.ref.valid()) {
            std::fprintf(
                stderr,
                "namp-rack: '%s' is a %s plug-in, which this build cannot host; it was kept as "
                "a placeholder\n",
                label.c_str(), node.formatTag.empty() ? "?" : node.formatTag.c_str());
            if (builder.addPlaceholder(node.section, node.ref, label) >= 0)
                ++report.placeholders;
            else
                ++report.skipped;
            ++nodeIndex;
            continue;
        }

        std::string error;
        const int index = builder.add(node.section, node.ref, error);
        if (index < 0) {
            std::fprintf(stderr, "namp-rack: %s: %s; kept as a placeholder\n", label.c_str(),
                         error.c_str());
            if (builder.addPlaceholder(node.section, node.ref, label) >= 0)
                ++report.placeholders;
            else
                ++report.skipped;
            ++nodeIndex;
            continue;
        }

        // State after the instance exists and before the chain is published — the window
        // ChainBuilder::loadNodeState documents, and the only window in which an LV2 plug-in has
        // both an instance to restore into and no audio thread reading it.
        bool stateApplied = false;
        if (!node.state.empty()) {
            std::string nodeDir;
            if (!stateDir.empty()) {
                nodeDir = stateDir + "/n" + std::to_string(nodeIndex) + "-" + node.formatTag;
                std::error_code ec;
                if (!std::filesystem::is_directory(nodeDir, ec))
                    nodeDir.clear();
            }
            stateApplied =
                builder.loadNodeState(node.section, index, node.state.data(), node.state.size(),
                                      nodeDir.empty() ? nullptr : nodeDir.c_str());
            if (!stateApplied)
                std::fprintf(stderr,
                             "namp-rack: %s rejected its saved state; its parameters were used "
                             "instead\n",
                             label.c_str());
        }

        // The parameter list is the fallback AND the check. Applying it blindly on top of a good
        // state restore would be wrong — see the ordering note in the header — but leaving it
        // unused whenever stateLoad() merely RETURNS TRUE is worse, because a plug-in that accepts
        // a blob and quietly ignores it is not hypothetical: two of the Windows plug-ins bridged on
        // this machine lose every parameter to its default under their own in-memory save/restore,
        // which `namp_hostcheck --state` demonstrates with no file and no host code in the way.
        // The knob positions in the file are still right, so they are put back.
        PluginBackend *backend = builder.backend(node.section, index);
        const uint32_t params = backend ? backend->paramCount() : 0;
        int repaired = 0;
        for (size_t p = 0; p < node.params.size() && p < params; ++p) {
            if (node.params[p] < 0.0)
                continue;
            ParamInfo info;
            if (backend->paramInfo(static_cast<uint32_t>(p), info) && info.isReadOnly)
                continue;
            // Only where they actually disagree, and only past the plug-in's own quantisation
            // grid: a state restore that landed on 0.58125 for a requested 0.580933 did its job,
            // and overwriting it would be the host arguing with the plug-in over a rounding.
            if (stateApplied && std::fabs(backend->paramGet(static_cast<uint32_t>(p)) -
                                          node.params[p]) <= kParamGridTolerance)
                continue;
            backend->paramSetFromUi(static_cast<uint32_t>(p), node.params[p]);
            ++repaired;
        }
        if (repaired > 0) {
            // Pushed in now rather than left in the ring, so the node is correct before it is
            // published and a save made immediately afterwards records the right thing.
            backend->paramFlushToPlugin();
            if (stateApplied)
                std::fprintf(
                    stderr,
                    "namp-rack: %s ignored %d of the settings in its own saved state; they "
                    "were restored from the preset's parameter list instead\n",
                    label.c_str(), repaired);
        }

        builder.setEnabled(node.section, index, node.enabled);
        builder.setMix(node.section, index, node.mix);
        ++report.loaded;
        ++nodeIndex;
    }

    builder.publish();
    return true;
}

//========================================================================
// Files
//------------------------------------------------------------------------
std::string rackDir()
{
#if defined(_WIN32)
    // %LOCALAPPDATA%, beside the scan cache and the search-path list — see defaultCachePath() in
    // scancache.cpp for why that variable and not a roaming one. A saved rack names the absolute
    // paths of the plug-ins it loads, so it has no meaning on another machine either.
    if (const char *local = std::getenv("LOCALAPPDATA"); local && *local)
        return std::string(local) + "\\NAMp-Rack\\racks";
    return {};
#else
    std::string base;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char *home = std::getenv("HOME"); home && *home)
        base = std::string(home) + "/.config";
    else
        return {};
    return base + "/NAMp-Rack/racks";
#endif
}

//------------------------------------------------------------------------
bool rackNameIsSafe(const std::string &name)
{
    if (name.empty() || name.size() > 64)
        return false;
    if (name[0] == '.')
        return false;
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

//------------------------------------------------------------------------
std::string rackPath(const std::string &name)
{
    const std::string dir = rackDir();
    if (dir.empty() || !rackNameIsSafe(name))
        return {};
#if defined(_WIN32)
    // The native separator, so the whole path reads as one Windows path rather than as a mixture.
    // Win32 accepts either, but the directory above came back with backslashes and a saved rack's
    // path is shown to the user.
    return dir + "\\" + name + ".namprack";
#else
    return dir + "/" + name + ".namprack";
#endif
}

//------------------------------------------------------------------------
std::vector<std::string> listRacks()
{
    std::vector<std::string> names;
    const std::string dir = rackDir();
    if (dir.empty())
        return names;

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec)
        return names;

    for (const auto &entry : it) {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc)
            continue;
        const std::string filename = entry.path().filename().string();
        const std::string suffix = ".namprack";
        if (filename.size() <= suffix.size() ||
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const std::string name = filename.substr(0, filename.size() - suffix.size());
        if (rackNameIsSafe(name))
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

//------------------------------------------------------------------------
std::string rackStateDir(const std::string &presetPath)
{
    if (presetPath.empty())
        return {};
    return presetPath + ".d";
}

//------------------------------------------------------------------------
bool saveRack(const ChainBuilder &builder, const std::string &path, std::string &error)
{
    error.clear();
    if (path.empty()) {
        error = "no preset path";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) {
        error = "cannot create " + std::filesystem::path(path).parent_path().string();
        return false;
    }

    const std::string stateDir = rackStateDir(path);
    RackPreset preset;
    captureRack(builder, stateDir, preset);
    const std::string text = writeRackPreset(preset);

    // Written to a sibling and renamed, so an interrupted save leaves the previous rack intact
    // rather than a half-written file that parses to a shorter chain.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + temp;
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            error = "short write to " + temp;
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        error = "cannot replace " + path;
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
bool loadRack(ChainBuilder &builder, const std::string &path, ApplyReport &report,
              std::string &error)
{
    report = ApplyReport();
    error.clear();
    if (path.empty()) {
        error = "no preset path";
        return false;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "no such rack preset: " + path;
        return false;
    }
    if (size > kMaxPresetBytes) {
        error = path + " is implausibly large for a rack preset";
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    RackPreset preset;
    std::string warnings;
    if (!parseRackPreset(text.data(), text.size(), preset, warnings)) {
        error = warnings.empty() ? ("cannot parse " + path) : warnings;
        return false;
    }
    if (!warnings.empty())
        std::fprintf(stderr, "namp-rack: %s: %s\n", path.c_str(), warnings.c_str());

    return applyRack(builder, preset, rackStateDir(path), report);
}

} // namespace NAMp::host
