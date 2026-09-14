// SpscQueue — fixed-capacity, lock-free, single-producer/single-consumer pointer ring.
//
// One use here: the audio thread hands retired objects back to the worker thread, which owns the
// only delete. The audio thread must never call a destructor, because a destructor is a free(),
// and free() takes the allocator lock.
//
// Contract, and it is not negotiable: exactly ONE thread may call push(), exactly ONE thread may
// call pop(), and they must be different threads for the whole lifetime of the queue. There is no
// protection against a second producer or a second consumer.
//
// push() is wait-free and never allocates. A full queue is reported, not waited on — the caller
// (the audio thread) keeps ownership and retries on a later block.

#pragma once

#include <atomic>
#include <cstddef>

namespace Rations
{

template <typename T, size_t Capacity> class SpscQueue
{
    static_assert(Capacity >= 2, "capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    // Producer side. Returns false if the queue is full; the caller keeps the pointer.
    bool push(T *value)
    {
        const size_t head = mHead.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & kMask;
        // One slot is always left empty so full and empty are distinguishable.
        if (next == mTail.load(std::memory_order_acquire))
            return false;
        mSlots[head] = value;
        mHead.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns nullptr when empty.
    T *pop()
    {
        const size_t tail = mTail.load(std::memory_order_relaxed);
        if (tail == mHead.load(std::memory_order_acquire))
            return nullptr;
        T *value = mSlots[tail];
        mTail.store((tail + 1) & kMask, std::memory_order_release);
        return value;
    }

    bool empty() const
    {
        return mHead.load(std::memory_order_acquire) == mTail.load(std::memory_order_acquire);
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    T *mSlots[Capacity] = {};
    std::atomic<size_t> mHead{0}; // written by the producer only
    std::atomic<size_t> mTail{0}; // written by the consumer only
};

} // namespace Rations
