// Lv2Worker implementation. See lv2worker.h for the protocol and the threading contract.

#include "lv2worker.h"

#include <cerrno>
#include <cstring>
#include <cstdio>

namespace NAMp::host
{

//------------------------------------------------------------------------
Lv2Worker::~Lv2Worker()
{
    stop();
}

//------------------------------------------------------------------------
bool Lv2Worker::start(const LV2_Worker_Interface *iface, LV2_Handle handle, bool threaded)
{
    stop();

    mIface = iface;
    mHandle = handle;
    mThreaded = threaded;

    // The feature is published whether or not the plug-in turns out to use it: it is handed over at
    // instantiate time, before the interface can be queried, so it has to be valid either way. With
    // no interface, schedule() answers LV2_WORKER_ERR_UNKNOWN, which is the honest reply.
    mSchedule.handle = this;
    mSchedule.schedule_work = &Lv2Worker::scheduleCallback;

    if (!iface || !handle || !threaded) {
        mRunning = true;
        return true;
    }

    if (sem_init(&mSignal, 0, 0) != 0) {
        std::fprintf(stderr,
                     "namp-rack: cannot create the LV2 worker semaphore (%s); this plug-in's "
                     "background work will run inline instead\n",
                     std::strerror(errno));
        // Degrade rather than refuse to load. Inline work on the audio thread is wrong, but a
        // plug-in that cannot be loaded at all is worse, and this path is unreachable in practice.
        mThreaded = false;
        mRunning = true;
        return true;
    }
    mSignalReady = true;
    mExit.store(false, std::memory_order_relaxed);
    mThread = std::thread([this] { run(); });
    mRunning = true;
    return true;
}

//------------------------------------------------------------------------
void Lv2Worker::stop()
{
    if (!mRunning)
        return;

    if (mThread.joinable()) {
        mExit.store(true, std::memory_order_release);
        sem_post(&mSignal);
        // Joined, not detached. The worker thread calls into the plug-in, so a thread still running
        // when the instance is freed is a use-after-free with the plug-in's own code on the stack —
        // A worker is joined in terminate() and in the destructor for exactly this reason, never
        // detached.
        mThread.join();
    }
    if (mSignalReady) {
        sem_destroy(&mSignal);
        mSignalReady = false;
    }

    mRequests.clear();
    mResponses.clear();
    mIface = nullptr;
    mHandle = nullptr;
    mRunning = false;
}

//------------------------------------------------------------------------
LV2_Worker_Status Lv2Worker::scheduleCallback(LV2_Worker_Schedule_Handle handle, uint32_t size,
                                              const void *data)
{
    return static_cast<Lv2Worker *>(handle)->schedule(size, data);
}

//------------------------------------------------------------------------
LV2_Worker_Status Lv2Worker::respondCallback(LV2_Worker_Respond_Handle handle, uint32_t size,
                                             const void *data)
{
    return static_cast<Lv2Worker *>(handle)->respond(size, data);
}

//------------------------------------------------------------------------
// AUDIO THREAD (or the owning thread during state restoration).
LV2_Worker_Status Lv2Worker::schedule(uint32_t size, const void *data) noexcept
{
    if (!mIface || !mIface->work)
        return LV2_WORKER_ERR_UNKNOWN;
    if (size > kMaxMessageBytes)
        return LV2_WORKER_ERR_NO_SPACE;

    if (!mThreaded) {
        // Synchronous mode: do it here and now. Only reachable off the audio thread — see the
        // non-threaded note in the header.
        return mIface->work(mHandle, &Lv2Worker::respondCallback, this, size, data);
    }

    if (!mRequests.write(data, size)) {
        mDroppedRequests.fetch_add(1, std::memory_order_relaxed);
        return LV2_WORKER_ERR_NO_SPACE;
    }
    // The one syscall on this path, and it does not block. See the header.
    sem_post(&mSignal);
    return LV2_WORKER_SUCCESS;
}

//------------------------------------------------------------------------
// WORKER THREAD.
LV2_Worker_Status Lv2Worker::respond(uint32_t size, const void *data) noexcept
{
    if (size > kMaxMessageBytes)
        return LV2_WORKER_ERR_NO_SPACE;
    if (!mThreaded) {
        // Synchronous mode: the plug-in expects the response before work() returns.
        return mIface->work_response ? mIface->work_response(mHandle, size, data)
                                     : LV2_WORKER_SUCCESS;
    }
    return mResponses.write(data, size) ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
}

//------------------------------------------------------------------------
void Lv2Worker::run()
{
    while (true) {
        // EINTR is not an error here, just a signal arriving; retrying is the documented handling.
        while (sem_wait(&mSignal) != 0 && errno == EINTR) {
        }

        if (mExit.load(std::memory_order_acquire))
            return;

        // One post may cover several requests if the audio thread got ahead, so drain rather than
        // handling exactly one — otherwise a request can sit in the queue until the next post.
        while (serveOne()) {
        }
    }
}

//------------------------------------------------------------------------
// WHAT THE HOST OWES HERE, AND WHAT IT DOES NOT. The worker header states both: the plug-in "MUST
// NOT make any assumptions about which thread calls this method, except that there are no real-time
// requirements and only one call may be executed at a time", and "the host MAY call this method
// from any non-real-time thread, but MUST NOT make concurrent calls to this method from several
// threads". One thread draining one queue meets both, and there is deliberately no third obligation
// to serialise work() against run() — that is what respond() and work_response() exist for, and a
// host that added a lock across the two would be putting one on the audio thread to fix a problem
// that is not its own.
//
// A plug-in may still race with ITSELF across that boundary, and one here does. Measured under
// ThreadSanitizer, driving guitarix's gx_amp (which requires work:schedule) through a chain of
// three each side: six data races, every one of them a write from Convproc::reset() inside work()
// against a read from Convproc::process() inside run(), both in libzita-convolver's own buffers.
// Our frames appear in those reports only as where the memory was allocated and who created the
// thread. It is untrusted code on the audio thread doing something no host can prevent, which is
// named rather than papered over.
bool Lv2Worker::serveOne()
{
    uint32_t size = 0;
    if (!mRequests.read(mRequestScratch, sizeof(mRequestScratch), size)) {
        // read() returns false both for "empty" and for "that message was too big and was dropped".
        // Either way there is nothing to serve this time round; a dropped message is already
        // counted against the sender, which could only have been our own write().
        return false;
    }
    if (mIface && mIface->work)
        mIface->work(mHandle, &Lv2Worker::respondCallback, this, size, mRequestScratch);
    return true;
}

//------------------------------------------------------------------------
// AUDIO THREAD.
void Lv2Worker::emitResponses() noexcept
{
    if (!mIface || !mIface->work_response || !mThreaded)
        return;

    // Bounded. A worker that responds faster than the audio thread drains must not be able to keep
    // this loop running past the block deadline; whatever is left waits for the next block, which
    // is exactly the latency the extension exists to introduce.
    for (int guard = 0; guard < 64; ++guard) {
        uint32_t size = 0;
        if (!mResponses.read(mResponseScratch, sizeof(mResponseScratch), size))
            return;
        mIface->work_response(mHandle, size, mResponseScratch);
    }
}

//------------------------------------------------------------------------
// AUDIO THREAD. Must come after emitResponses(); the specification defines end_run() as the signal
// that the cycle — responses included — is complete.
void Lv2Worker::endRun() noexcept
{
    if (mIface && mIface->end_run)
        mIface->end_run(mHandle);
}

} // namespace NAMp::host
