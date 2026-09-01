// Tuning constants for the crossfade engine.
//
// These live in their own header, free of any VST3 dependency, because the offline proof that the
// crossfade works links the DSP without the plug-in. A number the proof measures and a number the
// plug-in runs on must be the same number, or the proof is proving something else.

#pragma once

namespace Rations
{

// The four channels, in panel order. This is the value space of the kChannelId parameter and the
// index space of every four-element array in the plug-in and the editor; nothing may reorder it.
//
// It lives HERE, in the header that deliberately has no VST3 dependency, rather than beside the
// parameter IDs, for the same reason the tuning constants do: the channel rack and the offline
// proof that the channel switch is exact both need to name a channel, and neither of them links
// the plug-in. rationsids.h includes this file, so there is still exactly one definition.
enum Channel : int {
    kChannelClean = 0,
    kChannelCrunch = 1,
    kChannelOd1 = 2,
    kChannelOd2 = 3,
    kChannelCount = 4,
};

// What a channel is called when the user has not said otherwise, and the key each channel's
// attributes are named by in the capabilities message.
//
// These used to be directory names, resolved inside the bundle's Resources/captures. They are not
// any more: the plug-in ships no captures and every channel's bank is a folder or a file the user
// picked. So a channel's DISPLAYED name is now three-deep - the user's typed override, else the
// basename of what is loaded, else this - and this is only the last of the three, the name an
// empty channel carries so that a panel with nothing loaded still reads as an amp head.
inline constexpr const char *kChannelDefaultName[kChannelCount] = {"Clean", "Crunch", "OD1", "OD2"};

// What a whole bank states about its own levels, for the editor's benefit rather than the audio
// thread's. The DSP never consults this: entryGain() asks each entry individually and falls back to
// unity when a field is absent, so a bank of mixed captures is already handled correctly capture by
// capture. This exists so the settings page can grey an output mode the loaded captures cannot
// honour, and so a generic host UI can retitle the same controls "(n/a)".
//
// Each flag is true only when EVERY built entry states that field. "Any" would be the wrong test:
// an option that works for three captures out of twelve is one that changes the level as the dial
// crosses the ninth, and offering it would be telling the user the wrong thing.
//
// It lives HERE, with Channel, rather than in bank.h beside the entries it is derived from, and the
// reason is the same one Channel has plus one that is specific: bank.h includes NAM/dsp.h and so
// Eigen, the edit controller needs this type, and the editor's translation units include <X11/X.h>,
// which #defines `Success` — a name Eigen declares as an enumerator and stops compilation over. The
// file browser's `NoResult` carries a note about the same header doing the same thing to `None`.
struct CaptureLevels {
    bool hasLoudness = false;
    bool hasInputLevel = false;
    bool hasOutputLevel = false;
    // Whether Slim means anything for this bank: whether its captures are slimmable containers
    // with more than one size variant. The newer A2 captures are; the older ones the trainer now
    // calls A1 are not, and for those the control can do nothing at all - which is why the editor
    // does not show its icon rather than showing a knob that is inert. This one is not read off a
    // built model but off the PARSED source, because it is known as soon as the file is understood
    // and does not need a model to exist to be true.
    //
    // Same "every entry" rule as the three above and for the same reason: a bank where nine
    // captures out of twelve can be made smaller is one whose model changes size as the dial
    // crosses the tenth, and offering that would be telling the user the wrong thing.
    bool slimmable = false;
};

namespace engine
{

// Where Normalized output puts every capture, in dB.
//
// The value is the upstream plug-in's and is shared by every plug-in in this family, which is the
// point of it: a capture normalized here has to land where the same capture lands there, or the
// same file is two different loudnesses depending on what is playing it. It is a target, not a
// measurement, so it is a constant rather than something read from a model - what IS read from the
// model is each capture's own measured loudness, which is what gets brought to this.
inline constexpr double kNormalizedTargetDb = -18.0;

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

// --- the channel switch -------------------------------------------------------------------
//
// Switching channels is not a knob turn, so it is not the crossfade above. The incoming channel
// has not been fed anything for however long it has been idle, and a strictly feed-forward model
// with no convolution history produces a whole receptive field of wrong output before it produces
// a right one. Fading into that would fade into garbage.
//
// So the incoming channel is CAUGHT UP first: it is fed the last R native-rate input samples out
// of the ring below, faster than real time, with its output thrown away. When the ring is drained
// the model is exact, and only then does the fade run — between two exact signals, which is why
// kChannelFadeMs can be short. R is the incoming capture's own GetPrewarmSamples(), read from the
// model; it is never a constant here.

// Native-rate samples of input history kept for the catch-up. A power of two, so the ring indexes
// with a mask instead of a modulo.
//
// The size follows from the catch-up arithmetic rather than being picked. The read cursor lags the
// write head by R at the moment of the switch and never falls further behind than that — a
// catch-up with no CPU headroom parks at exactly R rather than letting the gap grow — so the ring
// must hold R plus whatever arrives while the gap is being closed, plus one native block. The A2
// fast path reports 6347 samples at 48 kHz; even at a rate barely above real time, where the
// catch-up takes several times R, the gap itself never exceeds R and the requirement is a small
// multiple of it. 32768 is a power of two clear of every such case with a large host block still
// to spare, and costs 256 KB per instance — against the ~33 MB the four banks already cost.
//
// A capture whose prewarm would not fit is not silently under-primed: ModelBank warns once at
// build time, where the number is first known and where printing is allowed.
inline constexpr int kInputRingSamples = 32768;

// How many models the audio thread may be running at once, in total, at any instant during a
// channel switch — the sounding channel's own branches included.
//
// A TOTAL, not a rate, and that is the whole of what this constant learned from being measured.
// The design was originally costed as a rate: the incoming channel consumes input at some multiple
// of real time, so the catch-up lasts R / ((multiple - 1) * Fs) and the peak is "outgoing branches
// plus multiple". The trouble is the phrase "outgoing branches", which is one at rest and TWO
// while a gain dial is moving — so a fixed rate quietly means two different peaks, and the larger
// of them is the one a player produces by stomping the footswitch while turning the gain knob.
// Measured, that combination missed the deadline at every rate, down to one where the catch-up
// took two thirds of a second.
//
// Stated as a total instead, the peak is the same number whatever else is happening: the rack asks
// the sounding channel how many branches it is about to run and gives the catch-up whatever is
// left. When nothing is left the switch is HELD — the outgoing channel keeps sounding, the switch
// simply arrives late — which is the same answer D4 already gives when a capture is not built yet,
// applied to CPU instead of to readiness. A late switch is a bad afternoon; a missed deadline is a
// hole in the recording.
//
// MEASURED, not chosen — rations_jackcheck, this machine, CPU governor left at powersave, the
// built bundle driven from a real JACK process callback at 48 kHz / 128 frames, in three states:
// stomping with the switches allowed to complete, stomping four times a second so the catch-up
// never finishes, and stomping while a gain dial sweeps. Worst block as a fraction of the 2.67 ms
// period, and xruns over fifteen seconds:
//
//     budget   catch-up   worst block   xruns
//      (1.0)      —          40%          0     <- one model at its detent, no switching at all
//      (2.0)      —          57%          0     <- a gain dial moving, no switching
//       2.4      331 ms     56-78%        0     <- four passes x three states, all clean
//       2.5      264 ms     55-82%        1
//       2.6      220 ms     54-80%        1
//       2.75     176 ms     67-76%       12
//       3.0      132 ms     68-84%       60
//       4.0       66 ms       108%       47
//       7.0       26 ms       176%      109
//
// Two things to read out of that. The usable deadline is about three quarters of the nominal
// period, not the whole of it — the ALSA interrupt, the graph turnaround and everything else on
// the machine live inside it too. And the cliff is steep: 2.4 is clean over four passes where 2.5
// is not, so 2.4 is the value taken. 128 frames is the buffer size it is qualified at, which is
// the smallest one anyone actually records with.
//
// What it costs: at rest the catch-up gets 2.4 - 1 = 1.4 samples per sample of real time, so it
// closes a 6347-sample gap at 0.4 samples per sample and the switch lands about 340 ms later. That
// is SLOW for a footswitch, and it is arithmetic rather than implementation: one model costs
// 29% of a JACK period on this machine, so there are about two and a half models to go round, the
// plug-in already spends one of them, and priming a whole receptive field on the audio thread
// needs most of what is left. The catch-up has a fixed amount of work to do — one receptive field
// of model time — and the only lever on how long that takes is how much spare CPU there is to do
// it in. Nothing about this constant can improve it; only the cost of a model can, which means
// either the governor or moving the priming off the audio thread. Both are decisions for the
// author, and the second one contradicts a binding rule, so neither was taken here.
inline constexpr double kSwitchModelBudget = 2.4;

// The floor under the above. A catch-up only closes the gap if it consumes input FASTER than real
// time, so anything at or below 1.0 makes no progress at all and is treated as no headroom: the
// switch is held and the cursor is parked one receptive field behind the write head, ready to
// resume the instant the sounding channel collapses back to a single branch.
inline constexpr double kCatchupMinRate = 1.0;

// How often the prime worker looks for new input. It is not signalled by the audio thread at all
// - RT publishes the ring's write head and nothing else - so this is the whole of the coupling
// between the two threads, and it is one atomic load. Well under a block period at 128 frames
// (2.67 ms), so the worker never has more than a fraction of a block to catch up on, and cheap
// enough that a tick which finds nothing to do costs a load and a sleep.
inline constexpr int kPrimeTickUs = 500;

// The fade between two channels, once the incoming one is exact. Both signals are true responses
// to the same input by the time this runs, so there is no discontinuity to mask and no curve to
// tune; the fade is here because the two channels are different amps at different levels, and a
// step between two correct signals is still a step.
//
// MEASURED, not chosen, by rations_switchcheck --fade-sweep, which asks the question of the two
// continuously-running references directly: it applies mixFade()'s own arithmetic to them at two
// thousand landing points spread across the take, in both directions, and scores the largest step
// the fade produces against the material's own step in the window that fade lands in — the same
// metric, and the same 1.5x threshold, the no-click assertion is gated on. A local yardstick
// rather than a global one, because a fade that lands in a dying note would otherwise hide behind
// a pick attack elsewhere in the take, and a dying note is where a short fade shows. Worst score
// over all six channel pairs at six combinations of the two dials, 36 runs:
//
//     fade ms   0.00    0.25   0.50   0.75   1.00   1.50   2.00   3.00   4.00   5.00   7.50  10.00
//     worst    x225    x16.2   x8.99  x4.42  x3.41  x2.35  x1.95  x1.56  x1.35  x1.28  x1.14  x1.10
//
// A one-sample fade is the hard switch, so the sweep carries its own control: x225 is a click by
// any measure, and the curve is that click being spread out. The knee is between 3 ms, which
// fails, and 4 ms, which passes everywhere. 5.0 is taken rather than 4.0 because the failing case
// and the marginal one are the same channel pair, the material is one synthetic take, and the
// extra millisecond costs 1 ms of an 18 ms switch — margin this constant can afford to buy, unlike
// kSwitchModelBudget above, where every step bought hundreds of milliseconds and the value had to
// be taken at the cliff.
//
// It was 10.0 while the catch-up took a third of a second, where nothing about it was noticeable.
// Now that the catch-up is over in thirteen milliseconds the fade is a real share of the switch,
// and a round number carried over from the plan is no longer good enough for it.
inline constexpr double kChannelFadeMs = 5.0;

// How fast a per-channel trim travels, in dB per second.
//
// A trim change is ramped rather than applied as a per-block scalar: a level that steps once per
// block zips audibly while the slider is being dragged. Stated as dB per second rather than as a
// ramp length, because a ramp length would need to know how wide the trim's range is, and the
// range belongs with the parameter rather than with the engine - this header has no VST3
// dependency and should not learn one to hold a number the panel owns.
//
// 1600 dB/s crosses the whole of the shipped +/-12 dB trim in 15 ms, which is the same length as
// the bypass ramp below and inaudible for the same reason. Nothing about the switch depends on
// it: an incoming channel's ramp is SNAPPED to its target as the fade begins, so a channel change
// opens at the level the player set rather than sliding up to it.
inline constexpr double kLevelRampDbPerSec = 1600.0;

// Bypass and start-up ramp length in milliseconds. Long enough to be inaudible, and never a hard
// mute — a hard bypass switch is itself a click.
inline constexpr double kBypassRampMs = 15.0;

} // namespace engine
} // namespace Rations
