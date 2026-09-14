// The platform seam: which run loop the standalone runs on.
//
// Everything above this line — the feedback pump, the rack tick, the chain collector, the hosted
// editors' idle pass — is written against ONE small contract and knows nothing about the windowing
// system underneath it:
//
//   run()                          pump until stop()
//   stop()                         end the loop; safe from a signal handler
//   registerTimer(handler, ms)     periodic callback on the loop's own thread
//   unregisterTimer(handler)       remove every timer that handler holds
//
// The two implementations are genuinely different shapes rather than one file with #ifdefs in it,
// and the difference is the reason this seam exists at all:
//
//   X11EventLoop    IS the loop. A VST3 plug-in on Linux owns no event loop, so the host must
//                   provide one and hand it over as Steinberg::Linux::IRunLoop — an X connection, a
//                   select() over it and over every descriptor a plug-in registers, and a timer
//                   list this project keeps itself. Both the run loop and the frame that hands it
//                   out are obligations here.
//
//   Win32EventLoop  is a pump over the loop Windows already runs. Messages reach a window procedure
//                   by way of the system, timers are a kernel service, and there is no IRunLoop to
//                   implement or to hand out — the SDK's own Win32 editor-host sample is a plain
//                   GetMessage / DispatchMessage loop that never provides one.
//
// SO THE TWO DO NOT IMPLEMENT THE SAME INTERFACES, only the same four verbs, which is why this is a
// type alias rather than an abstract base class. Only one of the pair is ever compiled; both are
// built by the phase gate, on this machine, which is what keeps the contract above from drifting on
// the platform nobody is looking at today.
//
// This is the same seam, spelled the same way, that the plug-in's own editor already uses to choose
// which IPlugView base it is built on. See nativewindow.h for its companion, which does the same
// job for windows.

#pragma once

#include "pluginterfaces/base/fplatform.h"

#if SMTG_OS_WINDOWS
#include "win32eventloop.h"
#else
#include "x11eventloop.h"
#endif

namespace Rations
{

#if SMTG_OS_WINDOWS
using EventLoop = Win32EventLoop;
#else
using EventLoop = X11EventLoop;
#endif

} // namespace Rations
