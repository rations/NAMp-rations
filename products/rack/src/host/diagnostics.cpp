// Diagnostics arming. See diagnostics.h.

#include "diagnostics.h"

#include <cstdlib>
#include <cstring>

namespace NAMp::host
{

namespace
{

// Resolved on the first call, which is during setup, never on the audio thread. A function-local
// static's guard is only taken on the first pass; afterwards this is a plain load.
bool resolve()
{
    const char *v = std::getenv("NAMPRACK_DIAG");
    if (!v || v[0] == 0)
        return false;
    return std::strcmp(v, "0") != 0;
}

} // namespace

//------------------------------------------------------------------------
bool diagArmed()
{
    static const bool armed = resolve();
    return armed;
}

} // namespace NAMp::host
