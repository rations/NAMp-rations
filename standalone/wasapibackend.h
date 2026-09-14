// WasapiBackend — the WASAPI implementation of AudioBackend, and the Windows FALLBACK.
//
// What this class is FOR, and the three things that cross the RT boundary, are described once in
// audiobackend.h. What is ASIO-specific is in asiobackend.h. What follows is only what is WASAPI's,
// and there is more of it than there was for ASIO, because WASAPI is the one audio API in this
// project that is not shaped like the amp.
//
// IT IS A FALLBACK AND NOT A SECOND-CLASS ONE. This is what a machine with no vendor ASIO driver
// gets, which is most laptops, and it has to be good enough to play through. It is not, however,
// what a Windows guitar player with an interface should be using: ASIO is, and the picker says so.
//
// WASAPI HAS NO DUPLEX STREAM, WHICH IS THE WHOLE PROBLEM. Capture and render are separate
// IAudioClient instances with separate buffers and separate events, and an amp needs both in one
// pass. The resolution — render is the master, capture crosses into it through a pre-allocated ring
// with a one-block target, drift is zero when both endpoints are on one device because they share a
// clock, and a two-device setup's drift is corrected at block granularity and COUNTED rather than
// concealed — is written out in this project's decision list and is not repeated here. What matters
// at this file's level: the audio thread's shape is the ASIO backend's, and the ring is what makes
// that possible.
//
// EXCLUSIVE FIRST, SHARED AS THE FALLBACK WITHIN THE FALLBACK. Exclusive mode gives the device's
// own format and its real period, which is the point of asking for it; it also fails routinely,
// because another application holds the device or because the period is not one the device accepts.
// AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED in particular is not an error but a documented negotiation —
// re-ask for the size, throw the client away, and initialise a new one — and it is implemented
// rather than reported. Shared mode always works and coexists with everything else on the machine;
// where IAudioClient3 is available its engine period is asked for, which is what makes shared mode
// tolerable rather than merely possible.
//
// THE DEVICE FORMAT IS NOT FLOAT in exclusive mode: it is whatever the hardware uses, commonly 24
// bits inside a 32-bit container, and the channels are INTERLEAVED where ASIO's are not. Both are
// handled by pcmsamples.h, shared with the ASIO backend and proved offline by
// tools/namp_audiocheck — including the interleaving, which is the difference between the two APIs
// and is one channel played at half speed if it is got wrong.
//
// COM LIVES ON THE AUDIO THREAD, and that is a deliberate departure from the arrangement the
// platform's own samples use. Those initialise COM and activate the device on the application's
// main thread, which is correct only as long as that thread is in the multi-threaded apartment —
// and this standalone's run loop cannot promise that, because the ASIO backend requires the SAME
// thread to be in a single-threaded one, and a hosted plug-in's editor may have initialised it
// first. So the audio thread — a thread this class owns outright and nothing else can have touched
// — enters the apartment, activates the devices, negotiates the format, and releases all of it
// before it exits. open() waits once for that to finish, which is the only wait anywhere in this
// file.

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
#include <thread>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
// What the setup page's picker chose. The two device ids are WASAPI endpoint id strings — opaque,
// stable across reboots, and what persists in the settings; the friendly name is for display only
// and changes when a user renames a device in the control panel.
struct WasapiSettings {
    // Empty means the system default endpoint, which is what a fresh install uses and what most
    // machines should stay on.
    std::string captureDeviceId;
    std::string renderDeviceId;

    // Exclusive mode is asked for first. Turning it off is a deliberate choice to share the device
    // with everything else on the machine, at the cost of the mixer's own latency.
    bool exclusive = true;

    // 0 asks the device for its own period, which is the right default in both modes.
    int blockSize = 0;
    // 0 keeps the endpoint's current rate. In SHARED mode that is not a preference but a
    // requirement — the mixer owns the rate and a client must use the mix format — so a request is
    // honoured in exclusive mode and ignored, with a word on stderr, in shared.
    double sampleRate = 0.0;

    // Which channel of the capture endpoint the guitar is in. A laptop's built-in input is mono or
    // fakes stereo; an interface's is whichever socket the lead is in.
    int inputChannel = 0;

    // The MIDI input for the footswitch, or WinMmMidiIn::kNoDevice.
    int midiDevice = WinMmMidiIn::kNoDevice;
};

//------------------------------------------------------------------------
class WasapiBackend final : public AudioBackend
{
public:
    ~WasapiBackend() override;

