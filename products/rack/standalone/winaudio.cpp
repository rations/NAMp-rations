// WinAudio implementation. See winaudio.h for the order the two backends are tried in and why.

#include "winaudio.h"

#include <cstdio>

namespace Rations
{

//------------------------------------------------------------------------
void WinAudio::setSystemWindow(void *window)
{
#if NAMPRACK_HAVE_ASIO
    mAsio.setSystemWindow(window);
#else
    (void)window;
#endif
}

//------------------------------------------------------------------------
bool WinAudio::asioWorthTrying() const
{
#if NAMPRACK_HAVE_ASIO
    if (mPreference == Preference::Wasapi)
        return false;
    // A registry enumeration, not a driver load: asking is free, and a driver whose hardware is
    // absent still lists here and simply fails to open. Which is why this only decides whether ASIO
    // is worth TRYING.
    return AsioBackend::driverCount() > 0;
#else
    return false;
#endif
}

//------------------------------------------------------------------------
bool WinAudio::probeDefaults(double &sampleRate, int &blockSize)
{
#if NAMPRACK_HAVE_ASIO
    if (asioWorthTrying()) {
        // Whichever driver the settings name, or the first one on the machine — the same choice
        // open() will make, so the numbers describe the device that will actually be opened.
        const AsioBackend::DriverCapabilities caps =
            AsioBackend::probe(mAsio.settings().driverName, nullptr);
        if (caps.ok && caps.currentRate > 0.0 && caps.preferredBlock > 0) {
            // ASIO states both outright, so unlike WASAPI's these are not estimates. The requested
            // block size wins if there is one, because that is what open() will ask the driver for.
            mProbedRate = caps.currentRate;
            mProbedBlock =
                mAsio.settings().blockSize > 0 ? mAsio.settings().blockSize : caps.preferredBlock;
            sampleRate = mProbedRate;
            blockSize = mProbedBlock;
            return true;
        }
        // Not a failure yet: WASAPI is asked next, exactly as it would be at open().
        if (!caps.error.empty() && mPreference == Preference::Asio)
            std::fprintf(stderr, "namp-rack: the ASIO driver could not be asked: %s\n",
                         caps.error.c_str());
    }
    if (mPreference == Preference::Asio) {
        std::fprintf(stderr, "namp-rack: ASIO was asked for and no driver answered\n");
        return false;
    }
#endif

    if (mWasapi.probeDefaults(sampleRate, blockSize)) {
        mProbedRate = sampleRate;
        mProbedBlock = blockSize;
        return true;
    }
    return false;
}

//------------------------------------------------------------------------
const char *WinAudio::activeName() const
{
#if NAMPRACK_HAVE_ASIO
    if (mActive == &mAsio)
        return "ASIO";
#endif
    if (mActive == &mWasapi)
        return "WASAPI";
    return "";
}

//------------------------------------------------------------------------
bool WinAudio::open(const char *clientName, Steinberg::Vst::IAudioProcessor *processor,
                    Steinberg::Vst::IComponent *component, const MidiRoute *route)
{
    if (mActive)
        return true;

#if NAMPRACK_HAVE_ASIO
    if (asioWorthTrying()) {
        if (mAsio.open(clientName, processor, component, route)) {
            mActive = &mAsio;
            std::printf("namp-rack: ASIO, driver '%s', %.0f Hz, %d frames\n",
                        mAsio.openedDriver().c_str(), mAsio.sampleRate(), mAsio.blockSize());
            std::fflush(stdout);
            return true;
        }
        if (mPreference == Preference::Asio) {
            // Asked for outright, so a failure is reported rather than worked around: the user
            // chose this driver and needs to know it did not open, not to be given a quieter device
            // with a different latency and left to notice.
            std::fprintf(stderr, "namp-rack: ASIO was asked for and would not open\n");
            return false;
        }
        std::fprintf(stderr, "namp-rack: no ASIO device opened; falling back to WASAPI\n");
    } else if (mPreference == Preference::Asio) {
        std::fprintf(stderr, "namp-rack: ASIO was asked for and this machine has no driver\n");
        return false;
    }
#endif

    if (mWasapi.open(clientName, processor, component, route)) {
        mActive = &mWasapi;
        std::printf(
            "namp-rack: WASAPI %s, in '%s', out '%s', %.0f Hz, %d frames%s\n",
            mWasapi.isExclusive() ? "exclusive" : "shared", mWasapi.openedCaptureName().c_str(),
            mWasapi.openedRenderName().c_str(), mWasapi.sampleRate(), mWasapi.blockSize(),
            mWasapi.twoClocks() ? " (two clocks: input and output are different devices)" : "");
        std::fflush(stdout);
        return true;
    }
    return false;
}

//------------------------------------------------------------------------
void WinAudio::close()
{
    if (!mActive)
        return;
    mActive->close();
    mActive = nullptr;
}

//------------------------------------------------------------------------
// Forwarded to BOTH, not only to the live one, because this is called before the device is opened
// and again with null after it is closed. A backend that was handed the engine only if it happened
// to be the one open would run the transparent path — which is exactly what an empty rack looks
// like, so it would not look like a bug.
void WinAudio::setChainEngine(NAMp::host::ChainEngine *engine)
{
#if NAMPRACK_HAVE_ASIO
    mAsio.setChainEngine(engine);
#endif
    mWasapi.setChainEngine(engine);
}

//------------------------------------------------------------------------
void WinAudio::notifyLatencyChanged()
{
    if (mActive)
        mActive->notifyLatencyChanged();
}

//------------------------------------------------------------------------
bool WinAudio::isOpen() const
{
    return mActive && mActive->isOpen();
}

//------------------------------------------------------------------------
// The probed figures until a device is open, so that the standalone's accessors agree with what
// setupProcessing was given rather than reporting zero.
double WinAudio::sampleRate() const
{
    return mActive ? mActive->sampleRate() : mProbedRate;
}

int WinAudio::blockSize() const
{
    return mActive ? mActive->blockSize() : mProbedBlock;
}

//------------------------------------------------------------------------
int WinAudio::takeBufferSizeChange()
{
    return mActive ? mActive->takeBufferSizeChange() : 0;
}

bool WinAudio::suspendProcessing()
{
    return mActive ? mActive->suspendProcessing() : false;
}

void WinAudio::resumeProcessing(int blockSize)
{
    if (mActive)
        mActive->resumeProcessing(blockSize);
}

bool WinAudio::takeDeviceReset()
{
    return mActive ? mActive->takeDeviceReset() : false;
}

//------------------------------------------------------------------------
bool WinAudio::pushParameter(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value)
{
    return mActive ? mActive->pushParameter(id, value) : false;
}

bool WinAudio::readFeedback(int index, Steinberg::Vst::ParamID &id, double &value,
                            uint32_t &seq) const
{
    return mActive ? mActive->readFeedback(index, id, value, seq) : false;
}

uint32_t WinAudio::dropouts() const
{
    return mActive ? mActive->dropouts() : 0;
}

// Whichever is live describes itself; there is nothing useful this class could add that the backend
// has not already said, and the backend's own name is the first word of it.
std::string WinAudio::deviceSummary() const
{
    return mActive ? mActive->deviceSummary() : std::string();
}

} // namespace Rations
