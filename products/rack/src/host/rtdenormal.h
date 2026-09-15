// Flush-to-zero / denormals-are-zero for the audio thread.
//
// This is re-armed before EVERY node in the chain, not once per block. A plug-in is free to change
// MXCSR and not restore it — some do, deliberately, and some do it by accident through a library
// they link. Re-arming per node means one misbehaving pedal cannot leave the rest of the chain
// running with subnormals, where a NAM feedback path or a reverb tail stalls the CPU badly enough
// to blow the deadline.
//
// The cost is two register writes, which is why it is affordable per node.
//
// The plug-in's own processor arms the same bits at the top of its process() for the same reason
// (see the comment in src/namprocessor.cpp) — JACK does not set them on client process threads and
// a host is not required to.

#pragma once

#if defined(__SSE__) || defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define NAMPRACK_HOST_HAVE_SSE_DENORMAL 1
#endif

namespace NAMp::host
{

//------------------------------------------------------------------------
inline void rtSetDenormalMode()
{
#ifdef NAMPRACK_HOST_HAVE_SSE_DENORMAL
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

} // namespace NAMp::host
