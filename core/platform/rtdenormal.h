// Flush-to-zero / denormals-are-zero for a real-time audio thread.
//
// This is the ONE place in the tree that knows how an instruction set is asked to stop producing
// subnormals. Every caller wraps it under its own name and nothing else reaches for an intrinsic.
//
// It is re-armed inside process(), not once at activation, and in the rack it is re-armed again
// after every node in the chain. A host is not required to set these bits on its audio threads,
// it may call process() on a different thread than the one that activated the plug-in, and JACK
// does not set them on client process threads. A hosted plug-in is free to change them and not
// restore them -- some do deliberately, and some do it by accident through a library they link.
// Subnormals in a NAM feedback path, a filter or a reverb tail stall the FPU badly enough to blow
// the deadline, so leaving one node's carelessness to poison the rest of the chain is not an
// option. The cost is two register writes, which is what makes per-node affordable.
//
// x86-64 needs two bits and they are not the same bit:
//   FTZ  flushes subnormal RESULTS to zero
//   DAZ  treats subnormal INPUTS as zero
//
// AArch64 needs one. FPCR bit 24 (FZ) does both jobs at once -- there is no separate DAZ control,
// and arming FZ alone is the complete equivalent of the x86 pair rather than half of it. Measured
// rather than assumed: with FZ clear, a subnormal input multiplied up into normal range keeps its
// value, and with FZ set the same multiply yields exactly zero, which can only happen if the input
// was flushed before the multiply. Subnormal results are flushed in both directions of that test.
//
// The working reference for the register access is the vendored WDL denormal header carried by the
// pedals submodule, which does the same mrs/msr pair against the same bit 24 mask. The bit's
// position is corroborated by the AArch64 FPCR layout in the C library's own fpu_control.h, where
// FZ sits at bit 24 and is absent from the reserved mask, i.e. it is a writable control bit.
//
// A word on the fallback arm. Before this header existed, every site guarded on __SSE__ and
// compiled to an EMPTY function everywhere else, so the real-time contract was silently unmet on
// any non-x86 target -- which is exactly the failure this header was written to end. The fallback
// is still a no-op because there is nothing correct to put there, but it now says so in one place
// and sets NAMP_RT_DENORMAL_ARMED to 0, so a build that cares can assert on it instead of
// discovering the gap by ear.

#pragma once

#if defined(__SSE__) || defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define NAMP_RT_DENORMAL_ARMED 1
#define NAMP_RT_DENORMAL_X86 1
#elif defined(__aarch64__)
#define NAMP_RT_DENORMAL_ARMED 1
#define NAMP_RT_DENORMAL_AARCH64 1
#else
#define NAMP_RT_DENORMAL_ARMED 0
#endif

namespace Rations
{

//------------------------------------------------------------------------
inline void rtSetDenormalMode()
{
#if defined(NAMP_RT_DENORMAL_X86)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(NAMP_RT_DENORMAL_AARCH64)
    // FPCR bit 24 (FZ). Read-modify-write, because every other bit in FPCR belongs to someone
    // else -- the rounding mode above all -- and a blind write would silently reset it. The write
    // is skipped when the bit is already set, which is what the reference implementation named
    // above does too; on this register that is the common case, since the only caller that arms it
    // repeatedly is the rack re-arming per node.
    unsigned long fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    const unsigned long armed = fpcr | (1UL << 24);
    if (armed != fpcr) {
        __asm__ __volatile__("msr fpcr, %0" : : "r"(armed));
    }
#endif
}

} // namespace Rations
