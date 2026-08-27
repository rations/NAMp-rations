// ModelBank — the worker thread that turns a path into a playable Bank.
//
// Every expensive thing lives here: directory scanning, JSON parsing, weight extraction, model
// construction, Reset and its prewarm (about 132 ms of inference per model). Ten captures done
// inline on the host's message thread would stall a DAW's UI for most of a second, so none of it
// happens there.
//
// Publication order is what makes the plug-in usable immediately rather than after the whole
// directory finishes: the worker publishes the Bank with no entries ready, then builds entries one
// at a time, always preferring whichever index the audio thread last asked for. So the capture
// under the knob lands first and the rest fill in behind it.
//
// This class must never touch the VST3 API. In particular it must never call allocateMessage() or
// sendMessage(), which are message-thread only. Everything the editor needs to know travels as
// hidden read-only output parameters written from the audio thread instead.

#pragma once

#include "bank.h"
#include "capturesource.h"
#include "spscqueue.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class ModelBank
{
public:
    ModelBank() = default;
    ~ModelBank();

    ModelBank(const ModelBank &) = delete;
    ModelBank &operator=(const ModelBank &) = delete;

    // --- message thread ------------------------------------------------------------------
    void start();
    // Joins the worker. Must be called before destruction and before anything the worker could
    // still be building goes away; a load in flight during teardown is otherwise a use-after-free.
    void stop();

    // Load a directory of captures, ordered by the gain token in each filename. An empty path
    // clears. Cancels anything in flight.
    void loadDirectory(const std::string &path, double slim, int maxBufferSize);
    // Load one capture as a bank of one. An empty path clears.
    void loadFile(const std::string &path, double slim, int maxBufferSize);
    // Rebuild every entry at a new size, from the cached sources — no file I/O, no large parse.
    void setSlim(double slim, int maxBufferSize);

    // Names of the captures in the most recently requested bank, in gain order. Safe to call from
    // the message thread at any time.
    std::vector<std::string> captureNames() const;
    // Last failure, for a single stderr warning at the call site. Empty when the last load was
    // clean.
    std::string lastError() const;

    // Frees a bank the audio thread is no longer using, after stop(). Only valid once the worker
    // is joined — before that, retirement goes through retire().
    static void destroyBank(Bank *bank);

    // --- audio thread --------------------------------------------------------------------
    // Takes ownership of a newly published bank, or nullptr if there is nothing new.
    Bank *takePending()
    {
        return mPending.exchange(nullptr, std::memory_order_acquire);
    }
    // Hands a bank back for deletion. Returns false when the queue is momentarily full, in which
    // case the caller keeps the pointer and tries again on a later block. Never blocks, never
    // deletes.
    bool retire(Bank *bank)
    {
        return mRetire.push(bank);
    }
    // Tells the worker which entry to build next. Relaxed on purpose: a stale hint costs one
    // entry's worth of ordering, never correctness.
    void setPriority(int index)
    {
        mPriority.store(index, std::memory_order_relaxed);
    }
    // Fraction of the current bank that is built and primed, 0 .. 1.
    float progress() const
    {
        return mProgress.load(std::memory_order_relaxed);
    }

private:
    enum class Job { None, Directory, File, Rebuild, Clear };

    struct Request {
        Job job = Job::None;
        std::string path;
        double slim = 1.0;
        int maxBufferSize = engine::kChunk;
        unsigned long long epoch = 0;
    };

    void run();
    void serve(const Request &request);
    // Scans/parses into mSources. Returns false and sets mError on a fatal problem; individual
    // unreadable files are skipped with a warning rather than failing the whole bank.
    bool gatherSources(const Request &request);
    void buildAndPublish(const Request &request);
    void drainRetired();
    bool cancelled(const Request &request) const
    {
        return mEpoch.load(std::memory_order_acquire) != request.epoch;
    }
    void post(Request request);

    std::thread mThread;
    mutable std::mutex mMutex;
    std::condition_variable mCv;
    Request mRequest; // guarded by mMutex
    bool mHasRequest = false;
    bool mQuit = false;
    std::string mError;              // guarded by mMutex
    std::vector<std::string> mNames; // guarded by mMutex

    // Worker-owned. The parsed form of every capture, kept so a Slim change costs no file I/O.
    std::vector<CaptureSource> mSources;

    // Bumped on every new request. The worker compares it while building and abandons a bank the
    // moment it is stale, which is also what makes a sample-rate or teardown race impossible.
    std::atomic<unsigned long long> mEpoch{0};

    std::atomic<Bank *> mPending{nullptr};
    std::atomic<int> mPriority{0};
    std::atomic<float> mProgress{0.0f};

    // Audio thread -> worker. Capacity covers far more banks than can plausibly be in flight; a
    // full queue is handled by the caller retrying, not by blocking.
    SpscQueue<Bank, 8> mRetire;
};

} // namespace Rations
