// Tuning constants for the crossfade engine.
//
// These live in their own header, free of any VST3 dependency, because the offline proof that the
// crossfade works links the DSP without the plug-in. A number the proof measures and a number the
// plug-in runs on must be the same number, or the proof is proving something else.

#pragma once

namespace Rations
{
namespace engine
{

// Native-rate samples per model call. Chosen by measurement: the per-model cost curve against
// buffer size is flat over 128-512 with its minimum at 256, and a fixed chunk is what guarantees
// no model is ever handed a block larger than the size it was Reset() with (the A2 fast path
// silently reallocates and wipes its convolution history in that case, on the audio thread).
inline constexpr int kChunk = 256;

// Largest capture directory accepted. A directory is untrusted input; this bounds both the scan
// and the per-instance memory.
//
// The cost is linear and was measured, not estimated, by building banks of 8..128 full-size
// models through the normal loadCaptureSource() + buildCaptureModel() path (Reset at kChunk,
// 48 kHz, i7-12700F): ~845 KB per built model plus ~97 KB per parsed source, i.e. ~940 KB per
// entry, flat to within 2% across that whole range. So this bound is worth 63 MB of resident
// memory in the worst case, against 34 MB at the previous value of 32.
//
// What does NOT scale with it: the audio-thread cost, which is two bound branches whatever N is
// (see the two-model peak-cost decision in the plan); and the Bank struct itself, which is 96
// bytes per entry, so 6 KB.
//
// What DOES scale with it: the time a full-travel sweep takes. Crossing one index may never be
// faster than one receptive field, so at 48 kHz it is 6347 samples = 132 ms per index, and a
// sweep from end to end is (N-1) x 132 ms — 8.3 s here, against 4.1 s at 32. That only applies
// to slamming the knob across its whole range; it is the rate limit that makes the crossfade
// click-free, not a delay that could be tuned away.
inline constexpr int kMaxBankEntries = 64;

// How fast the position may travel, in milliseconds per bank index. The engine raises this at run
// time if one receptive field is longer, so that crossing a whole index always takes at least as
// long as it takes to prime the capture being crossed into.
inline constexpr double kSlewPerIndexMs = 100.0;

// Auto-detent: after the knob has been still this long, glide to the nearest whole capture over
// kDetentGlideMs and stay there until the knob moves again. The glide is deliberately longer than
// the ~132 ms receptive field. At rest exactly one branch is bound, so the plug-in costs one
// model, not two.
inline constexpr double kDetentIdleMs = 300.0;
inline constexpr double kDetentGlideMs = 200.0;

// Bypass and start-up ramp length in milliseconds. Long enough to be inaudible, and never a hard
// mute — a hard bypass switch is itself a click.
inline constexpr double kBypassRampMs = 15.0;

} // namespace engine
} // namespace Rations
