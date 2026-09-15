// RtChain — the immutable description of a signal chain, as the audio thread reads it.
//
// This is a snapshot, not a model. It is built off the audio thread by ChainBuilder, published by a
// single atomic exchange, and never modified once published. The audio thread reads it and nothing
// else; adding, removing, reordering, bypassing or re-mixing a node all mean compiling a new
// snapshot, which is why none of those operations need a lock or a branch on the real-time path.
//
// SHAPE. A pedalboard has an amp in the middle of it, and so does this chain:
//
//     jack in (mono) -> pre[0..n) -> the amp -> post[0..m) -> jack out (stereo)
//
// THE AMP IS THE ANCHOR and is not a node here. It is mono-in / stereo-out
// (src/rationsprocessor.cpp
// declares kMono in and kStereo out), so it is structurally the point where the chain becomes
// stereo, and the standalone already drives it through its own VST3 plumbing. The chain therefore
// records where the anchor should read and write, and the host calls it in between the two
// sections. Pre-section nodes are mono; post-section nodes are stereo. A mono pedal in front of the
// amp is instantiated and run ONCE, which is where the parent project pays double.
//
// SLOTS. Every node reads one bus and writes another, named by a small integer rather than a
// pointer, because the pointers for the two ends of the chain are JACK's and change every block.
// The compiler alternates the two ping-pong slots, so no node ever copies its result back and
// no two live signals ever share a slot.
//
// See chainengine.h for what the slot numbers resolve to and chainbuilder.h for how they are
// assigned.

#pragma once

#include <cstdint>

namespace NAMp::host
{

class PluginBackend;

//------------------------------------------------------------------------
// A chain longer than this is a user error, not a use case; the cap is what keeps a snapshot a
// fixed-size object that can be allocated with one new.
constexpr int32_t kMaxChainNodes = 32;
// The chain never carries more than a stereo pair. This is a guitar pedalboard.
constexpr int32_t kMaxChainChannels = 2;

// The most MIDI messages one JACK cycle may carry into the rack. A foot on a switch produces one;
// a controller sweeping a CC at its fastest produces a few dozen a second, which at 48 kHz and 128
// frames is well under one per cycle. 256 is therefore not a limit anyone plays into - it is the
// number that makes the engine's per-chunk buffer a fixed allocation made in prepare(), because the
// alternative is growing a vector on the audio thread. Overflow drops the tail of the cycle and is
// counted, never grown into.
constexpr int32_t kMaxChunkMidi = 256;

//------------------------------------------------------------------------
// Bus slots. The first four are scratch owned by ChainEngine; the last two are aliases for JACK's
// own memory and are refilled every chunk.
constexpr int8_t kSlotPing = 0;
constexpr int8_t kSlotPong = 1;
// Holds the unprocessed copy of one node's input while that node runs, for wet/dry mixing. One slot
// is enough for the whole chain because the copy is consumed immediately after the node returns.
constexpr int8_t kSlotDry = 2;
// Reserved for the second live signal of a future parallel split/merge pair. Allocated now so
// adding the split later is a change to the compiler and not to the buffer layout.
constexpr int8_t kSlotSpare = 3;
constexpr int8_t kScratchSlotCount = 4;

// JACK's input buffer. READ ONLY: JACK may share an input port's buffer between clients, so nothing
// may ever be written through it. The compiler guarantees this by never assigning kSlotIn as any
// node's output.
constexpr int8_t kSlotIn = 4;
// JACK's two output buffers.
constexpr int8_t kSlotOut = 5;
constexpr int8_t kSlotCount = 6;

//------------------------------------------------------------------------
// One hosted plug-in in the chain, already resolved to a live instance.
//
// There is no `enabled` flag: a bypassed node is simply left out of the snapshot, so bypass costs
// nothing on the audio thread and the chain shortens rather than branching. That is also why
// `backend` is never null in a published snapshot.
struct RtNode {
    PluginBackend *backend = nullptr;
    int8_t inSlot = kSlotIn;
    int8_t outSlot = kSlotPing;
    int8_t channels = 1;
    // 1.0 = fully wet, and the only value that costs nothing. Anything less copies the node's input
    // to kSlotDry and blends afterwards.
    float mix = 1.0f;

    // The dry path's delay line, so a blend against a plug-in that reports latency lines up instead
    // of comb-filtering: the node's output is late by `latencySamples()`, so the dry copy has to be
    // made late by exactly the same amount before the two are summed. Null and zero when the node
    // reports no latency, which is the usual case and costs nothing.
    //
    // The storage belongs to the CHAIN BUILDER'S node, not to this snapshot, and that is the whole
    // point of the indirection. Every mix change republishes, and a user dragging the wet/dry
    // slider republishes on every pointer motion; a ring living in the snapshot would be freshly
    // zeroed dozens of times a second and the dry signal would collapse to silence for the length
    // of the drag. Living with the node instead, it survives every republish and is retired with
    // the instance it belongs to. It is only ever allocated while the node is unpublished, so the
    // audio thread cannot be holding a pointer into it when it moves.
    float *dryRing = nullptr; // channels * dryLen floats, channel-major
    int32_t dryLen = 0;       // per channel
    // Write cursor, owned by the audio thread for as long as this snapshot is live.
    int32_t *dryPos = nullptr;
};

//------------------------------------------------------------------------
// An entire chain. Allocated and freed only off the audio thread.
struct RtChain {
    // Monotonic, assigned at compile time. ChainBuilder uses it to decide when a backend that has
    // left the chain can safely be destroyed: not before every snapshot that referenced it has come
    // back through the retirement queue.
    uint64_t generation = 0;

    int32_t preCount = 0;
    int32_t postCount = 0;

    // Where the amp itself reads and writes. anchorIn names a mono slot, anchorOut a stereo one.
    int8_t anchorIn = kSlotIn;
    int8_t anchorOut = kSlotOut;

    // Summed reported latency of the hosted nodes only. The amp's own is added by the caller, which
    // is
    // the only place that knows it.
    uint32_t latency = 0;

    // Zero JACK's output buffers at the top of each chunk. Set only when the last thing to write
    // them is a hosted plug-in: JACK does not promise an output buffer is clean, and a plug-in that
    // declines to write leaves whatever was there. When the amp is the last writer this stays
    // false, because the amp always writes every sample and the memset would be pure cost.
    bool clearOutput = false;

    // Run the end-of-chain safety pass. False for an empty rack, so the standalone's existing cost
    // is unchanged when no plug-ins are loaded.
    bool clampOutput = false;

    RtNode pre[kMaxChainNodes];
    RtNode post[kMaxChainNodes];
};

} // namespace NAMp::host
