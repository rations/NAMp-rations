// State-stream string reads, shared between RationsProcessor::setState and
// RationsController::setComponentState — the two places this plug-in decodes a project's
// IBStream, which RULES.md treats as untrusted input: "A malformed state must produce a clean
// kResultFalse, never a crash inside the host."
//
// FStreamer::readStr8() (vst3sdk/base/source/fstreamer.cpp) is not safe to use on that input,
// in two ways neither call site can fix by using it more carefully:
//
//   - Its buffer comes from NEWSTR8, which is ::malloc (base/source/fstring.h). Every call site
//     here used to free the result with `delete[]`, which is undefined behaviour: malloc pairs
//     with free, not with delete[]. It happens to work on this machine's glibc; there is no
//     reason to expect that on the MinGW/msvcrt Windows build, and it is exactly the kind of
//     thing ASan or a different allocator turns into a crash.
//   - It never checks how many bytes its own readRaw() call actually got. `length` is trusted
//     unconditionally, so a blob truncated mid-string leaves the tail of the allocation
//     uninitialized with no guaranteed NUL inside it. Every call site here then did
//     `field = p` — a `char*`-to-`std::string` assignment that calls strlen() — which, on a
//     truncated blob, reads past the end of that allocation into whatever heap memory follows.
//     A malformed state file could crash the host or leak adjacent heap bytes into a path the
//     plug-in goes on to open, act on, and write back out through getState().
//
// This reads the identical wire format writeStr8() writes — int32 length (0 for an absent
// string), then `length` raw bytes being the content plus its trailing NUL, per FStreamer's own
// "len includes trailing zero" comment — but keeps the buffer as a bounds-known std::string
// throughout and never trusts a byte the stream did not actually deliver.
#pragma once

#include "base/source/fstreamer.h"

#include <string>

namespace Rations
{

// Reads one writeStr8()-encoded field into `out`. Returns true and leaves `out` empty for a
// legitimate absent string (length == 0) — that is not corruption, it is what an unfilled IR
// slot or an unnamed channel writes. Returns false, with `out` left untouched, for anything a
// well-formed blob could not have produced: a negative or implausible length (the same 262144
// ceiling FStreamer::readStr8() itself applies, so a blob that trips one trips the other
// identically), or a stream that ran out before delivering the bytes the length promised. The
// caller's job on false is the same as for every other field in these functions: propagate
// kResultFalse rather than adopt a value nothing actually read.
inline bool readStr8Checked(Steinberg::IBStreamer &streamer, std::string &out)
{
    using Steinberg::int32;
    using Steinberg::TSize;

    int32 length = 0;
    if (!streamer.readInt32(length))
        return false;
    if (length < 0 || length > 262144)
        return false;
    if (length == 0) {
        out.clear();
        return true;
    }

    std::string buf(static_cast<size_t>(length), '\0');
    const TSize got = streamer.readRaw(buf.data(), static_cast<TSize>(length));
    if (got != static_cast<TSize>(length))
        return false;

    // `length` counted the trailing NUL writeStr8() wrote; that byte is not part of the value.
    out.assign(buf.data(), static_cast<size_t>(length - 1));
    return true;
}

} // namespace Rations
