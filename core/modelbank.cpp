// ModelBank implementation. See modelbank.h for the ownership and cancellation rules.

#include "modelbank.h"

#include "platform/respath.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <filesystem>

namespace Rations
{

namespace fs = std::filesystem;

//------------------------------------------------------------------------
ModelBank::~ModelBank()
{
    stop();
}

//------------------------------------------------------------------------
void ModelBank::start()
{
    if (mThread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQuit = false;
    }
    mThread = std::thread([this] { run(); });
}

//------------------------------------------------------------------------
void ModelBank::stop()
{
    if (mThread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mQuit = true;
        }
        // Bump the epoch so a build in flight abandons its bank promptly instead of finishing all
        // ten models before noticing it should stop.
        mEpoch.fetch_add(1, std::memory_order_release);
        mCv.notify_all();
        mThread.join();
    }
    // The worker is gone, so nothing else can touch these.
    destroyBank(mPending.exchange(nullptr, std::memory_order_acquire));
    while (Bank *bank = mRetire.pop())
        destroyBank(bank);
}

//------------------------------------------------------------------------
void ModelBank::destroyBank(Bank *bank)
{
    delete bank;
}

//------------------------------------------------------------------------
void ModelBank::post(Request request)
{
    request.epoch = mEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mRequest = std::move(request);
        mHasRequest = true;
    }
    mCv.notify_one();
}

//------------------------------------------------------------------------
void ModelBank::loadDirectory(const std::string &path, double slim, int maxBufferSize)
{
    Request r;
    r.job = path.empty() ? Job::Clear : Job::Directory;
    r.path = path;
    r.slim = slim;
    r.maxBufferSize = maxBufferSize;
    post(std::move(r));
}

void ModelBank::loadFile(const std::string &path, double slim, int maxBufferSize)
{
    Request r;
    r.job = path.empty() ? Job::Clear : Job::File;
    r.path = path;
    r.slim = slim;
    r.maxBufferSize = maxBufferSize;
    post(std::move(r));
}

void ModelBank::setSlim(double slim, int maxBufferSize)
{
    Request r;
    r.job = Job::Rebuild;
    r.slim = slim;
    r.maxBufferSize = maxBufferSize;
    post(std::move(r));
}

//------------------------------------------------------------------------
std::vector<std::string> ModelBank::captureNames() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mNames;
}

CaptureLevels ModelBank::captureLevels() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mLevels;
}

std::string ModelBank::lastError() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mError;
}

//------------------------------------------------------------------------
void ModelBank::run()
{
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            // A timed wait, not an indefinite one. The audio thread pushes retired banks without
            // notifying: notifying would mean locking this mutex on the audio thread, which is
            // exactly the thing the whole design exists to avoid. So the worker wakes up on its
            // own from time to time and collects whatever has accumulated.
            mCv.wait_for(lock, std::chrono::milliseconds(250),
                         [this] { return mQuit || mHasRequest; });
            if (mQuit)
                break;
            if (mHasRequest) {
                request = mRequest;
                mHasRequest = false;
            }
        }
        drainRetired();
        if (request.job != Job::None)
            serve(request);
        drainRetired();
    }
    drainRetired();
}

//------------------------------------------------------------------------
void ModelBank::drainRetired()
{
    while (Bank *bank = mRetire.pop())
        destroyBank(bank);
}

//------------------------------------------------------------------------
void ModelBank::serve(const Request &request)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mError.clear();
    }
    if (request.job == Job::Clear) {
        mSources.clear();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mNames.clear();
            mLevels = CaptureLevels();
        }
        mProgress.store(0.0f, std::memory_order_relaxed);
        // Publishing an empty bank is how the audio thread learns to stop using the old one.
        auto *bank = new Bank();
        bank->epoch = request.epoch;
        destroyBank(mPending.exchange(bank, std::memory_order_release));
        return;
    }

    if (request.job != Job::Rebuild && !gatherSources(request))
        return;
    if (mSources.empty() || cancelled(request))
        return;
    buildAndPublish(request);
}

