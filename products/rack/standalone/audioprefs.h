// AudioPrefs — which audio device the user chose, and the file it persists to.
//
// A DEVICE CHOICE THAT DOES NOT SURVIVE A RESTART IS NOT A CHOICE. The picker in the rack strip is
// what sets these; this is what makes the setting mean something the next time the amp is opened,
// which on Windows is the difference between a working instrument and one that has to be re-aimed
// at the interface every session.
//
// CONFIG, NOT CACHE, and it sits beside the plug-in search paths for the same reason: losing the
// scan cache costs one rescan, losing this loses something the user chose. The file format, the
// caps and the escaping are all the search-path list's, deliberately — two config files in one
// program that parsed differently would be two sets of bugs.
//
// UNTRUSTED ON THE WAY IN, exactly like every other file this program reads. It is bounded in total
// size and per line, an unknown header version is discarded whole rather than migrated, and a line
// that does not parse is skipped with the rest of the file still honoured. A device id that no
// longer names anything is NOT rejected here — the backend falls back to the system default and
// says so, because an interface that is merely unplugged today is one the user still wants
// tomorrow.
//
// EVERY FIELD IS OPTIONAL AND AN EMPTY ONE MEANS "THE DEFAULT". That is what a first run gets, and
// it is also what a partially-written file degrades to.

#pragma once

#include <cstddef>
#include <string>

namespace Rations
{

//------------------------------------------------------------------------
struct AudioPrefs {
    // Caps, for the reason the search-path list has them: a corrupt or hostile file must not make
    // this program allocate without bound before it has drawn a window.
    static constexpr size_t kMaxFileBytes = 64u * 1024u;
    static constexpr size_t kMaxLineBytes = 4096;
    static constexpr size_t kMaxValueBytes = 1024;

    // "auto", "asio" or "wasapi". Anything else, including empty, reads as "auto" — which is ASIO
    // where there is a driver and WASAPI where there is not. Stored as a word rather than a number
    // so that a person reading the file can see what it says.
    std::string backend;
    // The ASIO driver's registered name, which is both its id and what the user recognises.
    std::string asioDriver;
    // WASAPI endpoint ids. Opaque strings from the system; empty means the system default.
    std::string captureDevice;
    std::string renderDevice;
    // Exclusive mode is asked for first. Turning it off is a deliberate choice to share the device
    // with the rest of the machine at the cost of the mixer's latency.
    bool exclusive = true;

    // $XDG_CONFIG_HOME/NAMp-Rack/audio, else $HOME/.config/NAMp-Rack/audio, and on Windows
    // %LOCALAPPDATA%\NAMp-Rack\audio. Empty when no variable resolves, in which case the choice is
    // usable for the session and simply not persisted.
    static std::string defaultFile();

    // A missing file is a first run, not an error. False only when the file existed and could not
    // be read or carried an unknown version, which is worth one warning.
    bool load(const std::string &file);
    // Writes the file, creating the directory if need be. False on any failure.
    bool save(const std::string &file) const;
};

} // namespace Rations
