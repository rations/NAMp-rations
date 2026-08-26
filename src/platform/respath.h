// Locating the plug-in's own resources at runtime.
//
// A VST3 bundle keeps its art and fonts in
//   <name>.vst3/Contents/Resources/{img,fonts}
// and the loaded module is
//   <name>.vst3/Contents/x86_64-linux/<name>.so     (Linux)
//   <name>.vst3/Contents/x86_64-win/<name>.vst3     (Windows)
// so the resource directory is two levels up from the loaded module, plus
// "Resources". The architecture directory is never named here — going up twice
// works for both layouts, and for any future one.
//
// Unlike an application, a plug-in cannot ask for "its own executable": the
// host's binary is not ours. The module's own path therefore comes from
// dladdr() on a symbol we own, or GetModuleHandleEx(FROM_ADDRESS) on Windows.
//
// Resolution order:
//   1. $RATIONS_RESOURCE_DIR, if set (development and packaging override);
//   2. the bundle layout above, derived from the module's own path;
//   3. an executable-relative "resources" directory, which is what the
//      standalone build uses when run from a build tree.
//
// Returns an empty string if nothing resolves; callers treat that as "no art",
// which every load path already degrades gracefully for.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Rations
{

// Cached after the first call.
const std::string &resourceDir();

// A UTF-8 string as a std::filesystem::path, without ever throwing.
//
// Constructing a path from a narrow string is not a safe operation on Windows.
// libstdc++ treats the narrow form as UTF-8 and THROWS
// std::filesystem::filesystem_error when the bytes are not valid UTF-8 —
// verified by running it, on a lone 0xE9, a truncated multi-byte sequence and
// a 0xFF. Passing an std::error_code does not help, because the throw happens
// while the path is being built, before any filesystem call sees it.
//
// Every path this plug-in acts on is untrusted (RULES.md section 3): a state
// blob written into a project file, a path handed over by a host through
// INampFileLoader, an environment override. Without this, one bad byte in a
// saved project would leave the host with an uncaught exception out of a worker
// thread or out of its own message loop.
//
// On POSIX no conversion happens and this can never fail.
//
// Returns false and leaves `out` empty for a string this platform cannot
// express as a path; callers treat that as "no such file", which every load
// path already degrades gracefully for.
bool utf8ToPath(const std::string &s, std::filesystem::path &out);

// The last component of a path: "MyAmp" from "/home/me/captures/MyAmp", and
// "cab.wav" from "C:\Users\me\IRs\cab.wav".
//
// This is what the editor's loader rows show. They are one line wide and a
// user's capture folder can sit arbitrarily deep, so the row shows the folder's
// own name — plus the capture count for a bank — rather than the path.
//
// std::filesystem, not find_last_of('/'), because the separator is not the same
// on both platforms. Windows paths come back from fs::path::string() with '\',
// which a search for '/' never finds, so the rows showed the user's entire
// path. Searching for both characters instead would be wrong in the other
// direction: a backslash in a POSIX filename is an ordinary character and must
// not split the name. That distinction is exactly what fs::path knows and a
// character search cannot.
//
// A trailing separator is handled, because a directory chosen in the browser
// can carry one and fs::path::filename() is empty for those. A path with no
// shorter form than itself — a root — is returned unchanged, as is one this
// platform cannot express as a path at all.
std::string pathBaseName(const std::string &path);

// Read a whole file into memory. `path` is UTF-8.
//
// Every resource loader goes through this rather than through the path-taking
// entry point its own library offers — cairo_image_surface_create_from_png,
// FT_New_Face, nsvgParseFromFile. Those all hand the narrow string to fopen(),
// which on Windows interprets it in the process's ANSI code page, so a bundle
// installed under a path the ACP cannot represent (any non-Latin-1 character
// under the usual 1252) would silently fail to find its own art and fonts and
// fall back to flat rectangles. std::ifstream over a std::filesystem::path uses
// the wide Win32 call underneath and has no such limit.
//
// Every one of those libraries already has a memory-taking entry point that the
// embedded-resource fallback uses, so this costs nothing but the read itself.
//
// Returns false on any failure, leaving `out` unspecified; callers already
// degrade gracefully, because a missing resource directory is a normal state.
bool readFileBytes(const std::string &path, std::vector<unsigned char> &out);

} // namespace Rations