//------------------------------------------------------------------------
bool ModelBank::gatherSources(const Request &request)
{
    mSources.clear();

    std::vector<fs::path> files;
    std::error_code ec;

    // request.path came from a state blob or from a host calling INampFileLoader,
    // so it is untrusted. On Windows a string that is not valid UTF-8 cannot be
    // made into a path at all and throws while being constructed — which, on
    // this thread, would be an uncaught exception out of a worker (respath.h).
    // Converted once, here, so nothing below has to think about it again.
    fs::path root;
    if (!utf8ToPath(request.path, root)) {
        std::lock_guard<std::mutex> lock(mMutex);
        mError = "not a usable path: " + request.path;
        return false;
    }

    if (request.job == Job::File) {
        files.push_back(root);
    } else {
        if (!fs::is_directory(root, ec)) {
            std::lock_guard<std::mutex> lock(mMutex);
            mError = "not a directory: " + request.path;
            return false;
        }
        // A directory is untrusted input, so the work is bounded twice over — but at the right
        // places. Listing names is cheap, so the enumeration cap is high and exists only to stop a
        // pathological directory from spinning forever. PARSING is what costs (about a megabyte of
        // JSON and 130 ms of prewarm per file), so the real cap is applied after sorting, which
        // means the captures kept are the lowest-numbered ones rather than whichever ones the
        // filesystem happened to hand back first.
        constexpr int kMaxNamesEnumerated = 4096;
        int seen = 0;
        for (fs::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            if (++seen > kMaxNamesEnumerated)
                break;
            if (!it->is_regular_file(ec))
                continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".nam")
                continue;
            files.push_back(it->path());
        }
        std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) {
            return captureFilenameLess(a.filename().string(), b.filename().string());
        });
        if (static_cast<int>(files.size()) > Bank::kMaxEntries) {
            // Say so. Truncating in silence means a 70-capture directory loads, plays and sweeps
            // without complaint while the top of the amp's gain range is simply absent — a
            // failure that looks exactly like a successful load. Named files, not just a count,
            // because which end got dropped is the whole question: the sort above is by capture
            // number, so what goes is the HIGHEST-gain captures.
            fprintf(stderr,
                    "Rations: %s holds %zu captures; the bank is capped at %d, so %zu were dropped "
                    "(kept up to \"%s\", dropped from \"%s\" on)\n",
                    request.path.c_str(), files.size(), Bank::kMaxEntries,
                    files.size() - static_cast<size_t>(Bank::kMaxEntries),
                    files[static_cast<size_t>(Bank::kMaxEntries) - 1].filename().string().c_str(),
                    files[static_cast<size_t>(Bank::kMaxEntries)].filename().string().c_str());
            files.resize(Bank::kMaxEntries);
        }
    }

    std::string firstError;
    int skipped = 0;
    for (const fs::path &file : files) {
        if (cancelled(request))
            return false;
        CaptureSource source;
        std::string error;
        if (!loadCaptureSource(file, source, error)) {
            // One bad file must not take the bank down; report it once and carry on.
            if (firstError.empty())
                firstError = file.filename().string() + ": " + error;
            ++skipped;
            fprintf(stderr, "Rations: skipping %s (%s)\n", file.filename().string().c_str(),
                    error.c_str());
            continue;
        }
        mSources.push_back(std::move(source));
    }

    std::vector<std::string> names;
    names.reserve(mSources.size());
    for (const CaptureSource &s : mSources)
        names.push_back(s.filename);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mNames = std::move(names);
        if (mSources.empty())
            mError = firstError.empty() ? "no readable captures in " + request.path : firstError;
        else if (skipped > 0)
            mError = firstError;
    }
    return !mSources.empty();
}

