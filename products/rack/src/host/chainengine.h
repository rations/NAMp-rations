// ChainEngine — the audio-thread half of the rack.
//
// It owns the scratch buses, adopts whatever chain has been published, runs the hosted nodes either
// side of the amp, and hands retired snapshots back to the builder. It allocates in prepare() and
// nowhere else, it never takes a lock, and it never calls a destructor. That is the whole real-time
// contract and it holds here without exception.
//
// HOW A BLOCK IS DRIVEN. The amp is not a node (see chainmodel.h), so the caller brackets its own
// processing with two calls:
//
//     engine.beginBlock();                       // once per JACK cycle
//     for each chunk:
//         engine.beginChunk(in, outL, outR, n, io);   // runs the pre-section
//         namp->process(io.anchorIn -> io.anchorOut); // the caller's own plumbing
//         engine.endChunk();                          // runs the post-section and the clamp
//
// beginBlock() is separate on purpose. Adopting a new chain must happen at most once per JACK
// cycle, or a chain published mid-block would process the first chunk of a block with one topology
// and the rest with another.
//
// WITH NO CHAIN PUBLISHED the engine is transparent: beginChunk() reports JACK's own pointers as
// the anchor's, endChunk() does nothing, and the cost is one branch. That is the case the
// standalone is in today, and it must stay free.
//
// RETIREMENT. The audio thread pushes the chain it stops using into a lock-free SPSC queue; the
// builder pops it and owns the only delete. The push itself is the proof that the audio thread has
// finished with the snapshot, which is why no settling delay is needed — the parent project waits
// two cycles precisely because it has no such handshake. A momentarily full queue is not an error:
// the audio thread keeps the pointer and retries on the next block.
//
// WHAT THE SWAP IS MEASURED TO COST, AND WHAT IT IS NOT. The handshake above is silent, and that is
// measured rather than argued: with a sine through the running amp at 128 frames, toggling a node's
// enabled flag 758 times — 758 publishes, 758 adoptions, 758 retirements — left the largest
// sample-to-sample step in the output at 0.005812, which is the figure for a chain that never
// changed at all, to every digit. Zero dropouts, nothing non-finite.
//
// LOADING a plug-in into a running chain is a different thing and is NOT silent. The same churn
// done by adding and removing the node instead took that step to 0.094341, a factor of sixteen, and
// it did so with a plug-in whose output was byte-for-byte the same as no plug-in at all — so the
// step is not the sound changing. It is that a freshly instantiated plug-in has empty delay lines
// and cold filter state, and its first samples therefore do not continue the signal that was
// already flowing. That is a property of inserting stateful DSP into live audio, not of this
// handshake, and it is the same problem the amp's own priming and crossfade exist to solve for
// models.
//
// FLAGGED, NOT FIXED. Closing it means running the outgoing and incoming chains together for a
// fade — twice the CPU for the duration and every departing plug-in kept alive across it — which
// is a design decision this engine has not taken and neither parent takes either. What is claimed
// here is what has been measured: the snapshot swap is inaudible; loading a plug-in mid-signal is
// not.

#pragma once

