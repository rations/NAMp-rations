// AsioBackend — the ASIO implementation of AudioBackend, and the primary Windows one.
//
// What this class is FOR, and the three things that cross the RT boundary, are described once in
// audiobackend.h and not repeated here. What follows is only what is specific to ASIO.
//
// ASIO IS JACK-SHAPED, which is why this file reads like jackclient.cpp. One driver, one callback,
// input and output together on one clock, a thread the driver owns rather than one this class
// creates, and a buffer size fixed for the session. So the shape is the same: the process callback
// obeys the same contract the plug-in's own process() does — no allocation, no locks, no logging,
// no file I/O — every VST3 structure is allocated once in open(), and the amp is bracketed by the
// chain engine's beginBlock/beginChunk/endChunk exactly as it is on Linux.
//
// THREE THINGS ARE GENUINELY DIFFERENT, and each shapes the file:
//
//   * THE BUFFERS ARE NOT FLOATS. A driver hands out its own format — ASIOGetChannelInfo reports it
//     per channel and it is commonly 32-bit integer — so unlike the JACK backend, which aims the
//     VST3 bus pointers straight at JACK's memory, this one converts into float buffers of its own
//     and converts back out. pcmsamples.h is that arithmetic — shared with the WASAPI backend,
//     because the layouts the two APIs describe are the same ones — and it is proved offline by
//     tools/namp_audiocheck, because no driver exists on the machine this is written on.
//
//   * MIDI IS A THIRD API. ASIO carries none, so the footswitch arrives through WinMM on a thread
//     the operating system owns and crosses into the audio thread through a ring. See winmmmidi.h.
//
//   * THE DRIVER CAN ASK TO BE REOPENED. kAsioResetRequest is how a driver reports that its own
//     control panel changed something fundamental, and it arrives while it is inside its callback.
//     There is no reconfiguring anything from there, so it is recorded and taken by the run loop
//     through takeDeviceReset().
//
// ONE DRIVER PER PROCESS, BY THE SDK'S CONSTRUCTION. The SDK's host side keeps the loaded driver in
// a global (theAsioDriver) and its callbacks are plain C function pointers with no user data, so
// there is one instance of this class at a time and the callbacks find it through a file-static
// pointer. That is the SDK's design and not a shortcut taken here; a standalone opens one device.
//
// COM: ASIO REQUIRES A SINGLE-THREADED APARTMENT on the thread that opens the driver, because the
// driver is a COM object instantiated with CoCreateInstance. open() initialises one and close()
// releases it, so the thread that opens must be the thread that closes — which is the run loop, the
// same thread every other method here is called on.

#pragma once

#include "audiobackend.h"

#include "host/chainengine.h"
#include "midiroute.h"
#include "winmmmidi.h"

#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
// THE ATTRIBUTION THE ASIO LICENCE REQUIRES IN THE SHIPPED PRODUCT, quoted exactly and not
// paraphrased.
//
// The agreement's own words, for a product distributed as a download, are that "ASIO" and
// Steinberg's notice — or the ASIO compatible logo and that notice — appear in an About box and/or
// a startup screen and/or the bundled documentation. This project has no About box, so it takes the
// other two: the standalone prints this line at startup, and the installer puts it in the
// documentation it installs beside the binary.
//
// IT IS A CONSTANT RATHER THAN A LITERAL AT THE PRINT SITE so that there is one spelling of it in
// the tree and a gate can check for that one. An attribution that drifted by a word would still
// look right and would no longer be the string the agreement fixes.
//
// It is also why the product is never NAMED after it: "ASIO compatible" or "for ASIO" in regular
// type is permitted, ASIO in a product or firm name is not.
constexpr const char *kAsioTrademarkNotice =
    "ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other "
    "countries";

//------------------------------------------------------------------------
// What the setup page's picker chose. Separate from open() because R6's interface signature is
// shared with JACK, which needs none of it: on Linux the server decides the rate and the block size
// and the standalone reports what it was given, while on Windows the user picks and the choice has
// to persist.
struct AsioSettings {
    // The driver's own name, as the picker listed it. Empty means "the first one the machine has",
    // which is what a fresh install gets.
    std::string driverName;

    // 0 asks the driver for its preferred size, which is the right default: it is the size the
    // hardware is actually built around, and a vendor's preferred figure beats anything guessed
    // here. Anything else is clamped into the driver's min/max and onto its granularity.
    int blockSize = 0;

    // 0 keeps whatever rate the driver is already at, which matters more on ASIO than elsewhere:
    // the rate can be owned by an external clock or by another application, and demanding one would
    // fail on hardware that is slaved.
    double sampleRate = 0.0;

