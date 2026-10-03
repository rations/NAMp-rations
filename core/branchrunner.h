// BranchRunner — a second thread that may run ONE model call for the audio thread.
//
// While a dial on the sounding channel moves, its CrossfadeEngine runs two models per chunk,
// branch A and branch B, and on one thread that doubles the amp's cost for the length of the
// sweep. The two calls are independent — two different nam::DSP instances, read-only shared
// input, separate outputs — so they can run at the same time on two cores. This class is the
// other core.
//
// The protocol is one job slot and one atomic state, and its shape is the safety argument:
//
//   Idle --post (RT)--> Posted --claim (helper)--> Running --finish (helper)--> Done
//                         |
//                         +--takeBack (RT)--> Idle      (RT runs the job itself)
//
//   * RT never waits for work that has not STARTED. After running branch A it tries to take the
//     job back with one compare-exchange; if the helper has not claimed it yet, RT runs branch B
//     itself and the block costs what it cost before this class existed. A helper that is late,
//     starved, or not running at all therefore costs nothing but a wasted post.
//   * RT waits only for a job the helper has already claimed, and that wait is bounded by one
//     model call on at most engine::kChunk samples. It spins on an acquire load with a pause
//     instruction; there is no syscall and no lock in it.
//   * Ownership is exclusive. From the post until RT has observed Done (or taken the job back),
//     the model, its input and its output buffer belong to the job, and RT touches none of them.
//     The slot's plain fields are written by RT before the release store of Posted and read by
//     the helper only after its acquire-release claim, so the atomic carries them.
//   * The helper is a real-time thread under every rule the audio thread is under: it allocates
//     nothing after start(), takes no lock, arms flush-to-zero exactly as RT does (so a branch
//     computed here is bit-identical to the same branch computed there), and is joined in stop().
//
// Waking the helper is a semaphore post per job: sem_post on Linux and ReleaseSemaphore on Windows.
// Neither blocks (sem_post is also async-signal-safe, signal-safety(7)), so both are fit for the
// audio thread. The helper sleeps on the semaphore between jobs and burns no core while the dial is
// still. A post that fails — a count at its ceiling — only means the helper is not woken, and RT
// then takes the job back, so the result is not checked on RT, where there is nothing to do with
// it.
//
// Priority is the helper's own business and never RT's, and it decides whether the helper may
// claim at all. The rule is that the helper is NEVER LESS REAL-TIME THAN THE AUDIO THREAD: a job
// claimed by a thread the scheduler can preempt in favour of ordinary work is a job RT could be
// left waiting on for a whole time slice. So:
//
//   * Linux: RT publishes its thread handle (a plain value, no syscall) and the helper copies that
//     thread's scheduling policy and priority onto itself. That works under JACK, inside a DAW and
//     inside a bridge alike, because it asks the thread actually running the audio rather than
//     guessing what the host gave it. If the audio thread is real-time and the copy is refused,
//     the helper claims nothing and every job is taken back. If the audio thread is NOT real-time
//     (an offline render, a test tool), the helper is on the same footing as it and may claim.
//   * Windows: the helper asks MMCSS for "Pro Audio", as the standalone's WASAPI thread does, and
//     claims nothing if that is refused. avrt.dll is loaded at run time rather than linked, so the
//     plug-in carries no new import. UNVERIFIED on a real Windows machine under ASIO — flagged.
//   * Everywhere else (macOS): not implemented. start() leaves the runner stopped and every job is
//     run serially, exactly as before this class existed. The macOS mechanism is an audio
//     workgroup, and nothing about it can be checked on the machine this was written on.

#pragma once

#include "NAM/dsp.h"

#include <atomic>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <semaphore.h>
#endif

namespace Rations
{

//------------------------------------------------------------------------
class BranchRunner
{
public:
    BranchRunner();
    ~BranchRunner();
    BranchRunner(const BranchRunner &) = delete;
    BranchRunner &operator=(const BranchRunner &) = delete;

    // Non-RT. Starts the helper thread. A no-op where the platform has no implementation, which
    // leaves the runner stopped: running() stays false and the engine runs both branches itself.
    void start();
    // Non-RT. Joins the helper. Must not race a process() call, which the plug-in lifecycle
    // already guarantees: terminate() never overlaps the audio thread.
    void stop();

    // RT. True when a helper exists to take a job. Checked before posting, so a stopped runner
    // costs one load and no syscall.
    bool running() const
    {
        return mRunning.load(std::memory_order_acquire);
    }

    // RT. Tell the helper which thread is the audio thread, so it can match that thread's
    // scheduling. Cheap enough to call every block; it only stores when the thread changes.
    void noteAudioThread();

    // RT. Hand one model call to the helper. The caller must later call either takeBack() or
    // waitDone() before touching model, in or out again.
    void post(nam::DSP *model, NAM_SAMPLE *in, NAM_SAMPLE *out, int numFrames);
    // RT. Returns true if the job had not been claimed and is now RT's again, to run itself.
    // Returns false if the helper is running it, in which case waitDone() must follow.
    bool takeBack();
    // RT. Waits for a job the helper has already claimed. Bounded by one model call.
    void waitDone();

    // Diagnostics, any thread: how many jobs the helper ran, and how many RT took back and ran
    // itself. The tools read them to prove the parallel path was actually taken, because a runner
    // that never wins a single claim is indistinguishable from serial by output alone.
    unsigned long long jobsRun() const
    {
        return mJobsRun.load(std::memory_order_relaxed);
    }
    unsigned long long jobsTakenBack() const
    {
        return mJobsTakenBack.load(std::memory_order_relaxed);
    }

private:
    enum State : int {
        kIdle = 0,
        kPosted = 1,
        kRunning = 2,
        kDone = 3,
    };

    void loop();
    // Called by the helper on itself, between jobs. Returns whether it may claim: true only when
    // it is at least as real-time as the audio thread. See the top of this file.
    bool mayClaim();

    // The job slot. Plain fields, published by the release store of kPosted and consumed after
    // the helper's acquire-release claim; see the protocol at the top of this file.
    nam::DSP *mModel = nullptr;
    NAM_SAMPLE *mIn = nullptr;
    NAM_SAMPLE *mOut = nullptr;
    int mFrames = 0;

    std::atomic<int> mState{kIdle};
    // Each written by one thread only: the helper counts what it ran, RT counts what it took back.
    std::atomic<unsigned long long> mJobsRun{0};
    std::atomic<unsigned long long> mJobsTakenBack{0};
    std::atomic<bool> mRunning{false};
    std::thread mThread;

#if defined(__linux__)
    sem_t mWake;
    bool mWakeValid = false;
    // The audio thread's handle as RT last published it. Atomic because RT writes it and the
    // helper reads it; pthread_t is an integer on glibc.
    std::atomic<pthread_t> mAudioThread{};
    std::atomic<bool> mAudioThreadKnown{false};
    // Helper-only: which audio thread the helper last matched, and the verdict it reached.
    pthread_t mMatchedThread{};
    bool mMatched = false;
    bool mClaimAllowed = false;
#elif defined(_WIN32)
    void *mWake = nullptr;      // HANDLE of a semaphore; void* keeps <windows.h> out of this header
    bool mClaimAllowed = false; // helper-only: whether MMCSS took it
#endif
};

} // namespace Rations
