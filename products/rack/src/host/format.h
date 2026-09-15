// PluginFormat — which plug-in standard a hosted node speaks.
//
// The enumerator value is an in-memory dispatch index and nothing else. What gets written to a
// saved chain is the ASCII tag: "VST3", "LV2", "VST2".
//
// That distinction is deliberate. The parent project persists the raw enum int and defends it with
// a comment saying the values must never be reordered — a comment guarding an invariant instead of
// a design that cannot break it. Persisting the tag means reordering this enum is a non-event, a
// saved chain is readable in a text editor, and a chain written by a newer build that names a
// format this build has never heard of degrades to a named placeholder rather than silently
// aliasing onto whichever backend happens to sit at that index.
//
// Adding a format is four edits and no more: an enumerator here, its tag in the table below, a
// backend .cpp, and one case in createBackend() at the top of chainbuilder.cpp. Nothing else in the
// tree switches on format.

#pragma once

#include <cstdint>
#include <cstring>

namespace NAMp::host
{

//------------------------------------------------------------------------
enum class PluginFormat : int32_t {
    Vst3 = 0,
    Lv2 = 1,
    Vst2 = 2,

    Count
};

//------------------------------------------------------------------------
// The persisted spelling. Never returns null: an out-of-range value yields "?" so a corrupt
// in-memory value cannot produce a null dereference in a log path.
inline const char *formatTag(PluginFormat f)
{
    switch (f) {
        case PluginFormat::Vst3:
            return "VST3";
        case PluginFormat::Lv2:
            return "LV2";
        case PluginFormat::Vst2:
            return "VST2";
        case PluginFormat::Count:
            break;
    }
    return "?";
}

//------------------------------------------------------------------------
// Parse a persisted tag. Returns false for anything unrecognised, which callers turn into a
// placeholder node rather than an error — see chainpreset.
inline bool formatFromTag(const char *tag, PluginFormat &out)
{
    if (!tag)
        return false;
    for (int32_t i = 0; i < static_cast<int32_t>(PluginFormat::Count); ++i) {
        const auto f = static_cast<PluginFormat>(i);
        if (std::strcmp(tag, formatTag(f)) == 0) {
            out = f;
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------
// Separator between the two halves of a composite key (see pluginref.h). ASCII unit separator
// rather than '\n' so a key never spans two lines of the scan cache.
constexpr char kKeySeparator = '\x1f';

} // namespace NAMp::host