    // Which of the device's channels the guitar is in and the amp comes out of. A two-input
    // interface has the instrument on either, and a multi-output one may not have monitors on 1/2.
    int inputChannel = 0;
    int outputChannelL = 0;
    int outputChannelR = 1;

    // The MIDI input for the footswitch, or WinMmMidiIn::kNoDevice. Nothing is opened by default —
    // see winmmmidi.h on why guessing is worse than asking.
    int midiDevice = WinMmMidiIn::kNoDevice;
};

//------------------------------------------------------------------------
class AsioBackend final : public AudioBackend
{
public:
    ~AsioBackend() override;

    // --- what the picker needs (main thread, before open) -----------------
    // The drivers installed on this machine. Reading the list does NOT load any of them: it is a
    // registry enumeration, so an unplugged interface or a broken driver still lists and simply
    // fails to open. Names are what the driver registered itself as, which is what the user will
    // recognise from every other audio application.
    static int driverCount();
    static bool driverName(int index, std::string &out);

    // The sizes and rates the CHOSEN driver will accept, for the picker to offer. Each loads the
    // driver, asks, and unloads it, so neither may be called while this backend is open — which is
    // also why the picker asks before opening rather than while running.
    struct DriverCapabilities {
        bool ok = false;
        int minBlock = 0;
        int maxBlock = 0;
        int preferredBlock = 0;
        int blockGranularity = 0; // -1 means powers of two, per the SDK
        int inputChannels = 0;
        int outputChannels = 0;
        double currentRate = 0.0;
        // Of the rates a guitar amp is ever asked for. A driver slaved to an external clock may
        // accept only the one it is locked to.
        std::vector<double> rates;
        std::string error;
    };
    static DriverCapabilities probe(const std::string &driverName, void *systemWindow);

    // --- configuration (main thread, before open) --------------------------
    void configure(const AsioSettings &settings)
    {
        mSettings = settings;
    }
    const AsioSettings &settings() const
    {
        return mSettings;
    }

    // The top-level window handle, which ASIOInit takes as its sysRef. Drivers use it as the parent
    // for their own control panel and for the message boxes some of them put up; passing null works
    // on most and puts a dialog behind everything on the rest.
    void setSystemWindow(void *hwnd)
    {
        mSystemWindow = hwnd;
    }

    // What the driver actually gave us, once open. Reported rather than assumed — the picker's
    // request is a request.
    const std::string &openedDriver() const
    {
        return mOpenedDriver;
    }

    // --- AudioBackend -----------------------------------------------------
    bool open(const char *clientName, Steinberg::Vst::IAudioProcessor *processor,
              Steinberg::Vst::IComponent *component, const MidiRoute *route) override;
    void close() override;

    void setChainEngine(NAMp::host::ChainEngine *engine) override
    {
        mChain = engine;
    }
    // ASIO has no graph to tell. ASIOGetLatencies is how a driver REPORTS its own latency to us,
    // not a way to declare ours to anything else — there is nothing downstream of the device — so
    // this is a no-op rather than a stub, and the figure it would have carried is printed at open()
    // where a person can read it.
    void notifyLatencyChanged() override
    {
    }

    bool isOpen() const override
    {
        return mOpen;
    }
    double sampleRate() const override
    {
        return mSampleRate;
    }
    int blockSize() const override
    {
        return mBlockSize.load(std::memory_order_relaxed);
    }

    int takeBufferSizeChange() override;
    bool suspendProcessing() override;
    void resumeProcessing(int blockSize) override;
    bool takeDeviceReset() override;

    bool pushParameter(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) override;
    bool readFeedback(int index, Steinberg::Vst::ParamID &id, double &value,
                      uint32_t &seq) const override;

    // Overloads the driver reported, plus blocks this process failed to finish inside their own
    // period. See the note on mLateBlocks: both are dropouts, and only counting the driver's would
    // leave the live gate reading zero on every driver that does not report them.
    uint32_t dropouts() const override
    {
        return mOverloads.load(std::memory_order_relaxed) +
               mLateBlocks.load(std::memory_order_relaxed);
    }

    std::string deviceSummary() const override;

    // Whether the driver said it can report overloads at all (kAsioCanReportOverload, added to the
    // SDK in 2012). Printed at open, because "no dropouts" and "no counter" look identical in a log
    // and only one of them is good news.
    bool driverReportsOverloads() const
    {
        return mDriverReportsOverloads;
    }

    // --- the SDK's callbacks, which are C function pointers ---------------
    // PUBLIC ONLY BECAUSE OF THAT. They are called by the driver, on the driver's thread, and
    // nothing else may call them.
    void bufferSwitch(long doubleBufferIndex);
    long message(long selector, long value);
    void sampleRateChanged(double rate);

private:
    bool openDriver(std::string &error); // loads, inits and configures; no buffers yet
    void closeDriver();
    void drainParameterRing(); // RT thread
    void readMidi();           // RT thread
    void publishFeedback();    // RT thread

