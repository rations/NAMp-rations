// Flush-to-zero / denormals-are-zero for the audio thread.
//
// This is re-armed before EVERY node in the chain, not once per block. A plug-in is free to change
// the FPU control register and not restore it — some do, deliberately, and some do it by accident
// through a library they link. Re-arming per node means one misbehaving pedal cannot leave the rest
// of the chain running with subnormals, where a NAM feedback path or a reverb tail stalls the CPU
// badly enough to blow the deadline.
//
// The cost is a register read and at most one write, which is why it is affordable per node.
//
// The plug-in's own processor arms the same bits at the top of its process() for the same reason
// (see the comment in src/rationsprocessor.cpp) — JACK does not set them on client process threads
// and a host is not required to.
//
// Which register, and which bits in it, is platform/rtdenormal.h's business: this wrapper exists so
// the chain code has a name of its own for the operation, not because it knows anything about it.

#pragma once

#include "platform/rtdenormal.h"

namespace NAMp::host
{

//------------------------------------------------------------------------
inline void rtSetDenormalMode()
{
    Rations::rtSetDenormalMode();
}

} // namespace NAMp::host
