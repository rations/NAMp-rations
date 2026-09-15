// Whether per-node cost measurement is switched on.
//
// The parent project ships the one diagnostic that actually attributes an xrun to a single plug-in
// rather than to the callback as a whole: per-node worst-case microseconds against the period
// budget. It is worth having, and it is worth not paying for when it is off.
//
// So the arming flag is read once from $NAMPRACK_DIAG and cached. On the audio path a node pays one
// predictable, always-the-same-way branch when diagnostics are off, and two clock reads when they
// are on. The environment is not re-read at runtime, which is what makes the cached copy safe to
// touch from the audio thread.

#pragma once

namespace NAMp::host
{

// True when $NAMPRACK_DIAG is set to anything other than "0" or the empty string. Safe to call from
// the audio thread: the first call happens during setup, and every later call is a relaxed load of
// an already-initialised value.
bool diagArmed();

} // namespace NAMp::host