    AsioSettings mSettings;
    void *mSystemWindow = nullptr;
    std::string mOpenedDriver;

    bool mOpen = false;
    bool mDriverLoaded = false;
    bool mBuffersCreated = false;
    bool mStarted = false;
    bool mComInitialised = false;
    bool mUseOutputReady = false;
    bool mDriverReportsOverloads = false;

    // The driver's own buffers, as ASIOCreateBuffers filled them in: one input and two outputs,
    // each double-buffered. Fixed at three channels because the amp is mono in, stereo out — R3,
    // and it is structural rather than a limit anyone chose here.
    static constexpr int kChannels = 3;
    struct ChannelBuffers {
        void *buffers[2] = {nullptr, nullptr};
        int format = 0;
    };
    ChannelBuffers mIn;
    ChannelBuffers mOut[2];

    // Our own float buffers, allocated once in open(). The driver's are in its own format, so
    // unlike the JACK backend these cannot be aimed at directly.
    std::vector<float> mInputFloat;
    std::vector<float> mOutputFloat[2];

    // SPSC ring: written only by the UI thread, read only by the audio thread.
    static constexpr uint32_t kRingSize = 512; // power of two
    struct Change {
        Steinberg::Vst::ParamID id;
        Steinberg::Vst::ParamValue value;
    };
    Change mRing[kRingSize] = {};
    std::atomic<uint32_t> mRingWrite{0};
    std::atomic<uint32_t> mRingRead{0};

    // One slot per distinct parameter id the plug-in has published. Identical to the JACK
    // backend's, deliberately: the editor above reads both through the same interface, and two
    // different coalescing rules would be two different sets of meter behaviour.
    struct FeedbackSlot {
        std::atomic<Steinberg::Vst::ParamID> id{0};
        std::atomic<double> value{0.0};
        std::atomic<uint32_t> seq{0};
    };
    FeedbackSlot mFeedback[kFeedbackSlots];
    std::atomic<int> mFeedbackCount{0};

    std::atomic<int> mNewBlockSize{0};
    std::atomic<bool> mSuspended{false};
    std::atomic<uint32_t> mCycle{0};
    std::atomic<bool> mDeviceReset{false};

    // Two counters, summed by dropouts(). mOverloads is what the DRIVER reported — kAsioOverload,
    // or kAsioResyncRequest, which is how most drivers actually say they lost data. mLateBlocks is
    // what this process measured about ITSELF: a callback that took longer than the block's own
    // period missed the deadline, whatever the driver chose to say about it.
    //
    // BOTH, BECAUSE EITHER ALONE IS A GATE THAT CAN ONLY PASS. A driver that does not implement
    // overload reporting leaves the first at zero forever; and a driver that reports an overload
    // caused by some other application's audio would leave the second at zero while the first
    // climbed. The live gate wants "did the amp fit in the buffer", and the second is the measure
    // of exactly that.
    std::atomic<uint32_t> mOverloads{0};
    std::atomic<uint32_t> mLateBlocks{0};
    // QueryPerformanceFrequency, read once at open. The period a block must fit inside, in counter
    // ticks, computed with it.
    int64_t mCounterFrequency = 0;
    int64_t mBlockPeriodTicks = 0;

    NAMp::host::ChainEngine *mChain = nullptr;
    uint32_t mPluginLatency = 0;

    WinMmMidiIn mMidi;

    Steinberg::Vst::IAudioProcessor *mProcessor = nullptr;
    Steinberg::Vst::IComponent *mComponent = nullptr;
    const MidiRoute *mRoute = nullptr;

    // This cycle's MIDI kept undecoded for the rack, alongside the decode readMidi() does for the
    // amp — the same two shapes, and for the same reason, as the JACK backend's.
    NAMp::host::RtMidiEvent mRackMidi[NAMp::host::kMaxChunkMidi];
    int32_t mRackMidiCount = 0;

    double mSampleRate = 48000.0;
    std::atomic<int> mBlockSize{1024};
    // What the DRIVER is running at, which is fixed for the session. mBlockSize is what the
    // PROCESSOR is set up for, and the two differ for the blocks between a size change and the run
    // loop acting on it — which is why the chunk loop exists here as well.
    int mDriverBlockSize = 0;

    Steinberg::Vst::HostProcessData mProcessData;
    Steinberg::Vst::ParameterChanges mInputChanges;
    Steinberg::Vst::ParameterChanges mOutputChanges;
    Steinberg::Vst::EventList mEvents;
};

} // namespace Rations
