// ChainEngine implementation. See chainengine.h for the block-driving contract and the retirement
// handshake. The audio-thread half of this file allocates nothing, takes no lock, calls no
// destructor, does no I/O and logs nothing.
//
// This translation unit must NOT be compiled with -ffast-math: clampChainOutput() depends on
// std::isfinite() actually being evaluated.

#include "chainengine.h"
#include "pluginbackend.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace NAMp::host
{

namespace
{

// Above this a sample is a runaway spike rather than a hot level, so it is clamped rather than
// passed on. The final [-1, 1] limit belongs to whatever is downstream of the rack.
constexpr float kSafetyLimit = 4.0f;

} // namespace

//------------------------------------------------------------------------
ChainEngine::~ChainEngine()
{
    // Anything still held at destruction is ours to free: the audio thread is gone by definition.
    delete mLive;
    delete mHoldover;
    delete mPending.exchange(nullptr, std::memory_order_acquire);
    while (RtChain *chain = mRetire.pop())
        delete chain;
}

//------------------------------------------------------------------------
bool ChainEngine::prepare(int32_t maxBlock)
{
    if (maxBlock <= 0)
        return false;

    // One allocation for every scratch bus, so the whole rack's working set is contiguous. At the
    // maximum this is 4 slots x 2 channels x maxBlock floats — 16 KiB at a 512-sample block.
    const size_t total = static_cast<size_t>(kScratchSlotCount) *
                         static_cast<size_t>(kMaxChainChannels) * static_cast<size_t>(maxBlock);
    mScratch.assign(total, 0.0f);
    mMaxBlock = maxBlock;

    // The one MIDI allocation, here and nowhere else. assign() rather than resize() so a re-prepare
    // at a new block size leaves no stale message behind to be replayed into the first chunk.
    mChunkMidiBuf.assign(static_cast<size_t>(kMaxChunkMidi), RtMidiEvent{});
    mMidi = nullptr;
    mMidiCount = 0;
    mBlockPos = 0;
    mChunkMidi = nullptr;
    mChunkMidiCount = 0;

    for (int8_t slot = 0; slot < kScratchSlotCount; ++slot) {
        for (int32_t c = 0; c < kMaxChainChannels; ++c) {
            const size_t offset =
                (static_cast<size_t>(slot) * static_cast<size_t>(kMaxChainChannels) +
                 static_cast<size_t>(c)) *
                static_cast<size_t>(maxBlock);
            mSlot[slot][c] = mScratch.data() + offset;
        }
    }
    // The two alias slots are refilled from JACK every chunk; leaving them null until then means a
    // compiler bug that assigned one of them to a scratch role crashes here instead of corrupting
    // somebody else's buffer.
    for (int32_t c = 0; c < kMaxChainChannels; ++c) {
        mSlot[kSlotIn][c] = nullptr;
        mSlot[kSlotOut][c] = nullptr;
    }
    return true;
}

//------------------------------------------------------------------------
RtChain *ChainEngine::publish(RtChain *chain)
{
    // Whatever comes back was never adopted — the exchange is atomic, so the audio thread either
    // took the previous pointer before this call or cannot take it now.
    return mPending.exchange(chain, std::memory_order_acq_rel);
}

//------------------------------------------------------------------------
void ChainEngine::abandon()
{
    if (mLive) {
        if (!mRetire.push(mLive))
            delete mLive; // the audio thread is not running, so this is our object to free
        mLive = nullptr;
    }
    if (mHoldover) {
        if (!mRetire.push(mHoldover))
            delete mHoldover;
        mHoldover = nullptr;
    }
    mLiveGeneration.store(0, std::memory_order_release);
    mLatency.store(0, std::memory_order_relaxed);
}

//------------------------------------------------------------------------
// Audio thread.
void ChainEngine::beginBlock(const RtMidiEvent *midi, int32_t midiCount) noexcept
{
    // Borrowed for the cycle. Nothing is copied here; the slicing happens per chunk.
    mMidi = (midi && midiCount > 0) ? midi : nullptr;
    mMidiCount = mMidi ? midiCount : 0;
    mBlockPos = 0;

    // A snapshot that could not be handed back last time takes priority: until it is gone, adopting
    // another would mean holding two dead chains with nowhere to put the second.
    if (mHoldover && mRetire.push(mHoldover))
        mHoldover = nullptr;
    if (mHoldover)
        return;

    RtChain *next = mPending.exchange(nullptr, std::memory_order_acquire);
    if (!next)
        return;

    if (mLive && !mRetire.push(mLive))
        mHoldover = mLive;
    mLive = next;

    mLatency.store(next->latency, std::memory_order_relaxed);
    mLiveGeneration.store(next->generation, std::memory_order_release);
}

//------------------------------------------------------------------------
// Audio thread. Nothing here allocates, locks or logs.
void ChainEngine::runSection(const RtNode *nodes, int32_t count, int32_t frames) noexcept
{
    const size_t bytes = sizeof(float) * static_cast<size_t>(frames);

    for (int32_t i = 0; i < count; ++i) {
        const RtNode &node = nodes[i];

        // A node at full wet — the normal case — pays for this one comparison and nothing else.
        const bool blend = node.mix < 1.0f;
        if (blend && node.dryLen > 0) {
            // The node's output is late by its reported latency, so the dry copy is made late by
            // exactly the same amount: push this block's input into the ring and take out what went
            // in `dryLen` samples ago. Without this the two paths sum out of phase and a wet/dry
            // blend against a latent plug-in is a comb filter, which is not a mix control, it is a
            // effect the user did not ask for.
            const int32_t len = node.dryLen;
            int32_t endPos = 0;
            for (int32_t c = 0; c < node.channels; ++c) {
                float *ring = node.dryRing + static_cast<size_t>(c) * static_cast<size_t>(len);
                const float *in = mSlot[node.inSlot][c];
                float *dry = mSlot[kSlotDry][c];
                int32_t pos = *node.dryPos;
                for (int32_t s = 0; s < frames; ++s) {
                    dry[s] = ring[pos];
                    ring[pos] = in[s];
                    if (++pos == len)
                        pos = 0;
                }
                endPos = pos; // every channel advances identically; recording it once is enough
            }
            *node.dryPos = endPos;
        } else if (blend) {
            for (int32_t c = 0; c < node.channels; ++c)
                std::memcpy(mSlot[kSlotDry][c], mSlot[node.inSlot][c], bytes);
        }

        AudioBlock block;
        block.in = mSlot[node.inSlot];
        block.out = mSlot[node.outSlot];
        block.channels = node.channels;
        block.frames = frames;
        // Every node is offered every message. Which one is for it is a question only the plug-in
        // can answer, because the answer is the binding its user taught it.
        block.midi = mChunkMidi;
        block.midiCount = mChunkMidiCount;
        node.backend->process(block);

        if (blend) {
            const float wet = node.mix;
            const float dry = 1.0f - node.mix;
            for (int32_t c = 0; c < node.channels; ++c) {
                float *out = mSlot[node.outSlot][c];
                const float *src = mSlot[kSlotDry][c];
                for (int32_t s = 0; s < frames; ++s)
                    out[s] = out[s] * wet + src[s] * dry;
            }
        }
    }
}

//------------------------------------------------------------------------
// Audio thread. Narrow the cycle's MIDI to [mBlockPos, mBlockPos + frames) and rebase it, so a node
// sees offsets measured from the chunk it is actually being given.
//
// Linear from the start of the cycle rather than a running cursor: the list is ordered and tiny —
// nearly always empty, and a stomp makes it one long — so the scan costs less than the state it
// would take to avoid it, and it cannot be left inconsistent by a chunk that returned early.
void ChainEngine::sliceMidi(int32_t frames) noexcept
{
    mChunkMidi = nullptr;
    mChunkMidiCount = 0;
    if (!mMidi || frames <= 0 || mChunkMidiBuf.empty())
        return;

    const int32_t first = mBlockPos;
    const int32_t last = mBlockPos + frames; // exclusive
    int32_t n = 0;
    for (int32_t i = 0; i < mMidiCount && n < static_cast<int32_t>(mChunkMidiBuf.size()); ++i) {
        const RtMidiEvent &e = mMidi[i];
        if (e.frame < first || e.frame >= last)
            continue;
        mChunkMidiBuf[static_cast<size_t>(n)] = e;
        mChunkMidiBuf[static_cast<size_t>(n)].frame = e.frame - first;
        ++n;
    }
    if (n > 0) {
        mChunkMidi = mChunkMidiBuf.data();
        mChunkMidiCount = n;
    }
}

//------------------------------------------------------------------------
// Audio thread.
void ChainEngine::beginChunk(const float *in, float *outL, float *outR, int32_t frames,
                             ChainIo &io) noexcept
{
    mFrames = frames;
    sliceMidi(frames);

    io.anchorIn = in;
    io.anchorOut[0] = outL;
    io.anchorOut[1] = outR;

    const RtChain *chain = mLive;
    if (!chain || frames <= 0 || frames > mMaxBlock)
        return; // an empty rack, or a block the buses were not sized for: stay transparent

    // The const_cast is confined to the slot table. kSlotIn is never any node's output slot, which
    // the chain compiler guarantees, so nothing is ever written through it.
    mSlot[kSlotIn][0] = const_cast<float *>(in);
    mSlot[kSlotIn][1] = nullptr;
    mSlot[kSlotOut][0] = outL;
    mSlot[kSlotOut][1] = outR;

    if (chain->clearOutput) {
        const size_t bytes = sizeof(float) * static_cast<size_t>(frames);
        std::memset(outL, 0, bytes);
        std::memset(outR, 0, bytes);
    }

    runSection(chain->pre, chain->preCount, frames);

    io.anchorIn = mSlot[chain->anchorIn][0];
    io.anchorOut[0] = mSlot[chain->anchorOut][0];
    io.anchorOut[1] = mSlot[chain->anchorOut][1];
}

//------------------------------------------------------------------------
// Audio thread.
void ChainEngine::endChunk() noexcept
{
    // Advance past this chunk whatever happens below: a chunk the engine stayed transparent for
    // still consumed its share of the block, and leaving the cursor behind would replay its
    // messages into the next one.
    mBlockPos += mFrames > 0 ? mFrames : 0;

    const RtChain *chain = mLive;
    if (!chain || mFrames <= 0 || mFrames > mMaxBlock)
        return;

    runSection(chain->post, chain->postCount, mFrames);

    if (chain->clampOutput)
        clampChainOutput(mSlot[kSlotOut][0], mSlot[kSlotOut][1], mFrames);
}

//------------------------------------------------------------------------
void clampChainOutput(float *left, float *right, int32_t frames) noexcept
{
    if (!left || !right)
        return;

    for (int32_t i = 0; i < frames; ++i) {
        float l = left[i];
        float r = right[i];

        if (!std::isfinite(l))
            l = 0.0f;
        else if (l > kSafetyLimit)
            l = kSafetyLimit;
        else if (l < -kSafetyLimit)
            l = -kSafetyLimit;

        if (!std::isfinite(r))
            r = 0.0f;
        else if (r > kSafetyLimit)
            r = kSafetyLimit;
        else if (r < -kSafetyLimit)
            r = -kSafetyLimit;

        left[i] = l;
        right[i] = r;
    }
}

//------------------------------------------------------------------------
void clampChainOutputExponent(float *left, float *right, int32_t frames) noexcept
{
    if (!left || !right)
        return;

    // A float is NaN or infinity exactly when its eight exponent bits are all set, so the test is
    // one mask and one compare on the integer representation — no floating-point compare, no
    // isfinite call, and nothing for -ffinite-math-only to have an opinion about. Multiplying by a
    // 0/1 mask rather than branching keeps the loop straight-line.
    constexpr uint32_t kExponent = 0x7f800000u;

    auto scrub = [](float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        const uint32_t finite = ((bits & kExponent) == kExponent) ? 0u : 0xffffffffu;
        bits &= finite;
        std::memcpy(&v, &bits, sizeof(v));
        // fmin/fmax rather than ?: so this compiles to minss/maxss with no branch. NaN is already
        // gone by here, so their NaN-propagation rules do not come into it.
        return std::fmin(std::fmax(v, -kSafetyLimit), kSafetyLimit);
    };

    for (int32_t i = 0; i < frames; ++i) {
        left[i] = scrub(left[i]);
        right[i] = scrub(right[i]);
    }
}

} // namespace NAMp::host
