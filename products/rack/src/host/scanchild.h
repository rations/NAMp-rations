// Out-of-process plug-in probing, as a mode of the host binary rather than a second executable.
//
// WHY OUT OF PROCESS AT ALL. Loading a .vst3 runs third-party code, including static initialisers,
// before anything has had a chance to validate it. A plug-in that segfaults or hangs on load must
// not take the rack down during discovery, so each bundle is probed by a process whose entire job
// is to be expendable, under a deadline and an output cap (Catalog::scanViaHelper).
//
// WHY NOT A SEPARATE BINARY. It used to be one — namp-scan — and that made the standalone a
// two-file product that only worked when both files travelled together. A host binary can instead
// re-exec ITSELF with a reserved first argument, which is the same isolation with nothing to ship
// beside it: the child is a fresh process, and if a plug-in kills it the parent records a negative
// result and moves on exactly as before.
//
// THE ONE CASE THAT CANNOT SELF-EXEC is this host library linked into a plug-in bundle, where
// /proc/self/exe is the DAW's binary and re-execing it would launch the DAW. The shipped product
// here is the standalone, so that case does not arise today — but it is one build-graph change
// away, and it costs nothing to keep the door open. That is why the external-helper path survives
// in Catalog::findScanHelper() and is still tried first: point $NAMPRACK_SCAN_HELPER at a binary
// that understands kScanChildFlag and the case is covered without changing anything here.
//
// PROTOCOL. The child is spawned as:
//
//     <argv0> --namp-rack-scan-child <FORMAT> <path>
//
// and writes one payload line per audio-effect class to stdout, in the format catalog.h documents.
// Everything diagnostic goes to stderr so it can never be mistaken for a result. Exit code 0 means
// the probe completed — including when it found nothing, which is a legitimate, cacheable answer.

#pragma once

namespace NAMp::host
{

// The reserved first argument. Chosen to be something no user would type by accident and no
// option parser would claim.
extern const char *const kScanChildFlag;

//------------------------------------------------------------------------
// Call this as the FIRST thing in main(), before any option parsing, any window, any audio device
// and any host context.
//
// When argv[1] is kScanChildFlag this runs the probe, writes the payload to stdout and returns
// true with `exitCode` set — the caller must then return that code immediately and do nothing
// else. Otherwise it returns false, having recorded that this binary understands the flag, so
// Catalog::findScanHelper() may re-exec it; the caller carries on as normal.
bool runScanChildIfRequested(int argc, char *argv[], int &exitCode);

//------------------------------------------------------------------------
// Whether runScanChildIfRequested() has been called, i.e. whether /proc/self/exe is a usable
// scan helper. False in a binary that never installed the mode — including the plug-in.
bool selfScanAvailable();

} // namespace NAMp::host
