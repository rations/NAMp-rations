// SpscByteQueue — fixed-capacity, lock-free, single-producer/single-consumer ring of
// variable-length messages.
//
// The existing src/spscqueue.h carries POINTERS, which is right for handing a retired object back
// to the thread that owns its destructor, and wrong for the LV2 worker extension. A plug-in calls
// LV2_Worker_Schedule::schedule_work() from run() with a pointer to data on its own stack, valid
// only for the duration of that call, so the payload has to be COPIED out of the audio thread's
// reach before schedule_work() returns. Copying is the whole point, and a pointer ring cannot do
// it.
//
// Message framing is a 4-byte length followed by the payload, each message padded so the next one
// starts 4-byte aligned. A message never wraps mid-way in the sense that matters: the copy wraps,
// but the reader reassembles it, so the caller never sees a split.
//
// CONTRACT, and it is not negotiable: exactly ONE thread calls write(), exactly ONE thread calls
// read(), and they are different threads for the whole lifetime of the queue. Nothing here defends
// against a second producer or a second consumer.
//
// write() is wait-free and never allocates. A full queue is REPORTED, not waited on: on the audio
// thread there is nothing to wait for and no time to wait in, so schedule_work() returns
// LV2_WORKER_ERR_NO_SPACE and the plug-in decides what that means. That is the specified behaviour,
// not a shortcut.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace NAMp::host
{

//------------------------------------------------------------------------
template <uint32_t Capacity> class SpscByteQueue
{
    static_assert(Capacity >= 64, "capacity must be big enough for a useful message");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    // Producer side. Returns false if the message does not fit, in which case nothing at all has
    // been written and the queue is left exactly as it was — a partially written message would
    // desynchronise the reader permanently.
    bool write(const void *data, uint32_t size) noexcept
    {
        if (!data && size)
            return false;

        const uint32_t padded = pad(size);
        const uint32_t need = kHeaderBytes + padded;
        // Guard the addition itself: `size` reaches here straight from a plug-in.
        if (padded < size || need < padded || need > Capacity - 1)
            return false;

        const uint32_t head = mHead.load(std::memory_order_relaxed);
        const uint32_t tail = mTail.load(std::memory_order_acquire);
        // One byte is always left unused so that head == tail means empty and never means full.
        if (available(head, tail) < need)
            return false;

        uint32_t at = copyIn(head, &size, kHeaderBytes);
        at = copyIn(at, data, size);
        // Release: everything above must be visible to the reader before the new head is.
        mHead.store((at + (padded - size)) & kMask, std::memory_order_release);
        return true;
    }

    // Consumer side. Copies the next message into `out` and sets `size`. Returns false when the
    // queue is empty, or when the message is larger than `capacity` — in which case the message is
    // DROPPED rather than left to block the queue forever, and `size` reports the size it would
    // have needed so the caller can say so.
    bool read(void *out, uint32_t capacity, uint32_t &size) noexcept
    {
        size = 0;

        const uint32_t tail = mTail.load(std::memory_order_relaxed);
        const uint32_t head = mHead.load(std::memory_order_acquire);
        if (tail == head)
            return false;

        uint32_t at = copyOut(tail, &size, kHeaderBytes);
        const uint32_t padded = pad(size);

        // write() cannot produce a length this large, so seeing one means the ring has
        // desynchronised and every byte in it is suspect. Resynchronising by discarding is the only
        // move that terminates; advancing past a bogus length would chase the corruption around the
        // buffer.
        if (padded > Capacity - kHeaderBytes) {
            clear();
            return false;
        }

        if (size > capacity) {
            mTail.store((at + padded) & kMask, std::memory_order_release);
            return false;
        }

        at = copyOut(at, out, size);
        mTail.store((at + (padded - size)) & kMask, std::memory_order_release);
        return true;
    }

    // Consumer side. Cheap enough to call every block before deciding to drain.
    bool empty() const noexcept
    {
        return mHead.load(std::memory_order_acquire) == mTail.load(std::memory_order_acquire);
    }

    // Consumer side only, and only while the producer is known to be stopped.
    void clear() noexcept
    {
        mTail.store(mHead.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    static constexpr uint32_t kMask = Capacity - 1;
    static constexpr uint32_t kHeaderBytes = sizeof(uint32_t);

    static constexpr uint32_t pad(uint32_t n) noexcept
    {
        return (n + 3u) & ~3u;
    }

    // Bytes the producer may still use, leaving the one-byte guard in place.
    static uint32_t available(uint32_t head, uint32_t tail) noexcept
    {
        return ((tail - head - 1) & kMask);
    }

    // Both halves of a wrapped copy, returning the position just past what was copied. Splitting
    // the memcpy is what lets a message straddle the end of the buffer without the caller knowing.
    uint32_t copyIn(uint32_t at, const void *src, uint32_t n) noexcept
    {
        const uint32_t first = n < Capacity - at ? n : Capacity - at;
        std::memcpy(mBuffer + at, src, first);
        if (n > first)
            std::memcpy(mBuffer, static_cast<const uint8_t *>(src) + first, n - first);
        return (at + n) & kMask;
    }

    uint32_t copyOut(uint32_t at, void *dst, uint32_t n) noexcept
    {
        const uint32_t first = n < Capacity - at ? n : Capacity - at;
        std::memcpy(dst, mBuffer + at, first);
        if (n > first)
            std::memcpy(static_cast<uint8_t *>(dst) + first, mBuffer, n - first);
        return (at + n) & kMask;
    }

    alignas(64) std::atomic<uint32_t> mHead{0};
    alignas(64) std::atomic<uint32_t> mTail{0};
    uint8_t mBuffer[Capacity] = {};
};

} // namespace NAMp::host
