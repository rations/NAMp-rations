// The platform seam: which audio backend the standalone drives the device through.
//
// The standalone holds one AudioBackend reference and talks to nothing else — R6's seam, taken
// before there was a second platform to need it, so that the Windows port would be a new
// implementation rather than a refactor under a deadline. This header is where the one
// platform-dependent declaration lives, and the comment that used to say "the Windows build changes
// this declaration and nothing else" is now literally this file.
//
//   JackClient  on Linux. One driver, one callback, input and output on one clock. JACK also
//   changes
//               its buffer size under a running client at any other client's request, which is why
//               the interface has a buffer-size handshake at all.
//
//   WinAudio    on Windows, which is itself a choice between two: ASIO first because it is what
//               interface vendors ship and what the latency depends on, WASAPI when there is no
//               vendor driver to be had. See winaudio.h for the order and why the fallback exists
//               at all, and R16 in the project guidance for how WASAPI's two clocks are joined.
//
// THE ONE VERB THAT IS NOT ON AudioBackend is probeDefaults, and it is here rather than on the
// interface because it is asked BEFORE a backend exists to ask. The standalone has to tell
// setupProcessing a sample rate and a maximum block size before the component is activated, and the
// only honest source for those is the device — so each implementation answers from its own
// platform: JACK opens a throwaway client and reads two numbers off the server, ASIO asks the
// driver outright, WASAPI asks an activated but uninitialised endpoint. Both spellings return false
// when there is no device at all, and the standalone then runs with the editor only.

#pragma once

#include "pluginterfaces/base/fplatform.h"

#if SMTG_OS_WINDOWS
#include "winaudio.h"
#else
#include "jackclient.h"
#endif

namespace Rations
{

#if SMTG_OS_WINDOWS
using NativeAudio = WinAudio;
#else
using NativeAudio = JackClient;
#endif

} // namespace Rations
