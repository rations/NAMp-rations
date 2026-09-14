// WinAudio — which of the two Windows backends is actually driving the device.
//
// ASIO IS PRIMARY AND WASAPI IS THE FALLBACK, and this is the object that makes that a run-time
// fact rather than a build-time one. The reason is the product: ASIO is what Windows guitar players
// run, because it is what the interface vendors ship and what the latency depends on. But a machine
// may have no vendor driver at all — a laptop with built-in audio, a USB interface running on the
// class driver — and on that machine a standalone amp that could only reach ASIO would be an amp
// that makes no sound. So both are built, always, and the choice is made when the device is opened.
//
// THE ORDER, and what counts as a failure worth falling back from:
//
//   1. ASIO, if this build has it and the machine has at least one driver installed. Enumerating
//   the
//      drivers loads none of them — it is a registry read — so asking is free and an unplugged
//      interface still lists and simply fails to open.
//   2. WASAPI otherwise, and also when ASIO was there and would not open. A driver that is
//   installed
//      but whose hardware is absent is the ordinary case here, not an exceptional one.
//
// A user who wants one or the other outright says so, and that is what Preference is for: a machine
// with a vendor driver whose owner would rather share the device with everything else can ask for
// WASAPI and get it, and the fallback then does not fire.
//
// WHY DELEGATION RATHER THAN A COMMON BASE DOING THE WORK. Both backends already implement
// AudioBackend in full, and they are not variations on one shape — ASIO is one driver with one
// callback on one clock, while WASAPI is two endpoints with two clocks and a ring between them (see
// R16's design in the project guidance, and wasapibackend.h). Trying to share an implementation
// between them was rejected for exactly that reason. What they DO share is the interface the
// standalone talks to, so this class is that interface and a pointer to whichever one is live.
//
// EVERYTHING BELOW AudioBackend IS UNCHANGED BY THIS. The standalone holds an AudioBackend
// reference and cannot tell which backend is behind it; that is the same seam the Linux build uses
// to hold a JACK client. See nativeaudio.h.

#pragma once

#include "audiobackend.h"
#include "wasapibackend.h"

#if NAMPRACK_HAVE_ASIO
#include "asiobackend.h"
#endif

#include <string>

namespace Rations
{

//------------------------------------------------------------------------
class WinAudio final : public AudioBackend
{
public:
    enum class Preference {
        // ASIO if there is one, WASAPI otherwise. What a fresh install uses.
        Auto,
        // ASIO only: fail rather than quietly opening something with a different latency. For a
        // user
        // who has chosen their interface and wants to know when it is not there.
        Asio,
        // WASAPI only, for sharing the device with the rest of the machine.
        Wasapi,
    };

    void setPreference(Preference preference)
    {
        mPreference = preference;
    }
    Preference preference() const
    {
        return mPreference;
    }

    // Both are set before open(), from the picker or from whatever was saved. Which one is
    // consulted depends on which backend ends up opening.
#if NAMPRACK_HAVE_ASIO
    void configureAsio(const AsioSettings &settings)
    {
        mAsio.configure(settings);
    }
    const AsioSettings &settings() const
    {
        return mAsio.settings();
    }
#endif
    void configureWasapi(const WasapiSettings &settings)
    {
        mWasapi.configure(settings);
    }
    const WasapiSettings &wasapiSettings() const
    {
        return mWasapi.settings();
    }

    // Which of the two is driving the device, for a picker that has to mark the row that is open.
    // Both are false when nothing is open, which is an ordinary state.
    bool usingWasapi() const
    {
        return mActive == &mWasapi;
    }
    bool usingAsio() const
    {
#if NAMPRACK_HAVE_ASIO
        return mActive == &mAsio;
#else
        return false;
#endif
    }

    // The top-level window, which ASIO takes as its sysRef — drivers use it as the parent for their
    // own control panel and for the message boxes some of them put up. WASAPI has no equivalent and
    // ignores it.
    void setSystemWindow(void *window);

    // What the device is already running at, learned WITHOUT opening it, so that setupProcessing
    // can be told the truth before the component is activated. Asks whichever backend would open.
    //
    // False means there is no device to be had at all, and the standalone continues with the editor
    // only — the same answer its Linux counterpart gives when there is no JACK server.
    bool probeDefaults(double &sampleRate, int &blockSize);

    // Which backend is driving the device, for the log and for the setup page. Empty until open().
    const char *activeName() const;

    // --- AudioBackend -----------------------------------------------------
    bool open(const char *clientName, Steinberg::Vst::IAudioProcessor *processor,
              Steinberg::Vst::IComponent *component, const MidiRoute *route) override;
    void close() override;

    void setChainEngine(NAMp::host::ChainEngine *engine) override;
    void notifyLatencyChanged() override;

    bool isOpen() const override;
    double sampleRate() const override;
    int blockSize() const override;

    int takeBufferSizeChange() override;
    bool suspendProcessing() override;
    void resumeProcessing(int blockSize) override;
    bool takeDeviceReset() override;

    bool pushParameter(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) override;
    bool readFeedback(int index, Steinberg::Vst::ParamID &id, double &value,
                      uint32_t &seq) const override;

    uint32_t dropouts() const override;
    std::string deviceSummary() const override;

private:
    // True when ASIO is built in, the preference allows it, and the machine has a driver to try.
    bool asioWorthTrying() const;

    Preference mPreference = Preference::Auto;
#if NAMPRACK_HAVE_ASIO
    AsioBackend mAsio;
#endif
    WasapiBackend mWasapi;

    // Whichever of the above is driving the device; null when none is. Every AudioBackend method
    // below reads this, and the ones with something sensible to say before a device exists say it
    // rather than delegating to a backend that is not the one that will open.
    AudioBackend *mActive = nullptr;

    // What probeDefaults() last reported, so that the standalone's own accessors answer the same
    // numbers setupProcessing was given, in the window before a device is open.
    double mProbedRate = 48000.0;
    int mProbedBlock = 1024;
};

} // namespace Rations
