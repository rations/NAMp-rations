// Lv2Worker — the LV2 worker extension (lv2:worker), which is how a plug-in gets work done that
// cannot happen on the audio thread.
//
// A plug-in that needs to load a sample, build a filter table or free a large buffer calls
// schedule_work() from run(). The host copies the request away, does the work on another thread,
// and hands the result back to the plug-in at the top of a later run() through work_response().
// That round trip is what lets a plug-in change something expensive without allocating, blocking or
// freeing on the audio thread — the same discipline this project applies to itself.
//
// The model is jalv's, because it is the reference implementation of a specified protocol and there
// is no second way to be correct about the ordering. The code is not: jalv's worker.c is built on
// zix's semaphore and ring, and this one is built on the SPSC ring already in this tree, so the
// dependency stays lilv/suil/lv2 and does not grow a fourth library for two data structures.
//
// THREADING.
//   * schedule() runs on the AUDIO thread. It copies into a ring and posts a semaphore. That post
//   is
//     the single syscall the audio path is allowed to make, and it is deliberate: sem_post never
//     blocks and never takes a lock the audio thread can be descheduled holding, and the only
//     alternative — a worker that polls — trades a bounded wake for a permanent stream of them. It
//     is flagged here rather than left to be discovered.
//   * work() runs on the WORKER thread and is where the plug-in is allowed to do anything at all.
//   * emitResponses() and endRun() run on the AUDIO thread, in that order, at the end of every
//     block. The specification requires end_run() after any responses, because that is the
//     plug-in's signal that the cycle is complete.
//
// NON-THREADED MODE exists for state restoration. lilv_state_restore() can make a plug-in schedule
// work and then expect the response before it will report the new state, with no run() cycle in
// between; doing that work inline is what jalv does and what the specification's "synchronous"
// language permits, and it is safe precisely because state restoration never happens on the audio
// thread in this host — the chain builder does it while the node is unpublished.

#pragma once

#include "spscbytequeue.h"

#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>

#include <atomic>
#include <cstdint>
#include <semaphore.h>
#include <thread>

namespace NAMp::host
{

//------------------------------------------------------------------------
class Lv2Worker
{
public:
    // A worker request or response bigger than this is refused. The extension carries small control
    // messages — a command tag, a path, a pointer to something the plug-in already owns — not bulk
    // data, and a plug-in that wants to move a megabyte through here is misusing it.
    static constexpr uint32_t kMaxMessageBytes = 8192;

    Lv2Worker() = default;
    ~Lv2Worker();

    Lv2Worker(const Lv2Worker &) = delete;
    Lv2Worker &operator=(const Lv2Worker &) = delete;

    // Attach to an instantiated plug-in. `iface` may be null, in which case the plug-in does not
    // use the extension and every call below is a cheap no-op. `threaded` false runs work inline,
    // for state restoration. Returns false only if the thread could not be started.
    bool start(const LV2_Worker_Interface *iface, LV2_Handle handle, bool threaded);

    // Stops the thread and waits for it. Safe to call twice, and safe to call when never started.
    // Must be called before the plug-in instance is freed: the worker thread calls into the
    // plug-in.
    void stop();

    // The feature to hand to the plug-in. Valid for the life of this object.
    LV2_Worker_Schedule *scheduleFeature() noexcept
    {
        return &mSchedule;
    }

    // Audio thread. Deliver anything the worker finished since the last block, then tell the
    // plug-in the cycle is over. Both are no-ops when the plug-in has no worker interface.
    void emitResponses() noexcept;
    void endRun() noexcept;

    // Requests the audio thread had to refuse because the queue was full, and responses that could
    // not be delivered. Non-zero means a plug-in is being starved and the rack should say so.
    uint64_t droppedRequests() const noexcept
    {
        return mDroppedRequests.load(std::memory_order_relaxed);
    }

private:
    static LV2_Worker_Status scheduleCallback(LV2_Worker_Schedule_Handle handle, uint32_t size,
                                              const void *data);
    static LV2_Worker_Status respondCallback(LV2_Worker_Respond_Handle handle, uint32_t size,
                                             const void *data);

    // Audio thread, via scheduleCallback.
    LV2_Worker_Status schedule(uint32_t size, const void *data) noexcept;
    // Worker thread, via respondCallback.
    LV2_Worker_Status respond(uint32_t size, const void *data) noexcept;

    void run();
    // Take one request off the queue and give it to the plug-in. Shared by the worker thread and by
    // the inline path.
    bool serveOne();

    const LV2_Worker_Interface *mIface = nullptr;
    LV2_Handle mHandle = nullptr;
    bool mThreaded = false;
    bool mRunning = false;

    std::thread mThread;
    sem_t mSignal = {};
    bool mSignalReady = false;
    std::atomic<bool> mExit{false};

    // Audio -> worker, and worker -> audio. Two rings, each strictly single-producer and
    // single-consumer, which is what makes both directions lock-free.
    SpscByteQueue<64 * 1024> mRequests;
    SpscByteQueue<64 * 1024> mResponses;

    // Staging for one message. Owned by whichever thread is draining, never shared, and sized once
    // here so neither drain path allocates.
    uint8_t mRequestScratch[kMaxMessageBytes] = {};
    uint8_t mResponseScratch[kMaxMessageBytes] = {};

    LV2_Worker_Schedule mSchedule{};
    std::atomic<uint64_t> mDroppedRequests{0};
};

} // namespace NAMp::host
