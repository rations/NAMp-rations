// Rations — a non-finite test that survives -ffast-math.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// WHY THIS IS NOT std::isfinite. The plug-in target is compiled with -ffast-math, which implies
// -ffinite-math-only, which licenses the compiler to assume that no NaN and no infinity ever
// occurs. Under that assumption std::isfinite(x) folds to the constant true and std::isnan(x) to
// false, so a guard written with either is deleted from the build. Measured with this machine's
// g++ at -O3 on a runtime-produced quiet NaN: std::isfinite returns 1 with -ffast-math on and 0
// with it off. The Boost's Newton-divergence guard was written as std::isfinite and was therefore
// inert from the day it was added; that is what this header exists to fix.
//
// The same flag makes the ordinary two-sided clamps in this tree absorb a NaN instead of passing
// it on, because GCC lowers `v < lo ? lo : (v > hi ? hi : v)` to minsd/maxsd, which return a fixed
// operand when either input is a NaN. That is luck, not a guarantee — it depends on the compiler's
// instruction selection at each call site — so it is not something to rely on, and not a reason to
// leave a real guard dead.
//
// -ffast-math is not removable here. The pedals' measured behaviour was recorded from a build that
// has it, so dropping it would change what they sound like — the one thing that must not move.
//
// So the test has to stay off the floating-point unit and read the bits. IEEE-754 binary64 says a
// value is an infinity or a NaN exactly when its eleven exponent bits are all ones, whatever the
// sign and the significand are, which is one mask and one integer compare. memcpy rather than a
// union or a pointer cast because it is the only spelling that is not a strict-aliasing violation,
// and every compiler this project uses turns it into a register move.
//
// NAMESPACE. This sits in Rations::pedals rather than in a Rations::dsp beside the rest of the
// numerics, because AudioDSPTools owns the GLOBAL namespace `dsp` and this tree names it that way
// in sixteen places — a `Rations::dsp` would shadow it from inside namespace Rations and silently
// re-point them. The sibling stompbox build, which has no AudioDSPTools in it, does put the same
// function in Rations::dsp; the body is identical and only the enclosing namespace differs.
#pragma once

#include <cstdint>
#include <cstring>

namespace Rations
{
namespace pedals
{

// True for an ordinary number or a denormal; false for +/-inf and for every NaN.
inline bool isFinite(double v)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return (bits & UINT64_C(0x7FF0000000000000)) != UINT64_C(0x7FF0000000000000);
}

} // namespace pedals
} // namespace Rations