//------------------------------------------------------------------------
void ModelBank::buildAndPublish(const Request &request)
{
    const int count = static_cast<int>(std::min<size_t>(mSources.size(), Bank::kMaxEntries));

    auto *bank = new Bank();
    bank->count = count;
    bank->epoch = request.epoch;
    bank->isDirectory = request.job != Job::File;
    for (int i = 0; i < count; ++i)
        bank->entries[i].name = mSources[static_cast<size_t>(i)].filename;

    mProgress.store(0.0f, std::memory_order_relaxed);
    {
        // Nothing is built yet, so nothing is known yet. Starting from all-false rather than
        // all-true matters: the flags are ANDed as entries land below, and a bank whose entries
        // all fail to build must report that it supports nothing rather than everything vacuously.
        std::lock_guard<std::mutex> lock(mMutex);
        mLevels = CaptureLevels();
        // Slimmable is the exception, and is settled HERE rather than accumulated below: it is a
        // fact about the parsed file, not about the model that gets built from it, so it is known
        // now and needs no entry to have landed. Every source must offer more than one variant -
        // an older single-variant capture in the bank means Slim cannot act on the whole of it.
        mLevels.slimmable = count > 0;
        for (int i = 0; i < count && mLevels.slimmable; ++i)
            mLevels.slimmable = mSources[static_cast<size_t>(i)].submodels.size() > 1;
    }

    // Publish before anything is ready. The audio thread will output ramped silence until the
    // first entry lands, which is a fraction of a second, and in exchange the plug-in starts
    // sounding as soon as the capture under the knob is built rather than after all of them.
    Bank *previous = mPending.exchange(bank, std::memory_order_release);
    // If the audio thread never collected the previous bank, it never saw it, so this thread
    // still owns it.
    destroyBank(previous);

    int built = 0;
    std::vector<bool> done(static_cast<size_t>(count), false);
    // Whether the level summary has been seeded yet. Not derivable from `built`, which counts
    // attempts rather than successes: an unreadable first capture increments it and produces no
    // metadata, and seeding from that entry would start the AND below from three false flags.
    bool levelsSeeded = false;

    while (built < count) {
        if (cancelled(request)) {
            // Stop touching this bank immediately. It is either still in mPending (and will be
            // freed by the next publish) or already with the audio thread (and will come back
            // through the retirement queue). Either way it is no longer ours to write to.
            return;
        }

        // Serve the audio thread's current position first, then fill outwards from it, so a knob
        // parked in the middle of the bank does not wait for the low-gain end to finish.
        int next = -1;
        const int wanted = mPriority.load(std::memory_order_relaxed);
        const int hinted = wanted < 0 ? 0 : (wanted >= count ? count - 1 : wanted);
        for (int radius = 0; radius < count && next < 0; ++radius) {
            const int lo = hinted - radius;
            const int hi = hinted + radius;
            if (lo >= 0 && !done[static_cast<size_t>(lo)])
                next = lo;
            else if (hi < count && !done[static_cast<size_t>(hi)])
                next = hi;
        }
        if (next < 0)
            break;

        std::string error;
        auto model = buildCaptureModel(mSources[static_cast<size_t>(next)], request.slim,
                                       request.maxBufferSize, error);
        done[static_cast<size_t>(next)] = true;
        ++built;

        if (!model) {
            fprintf(stderr, "Rations: could not build %s (%s)\n",
                    mSources[static_cast<size_t>(next)].filename.c_str(), error.c_str());
            std::lock_guard<std::mutex> lock(mMutex);
            if (mError.empty())
                mError = mSources[static_cast<size_t>(next)].filename + ": " + error;
            continue; // entry stays not-ready; the engine skips it
        }

        // Re-check before writing: if the request went stale while this model was being built and
        // prewarmed, the bank may already belong to someone else.
        if (cancelled(request))
            return;

        BankEntry &entry = bank->entries[next];
        entry.prewarmSamples = model->GetPrewarmSamples();
        entry.hasLoudness = model->HasLoudness();
        if (entry.hasLoudness)
            entry.loudnessDb = model->GetLoudness();
        entry.hasInputLevel = model->HasInputLevel();
        if (entry.hasInputLevel)
            entry.inputLevelDbu = model->GetInputLevel();
        entry.hasOutputLevel = model->HasOutputLevel();
        if (entry.hasOutputLevel)
            entry.outputLevelDbu = model->GetOutputLevel();
        entry.model = std::move(model);
        // Release: everything written above must be visible to any thread that observes ready.
        entry.ready.store(true, std::memory_order_release);

        // Fold this entry into the bank-wide summary the editor reads. The first built entry seeds
        // it and every one after that can only take a flag away, which is the AND the summary is
        // defined as - a mode is offered only when every capture in the bank can honour it.
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!levelsSeeded) {
                levelsSeeded = true;
                mLevels.hasLoudness = entry.hasLoudness;
                mLevels.hasInputLevel = entry.hasInputLevel;
                mLevels.hasOutputLevel = entry.hasOutputLevel;
            } else {
                mLevels.hasLoudness = mLevels.hasLoudness && entry.hasLoudness;
                mLevels.hasInputLevel = mLevels.hasInputLevel && entry.hasInputLevel;
                mLevels.hasOutputLevel = mLevels.hasOutputLevel && entry.hasOutputLevel;
            }
        }

        mProgress.store(static_cast<float>(built) / static_cast<float>(count),
                        std::memory_order_relaxed);
    }
}

} // namespace Rations