    // --- what the picker needs (main thread, before open) -----------------
    struct DeviceEntry {
        std::string id;   // the endpoint id, which is what gets saved
        std::string name; // the friendly name, which is what gets shown
        bool isDefault = false;
    };
    // The active endpoints of one direction. Enumeration opens no stream and takes no device, so it
    // is safe to call while something else is playing — and it lists only ACTIVE endpoints, because
    // an unplugged or disabled one is not something to offer.
    static bool enumerate(bool capture, std::vector<DeviceEntry> &out);

    // What a stream on the configured render endpoint would run at, learned WITHOUT opening one:
    // GetMixFormat and GetDevicePeriod both answer from an activated but UNINITIALISED
    // IAudioClient, so nothing is taken from whatever is currently playing.
    //
    // THE BLOCK SIZE IS AN ESTIMATE AND NOT A GUARANTEE, unlike ASIO's, where the driver states its
    // preferred size outright. The period this reports is the device's default one, and the size
    // the stream actually ends up at can still differ: exclusive mode clamps up to the device
    // minimum, shared mode on IAudioClient3 clamps into the engine's own range, and an unaligned
    // buffer is renegotiated. The standalone reconciles the difference against the real figure the
    // moment the device is open — see the note there on why a wrong estimate degrades rather than
    // breaks.
    //
    // RENDER, NOT CAPTURE, because render is the master clock; see the two-clocks note at the top
    // of this file.
    bool probeDefaults(double &sampleRate, int &blockSize);

    void configure(const WasapiSettings &settings)
    {
        mSettings = settings;
    }
    const WasapiSettings &settings() const
    {
        return mSettings;
    }

    // What the device actually gave us once open, for the picker to show and for the log. Reported
    // rather than assumed: exclusive mode may have been refused, and the period may have been
    // renegotiated.
    bool isExclusive() const
    {
        return mOpenedExclusive;
    }
    const std::string &openedRenderName() const
    {
        return mRenderName;
    }
    const std::string &openedCaptureName() const
    {
        return mCaptureName;
    }
    // True when capture and render are different physical devices, which is the only case in which
    // the two clocks drift. The picker warns about it, and driftCorrections() is what it costs.
    bool twoClocks() const
    {
        return mTwoClocks;
    }

    // --- AudioBackend -----------------------------------------------------
    bool open(const char *clientName, Steinberg::Vst::IAudioProcessor *processor,
              Steinberg::Vst::IComponent *component, const MidiRoute *route) override;
    void close() override;

    void setChainEngine(NAMp::host::ChainEngine *engine) override
    {
        mChain = engine;
    }
    // Nothing downstream of the device to tell, as with ASIO. The figure is printed at open().
    void notifyLatencyChanged() override
    {
    }

