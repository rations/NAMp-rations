// BranchRunner implementation. See branchrunner.h for the protocol and why it is safe.

#include "branchrunner.h"

#include "platform/rtdenormal.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Rations
{

namespace
{

// One spin-wait step. PAUSE on x86 and YIELD on AArch64 tell the core this is a spin loop, which
// saves power and frees the sibling hyper-thread; neither is a syscall or a scheduler yield.
inline void cpuRelax()
{
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

#if defined(_WIN32)
// avrt.dll's two MMCSS entry points, resolved at run time so the plug-in gains no import. The
// signatures are avrt.h's (MinGW-w64): AvSetMmThreadCharacteristicsW(LPCWSTR, LPDWORD) returning a
// HANDLE, and AvRevertMmThreadCharacteristics(HANDLE) returning a BOOL.
using AvSetFn = HANDLE(WINAPI *)(LPCWSTR, LPDWORD);
using AvRevertFn = BOOL(WINAPI *)(HANDLE);

// GetProcAddress returns a FARPROC; going through a generic function pointer is the conversion
// GCC's -Wcast-function-type accepts without complaint.
template <typename Fn> Fn resolve(HMODULE module, const char *name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}
#endif

} // namespace

//------------------------------------------------------------------------
BranchRunner::BranchRunner()
{
#if defined(__linux__)
    mWakeValid = sem_init(&mWake, 0, 0) == 0;
    if (!mWakeValid)
        std::fprintf(stderr, "rations: branch runner unavailable (sem_init: %s)\n",
                     std::strerror(errno));
#elif defined(_WIN32)
    mWake = CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
    if (!mWake)
        std::fprintf(stderr, "rations: branch runner unavailable (CreateSemaphoreW: %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
#endif
}

BranchRunner::~BranchRunner()
{
    stop();
#if defined(__linux__)
    if (mWakeValid)
        sem_destroy(&mWake);
#elif defined(_WIN32)
    if (mWake)
        CloseHandle(static_cast<HANDLE>(mWake));
#endif
}

//------------------------------------------------------------------------
void BranchRunner::start()
{
#if defined(__linux__) || defined(_WIN32)
#if defined(__linux__)
    if (!mWakeValid)
        return;
    mMatched = false;
#else
    if (!mWake)
        return;
#endif
    if (mThread.joinable())
        return;
    mState.store(kIdle, std::memory_order_relaxed);
    mClaimAllowed = false;
    mThread = std::thread([this] { loop(); });
    mRunning.store(true, std::memory_order_release);
#endif
}

void BranchRunner::stop()
{
    if (!mThread.joinable())
        return;
    mRunning.store(false, std::memory_order_release);
#if defined(__linux__)
    sem_post(&mWake);
#elif defined(_WIN32)
    ReleaseSemaphore(static_cast<HANDLE>(mWake), 1, nullptr);
#endif
    mThread.join();
}

//------------------------------------------------------------------------
void BranchRunner::noteAudioThread()
{
#if defined(__linux__)
    const pthread_t self = pthread_self();
    if (!mAudioThreadKnown.load(std::memory_order_relaxed) ||
        !pthread_equal(mAudioThread.load(std::memory_order_relaxed), self)) {
        mAudioThread.store(self, std::memory_order_relaxed);
        mAudioThreadKnown.store(true, std::memory_order_release);
    }
#endif
}

//------------------------------------------------------------------------
void BranchRunner::post(nam::DSP *model, NAM_SAMPLE *in, NAM_SAMPLE *out, int numFrames)
{
    mModel = model;
    mIn = in;
    mOut = out;
    mFrames = numFrames;
    mState.store(kPosted, std::memory_order_release);
    // Not checked, deliberately: a failed post means the helper is not woken, and RT then takes
    // the job back and runs it itself. See the header.
#if defined(__linux__)
    sem_post(&mWake);
#elif defined(_WIN32)
    ReleaseSemaphore(static_cast<HANDLE>(mWake), 1, nullptr);
#endif
}

bool BranchRunner::takeBack()
{
    int expected = kPosted;
    // Acquire on failure too: if the helper got there first, everything after this point reads
    // state the helper published.
    if (!mState.compare_exchange_strong(expected, kIdle, std::memory_order_acq_rel,
                                        std::memory_order_acquire))
        return false;
    mJobsTakenBack.store(mJobsTakenBack.load(std::memory_order_relaxed) + 1,
                         std::memory_order_relaxed);
    return true;
}

void BranchRunner::waitDone()
{
    while (mState.load(std::memory_order_acquire) != kDone)
        cpuRelax();
    mState.store(kIdle, std::memory_order_relaxed);
}

//------------------------------------------------------------------------
bool BranchRunner::mayClaim()
{
#if defined(__linux__)
    if (!mAudioThreadKnown.load(std::memory_order_acquire))
        return false;
    const pthread_t audio = mAudioThread.load(std::memory_order_relaxed);
    if (mMatched && pthread_equal(audio, mMatchedThread))
        return mClaimAllowed;
    mMatched = true;
    mMatchedThread = audio;
    mClaimAllowed = false;

    int policy = 0;
    sched_param param = {};
    int err = pthread_getschedparam(audio, &policy, &param);
    if (err != 0) {
        std::fprintf(stderr,
                     "rations: branch runner cannot read the audio thread's priority (%s); it "
                     "stays idle and the audio thread runs both branches\n",
                     std::strerror(err));
        return false;
    }
    if (policy != SCHED_FIFO && policy != SCHED_RR) {
        // The audio thread is not real-time, so a helper at the default policy is on the same
        // footing as it and may share its work.
        mClaimAllowed = true;
        return true;
    }
    err = pthread_setschedparam(pthread_self(), policy, &param);
    if (err != 0) {
        std::fprintf(stderr,
                     "rations: branch runner cannot match the audio thread's real-time priority "
                     "(%s); it stays idle and the audio thread runs both branches\n",
                     std::strerror(err));
        return false;
    }
    mClaimAllowed = true;
    return true;
#elif defined(_WIN32)
    return mClaimAllowed;
#else
    return false;
#endif
}

void BranchRunner::loop()
{
    // The same floating-point mode as the audio thread, so a branch computed here is bit-identical
    // to the same branch computed there.
    rtSetDenormalMode();

#if defined(_WIN32)
    HANDLE mmcss = nullptr;
    AvRevertFn avRevert = nullptr;
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (avrt) {
        const AvSetFn avSet = resolve<AvSetFn>(avrt, "AvSetMmThreadCharacteristicsW");
        avRevert = resolve<AvRevertFn>(avrt, "AvRevertMmThreadCharacteristics");
        DWORD taskIndex = 0;
        if (avSet && avRevert)
            mmcss = avSet(L"Pro Audio", &taskIndex);
    }
    mClaimAllowed = mmcss != nullptr;
    if (!mClaimAllowed)
        std::fprintf(stderr, "rations: branch runner could not join MMCSS \"Pro Audio\"; it stays "
                             "idle and the audio thread runs both branches\n");
#endif

    for (;;) {
#if defined(__linux__)
        if (sem_wait(&mWake) != 0) {
            if (errno == EINTR)
                continue;
            std::fprintf(stderr, "rations: branch runner stopped (sem_wait: %s)\n",
                         std::strerror(errno));
            break;
        }
#elif defined(_WIN32)
        if (WaitForSingleObject(static_cast<HANDLE>(mWake), INFINITE) != WAIT_OBJECT_0) {
            std::fprintf(stderr, "rations: branch runner stopped (WaitForSingleObject: %lu)\n",
                         static_cast<unsigned long>(GetLastError()));
            break;
        }
#else
        break;
#endif
        if (!mRunning.load(std::memory_order_acquire))
            break;
        // A helper that may not claim simply never does: every job it is woken for is taken back
        // by RT, which is the serial path.
        if (!mayClaim())
            continue;

        // A wake can be stale: RT may have taken the job back already, and every post counts one
        // wake whether or not the helper was the one that ran it. The claim settles it.
        int expected = kPosted;
        if (!mState.compare_exchange_strong(expected, kRunning, std::memory_order_acq_rel,
                                            std::memory_order_relaxed))
            continue;
        NAM_SAMPLE *in = mIn;
        NAM_SAMPLE *out = mOut;
        mModel->process(&in, &out, mFrames);
        mJobsRun.store(mJobsRun.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        mState.store(kDone, std::memory_order_release);
    }

#if defined(_WIN32)
    if (mmcss && !avRevert(mmcss))
        std::fprintf(stderr, "rations: branch runner could not leave MMCSS (%lu)\n",
                     static_cast<unsigned long>(GetLastError()));
    // LoadLibraryW counts references, so this releases only the one taken above.
    if (avrt)
        FreeLibrary(avrt);
#endif
}

} // namespace Rations