#include "chainmodel.h"
// AudioBlock and RtMidiEvent: what a node is handed, and what rides beside the audio.
#include "pluginref.h"
#include "spscqueue.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace NAMp::host
{

//------------------------------------------------------------------------
// Where the anchor should read and write for this chunk. All three pointers are valid for exactly
// `frames` samples and only until endChunk() returns.
struct ChainIo {
    const float *anchorIn = nullptr;
    float *anchorOut[kMaxChainChannels] = {nullptr, nullptr};
};

//------------------------------------------------------------------------
class ChainEngine
{
public:
    ChainEngine() = default;
    ~ChainEngine();

    ChainEngine(const ChainEngine &) = delete;
    ChainEngine &operator=(const ChainEngine &) = delete;

    //--- owning thread, with the audio thread stopped or suspended -------
    // Sizes every scratch bus. Must be called before any chain is published and again whenever the
    // block size changes. Returns false only if the allocation failed.
    bool prepare(int32_t maxBlock);
    int32_t maxBlock() const
    {
        return mMaxBlock;
    }

    //--- builder thread --------------------------------------------------
    // Hands a compiled chain to the audio thread. Ownership transfers on success. If a previously
    // published chain had not yet been adopted it comes back here and the caller must delete it —
    // the audio thread definitively never saw it, because the exchange is atomic.
    RtChain *publish(RtChain *chain);
    // Pops one snapshot the audio thread has finished with, or nullptr. The caller owns it.
    RtChain *takeRetired()
    {
        return mRetire.pop();
    }
    // Generation of the snapshot the audio thread is actually running, or 0 if it has never adopted
    // one (or has abandoned it). This is what tells the builder when a backend that has left the
    // chain may be destroyed: once a LATER generation is live, the audio thread has demonstrably
    // stopped touching every earlier one, because adopting is what hands the previous one back.
    uint64_t liveGeneration() const
    {
        return mLiveGeneration.load(std::memory_order_acquire);
    }

    // Total reported latency of the hosted nodes, in samples. Published by the audio thread when it
    // adopts a chain, so it is what is actually running rather than what has been queued.
    uint32_t latencySamples() const
    {
        return mLatency.load(std::memory_order_relaxed);
    }

    //--- audio thread ----------------------------------------------------
    // Adopt a newly published chain and hand back the old one. Once per JACK cycle.
    //
    // `midi` is the whole cycle's messages, ordered by frame, valid until the next beginBlock().
    // The engine copies nothing: it hands each chunk the sub-range that falls inside it, with the
    // frame offsets rebased, so a node never has to know it was handed a chunk rather than a block.
    // Pass nothing for a cycle with no MIDI, which is nearly all of them.
    void beginBlock(const RtMidiEvent *midi = nullptr, int32_t midiCount = 0) noexcept;
    // Run the pre-section and report where the anchor should read and write. `in` is JACK's input
    // buffer and is never written.
    void beginChunk(const float *in, float *outL, float *outR, int32_t frames,
                    ChainIo &io) noexcept;
    // Run the post-section, then the safety pass over JACK's outputs.
    void endChunk() noexcept;

    // The audio thread is stopping for good: hand back whatever is still held so the builder can
    // free it. Call only with the audio thread known not to be running.
    void abandon();

private:
    void runSection(const RtNode *nodes, int32_t count, int32_t frames) noexcept;

    // Scratch sample memory, one contiguous allocation. mSlot indexes into it for the scratch slots
    // and at JACK's buffers for the two alias slots.
    std::vector<float> mScratch;
    float *mSlot[kSlotCount][kMaxChainChannels] = {};
    int32_t mMaxBlock = 0;

    // Audio-thread state.
    RtChain *mLive = nullptr;
    // Audio thread. Narrow this cycle's MIDI to the chunk about to run and rebase its offsets.
    void sliceMidi(int32_t frames) noexcept;

    RtChain *mHoldover = nullptr; // retired but the queue was full; retried next block
    int32_t mFrames = 0;

    // This cycle's MIDI, and where in it the current chunk starts. Borrowed, never owned and never
    // copied — the caller's array outlives the block. mBlockPos advances in endChunk(), so the pre
    // and post sections of one chunk are handed the identical slice, which is what makes a message
    // land on the same sample either side of the amp.
    const RtMidiEvent *mMidi = nullptr;
    int32_t mMidiCount = 0;
    int32_t mBlockPos = 0;
    // The slice for the chunk being run, rebased to it. Recomputed once per beginChunk().
    const RtMidiEvent *mChunkMidi = nullptr;
    int32_t mChunkMidiCount = 0;
    // The rebased copy handed to nodes. Fixed capacity, sized in prepare(); a cycle carrying more
    // than this drops the overflow rather than growing, because growing is a malloc on the audio
    // thread. kMaxChunkMidi is far above what a footswitch or a controller sweep produces.
    std::vector<RtMidiEvent> mChunkMidiBuf;

    std::atomic<RtChain *> mPending{nullptr};
    std::atomic<uint32_t> mLatency{0};
    std::atomic<uint64_t> mLiveGeneration{0};

    // Audio thread -> builder. Far deeper than the number of edits that can be in flight; a full
    // queue is handled by retrying, never by blocking or deleting.
    //
    // Qualified because it is the AMP's queue, in the amp's namespace, and there is deliberately
    // no second copy of it here: the same fixed-capacity lock-free ring that hands a retired model
    // back off the audio thread hands a retired chain snapshot back, for the same reason and under
    // the same rule — the audio thread never calls a destructor.
    Rations::SpscQueue<RtChain, 8> mRetire;
};

//------------------------------------------------------------------------
// Speaker-safety net for the end of the chain: replace non-finite samples with silence and clamp
// magnitude to +/- 4.0 (about +12 dBFS, so runaway spikes only and not merely hot levels).
//
// This runs ONCE per block over the chain's output, not after every node as the parent project
// does. A plug-in that emits NaN poisons everything downstream either way, so the per-node pass
// buys nothing but cost; what matters is that nothing non-finite reaches a speaker.
//
// It lives in a translation unit compiled WITHOUT -ffast-math on purpose: -ffinite-math-only lets
// the compiler assume isfinite() is always true and delete the test outright.
void clampChainOutput(float *left, float *right, int32_t frames) noexcept;

// The alternative the plan's risk 19 names — the same guarantee with no branch per sample, testing
// the IEEE exponent field directly and clamping with min/max — and the answer to it.
//
// MEASURED, AND IT LOSES. `namp_chaincheck --clamp-bench` on this machine, 256 frames x 200000
// iterations, nanoseconds per sample:
//
//                        ordinary   poisoned
//     isfinite (above)     0.5376     0.4849
//     exponent test        1.1894     1.1712
//
// Better than twice as slow, on both profiles. The reason is visible in the disassembly: the plain
// loop above is auto-vectorised, while the memcpy-round-trip through an integer defeats that and
// leaves scalar code. So the readable version ships and the clever one does not — which is the
// point of measuring rather than reasoning about it.
//
// It is kept, rather than deleted, so the comparison can be re-run on other hardware; it is not
// called by anything but the benchmark. Both live in the same translation unit so the two are
// always compiled under identical flags.
void clampChainOutputExponent(float *left, float *right, int32_t frames) noexcept;

} // namespace NAMp::host