    bool isOpen() const override
    {
        return mThread.joinable() && mRunning.load(std::memory_order_acquire);
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

    // Every way this backend can fail to deliver a block on time, summed. See the counters
    // themselves for what each one means; the reason they are summed here rather than reported
    // separately is that the live gate asks one question — did the audio come out whole — and each
    // of them is a no.
    uint32_t dropouts() const override
    {
        return mCaptureGlitches.load(std::memory_order_relaxed) +
               mUnderruns.load(std::memory_order_relaxed) +
               mDriftCorrections.load(std::memory_order_relaxed);
    }

    // The three, separately, because they have different causes and different fixes: a capture
    // glitch is the system losing input, an underrun is this process being too slow, and a drift
    // correction is the cost of running two clocks. Shown in the setup page rather than only
    std::string deviceSummary() const override;

    // summed.
    uint32_t captureGlitches() const
    {
        return mCaptureGlitches.load(std::memory_order_relaxed);
    }
    uint32_t underruns() const
    {
        return mUnderruns.load(std::memory_order_relaxed);
    }
    uint32_t driftCorrections() const
    {
        return mDriftCorrections.load(std::memory_order_relaxed);
    }

private:
    // The audio thread, start to finish: apartment, devices, format, the loop, and teardown.
    void audioThread();
    // Called from it, before the loop. Everything that can fail, with a reason.
    bool setUpDevices(std::string &error);
    void releaseDevices();
    // One wake-up's worth of work.
    void runBlock();
    // Capture into the ring, and the drift correction that keeps its fill near one block.
    void drainCapture();
    int takeFromRing(float *dst, int frames); // returns how many were real rather than padding

    void drainParameterRing(); // RT thread
    void readMidi();           // RT thread
    void publishFeedback();    // RT thread

    WasapiSettings mSettings;

    // --- the COM objects, created and released on the audio thread ---------
    // Held as void* so this header pulls in no windows.h and nothing that includes it inherits one.
    // Every use casts in the implementation, where the types are in scope.
    void *mEnumerator = nullptr;    // IMMDeviceEnumerator
    void *mCaptureDevice = nullptr; // IMMDevice
    void *mRenderDevice = nullptr;
    void *mCaptureClient = nullptr; // IAudioClient
    void *mRenderClient = nullptr;
    void *mCapture = nullptr; // IAudioCaptureClient
    void *mRender = nullptr;  // IAudioRenderClient
    void *mRenderEvent = nullptr;
    void *mCaptureEvent = nullptr;
    void *mMmcssHandle = nullptr; // AvSetMmThreadCharacteristics
    bool mComInitialised = false;

    std::string mCaptureName;
    std::string mRenderName;
    bool mOpenedExclusive = false;
    bool mTwoClocks = false;

    // The two streams' formats, as pcmsamples.h understands them.
    int mCaptureFormat = 0;
    int mCaptureChannels = 0;
    int mRenderFormat = 0;
    int mRenderChannels = 0;
    int mRenderFrames = 0; // the render endpoint's whole buffer, which bounds a block

    // --- the handshake with open() -----------------------------------------
    // open() waits for mSetupDone, reads mSetupOk, allocates the device-sized buffers, and then
    // releases the thread with mGoAhead. TWO STEPS RATHER THAN ONE, so that every allocation
    // happens on the main thread: the sizes are not known until the format is negotiated, and
    // negotiating is the audio thread's job because the apartment is.
    void *mSetupDone = nullptr;
    void *mGoAhead = nullptr;
    std::atomic<bool> mSetupOk{false};
    std::string mSetupError; // written before mSetupDone is signalled, read after
    std::atomic<bool> mStop{false};
    std::atomic<bool> mRunning{false};
    std::thread mThread;

    // --- the capture ring --------------------------------------------------
    // Mono, because the amp is: one channel is taken from the interleaved capture stream and the
    // rest are ignored. Allocated once by open() to kRingBlocks blocks and never resized.
    //
    // THE TARGET FILL IS ONE BLOCK. Below it the missing frames are silence and an underrun is
    // counted; above the high-water mark the oldest excess is dropped and a drift correction is
    // counted. With both endpoints on one device neither ever fires, because one device has one
    // clock — see the decision list.
    static constexpr int kRingBlocks = 4;
    // Whether the ring has ever held a whole block. UNTIL IT HAS, A SHORT READ IS NOT AN UNDERRUN:
    // capture and render start within a few milliseconds of each other but not simultaneously, so
    // the first wake-up or two genuinely have nothing to play yet. Counting those would put a floor
    // under dropouts() that no machine could get below, and the live gate asks for zero — a gate
    // that can never pass proves as little as one that can never fail. Before it is set the output
    // is silence rather than invented silence fed through the amp.
    bool mPrimed = false;
    std::vector<float> mCaptureRing;
    int mRingCapacity = 0;
    int mRingWritePos = 0;
    int mRingReadPos = 0;
    int mRingFill = 0; // frames available; only the audio thread touches any of these

    // Our own float buffers, allocated by open() once the block size is known.
    std::vector<float> mInputFloat;
    std::vector<float> mOutputFloat[2];

    // SPSC ring: written only by the UI thread, read only by the audio thread.
    static constexpr uint32_t kParamRingSize = 512; // power of two
    struct Change {
        Steinberg::Vst::ParamID id;
        Steinberg::Vst::ParamValue value;
    };
    Change mParamRing[kParamRingSize] = {};
    std::atomic<uint32_t> mParamWrite{0};
    std::atomic<uint32_t> mParamRead{0};

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

    std::atomic<uint32_t> mCaptureGlitches{0};
    std::atomic<uint32_t> mUnderruns{0};
    std::atomic<uint32_t> mDriftCorrections{0};

    NAMp::host::ChainEngine *mChain = nullptr;
    uint32_t mPluginLatency = 0;

    WinMmMidiIn mMidi;

    Steinberg::Vst::IAudioProcessor *mProcessor = nullptr;
    Steinberg::Vst::IComponent *mComponent = nullptr;
    const MidiRoute *mRoute = nullptr;

    NAMp::host::RtMidiEvent mRackMidi[NAMp::host::kMaxChunkMidi];
    int32_t mRackMidiCount = 0;

    double mSampleRate = 48000.0;
    std::atomic<int> mBlockSize{1024};

    Steinberg::Vst::HostProcessData mProcessData;
    Steinberg::Vst::ParameterChanges mInputChanges;
    Steinberg::Vst::ParameterChanges mOutputChanges;
    Steinberg::Vst::EventList mEvents;
};

} // namespace Rations
