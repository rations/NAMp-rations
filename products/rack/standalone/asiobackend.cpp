// AsioBackend implementation. See asiobackend.h for what is ASIO-specific and audiobackend.h for
// the contract it implements.

#include "asiobackend.h"
#include "pcmsamples.h"

#include "host/hostapp.h"

#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstevents.h"

#include <windows.h>

// The ASIO SDK's host side, from the dependency sysroot and from nowhere else — it is licensed to
// be used, never redistributed, so no copy of it exists in this repository. See NOTICE.
// WINDOWS IS UNDEFINED FIRST, AND IT IS NOT OPTIONAL. Both SDKs define a bare macro of that name:
// the VST3 SDK's fplatform.h makes it SMTG_OS_WINDOWS, and ASIO's ginclude.h defines it to 1. The
// values agree, but the tokens do not, so whichever is included second warns — and this project
// fails a build on a warning in code it owns. Undefined here rather than reordered, because every
// VST3 header that wanted it has already been included by this line and nothing below uses it.
#undef WINDOWS
//
// asiosys.h COMES FIRST, AND THE ORDER IS AN ABI DECISION RATHER THAN A STYLE ONE. asio.h includes
// nothing at all, and three of its own types are conditional on macros only asiosys.h (or the host
// tree's ginclude.h) defines: with IEEE754_64FLOAT and NATIVE_INT64 undefined, ASIOSampleRate is an
// eight-BYTE STRUCT instead of a double and ASIOSamples and ASIOTimeStamp become hi/lo pairs. Half
// the API's signatures change shape, silently, and the SDK's own host sample includes them in this
// order for exactly that reason.
#include "asiosdk/common/asiosys.h"
#include "asiosdk/common/asio.h"
#include "asiosdk/host/asiodrivers.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace Steinberg;

// Declared by the SDK's host helper (host/asiodrivers.cpp) and not by any header it ships.
extern bool loadAsioDriver(char *name);
extern AsioDrivers *asioDrivers;

namespace Rations
{

namespace
{

// The queue and event sizes, and the reasoning behind each, are the JACK backend's; they are the
// same numbers because they answer the same question about the same plug-in.
constexpr int32 kMaxInputParameters = 64;
constexpr int32 kMaxOutputParameters = 8;
constexpr int32 kMaxEvents = 256;

constexpr int kMidiStatusMask = 0xf0;
constexpr int kMidiChannelMask = 0x0f;
constexpr int kMidiNoteOff = 0x80;
constexpr int kMidiNoteOn = 0x90;
constexpr int kMidiControlChange = 0xb0;
constexpr int kMidiProgramChange = 0xc0;

// A driver's name is a char[32] throughout the SDK — ASIODriverInfo::name, asioGetDriverName's
// bound, and the registry value they come from. Named once rather than repeated as a literal.
constexpr int kAsioNameLength = 32;

// THE ONE INSTANCE. The SDK keeps the loaded driver in a global of its own and its callbacks are
// plain C function pointers with no user data, so the callbacks reach the backend through this. Set
// by open() before ASIOCreateBuffers installs the callbacks and cleared by close() after
// ASIODisposeBuffers has removed them, which is the window in which the driver may call.
AsioBackend *gBackend = nullptr;

//------------------------------------------------------------------------
// The SDK's enum values, asserted against the copies in pcmsamples.h. Cast on OUR side, because
// the two are distinct enumeration types and comparing them bare is a warning in itself — what is
// being read is still the SDK's value.
//
// That header deliberately includes nothing of ASIO's, so that the conversion it holds can be
// compiled and proved on a machine with no SDK and no driver. These asserts are what keeps the two
// from drifting: a renumbering upstream fails the build here rather than silently converting one
// format as another.
static_assert(static_cast<int>(kPcmInt16LSB) == ASIOSTInt16LSB,
              "pcmsamples.h disagrees with the SDK");
static_assert(static_cast<int>(kPcmInt24LSB) == ASIOSTInt24LSB,
              "pcmsamples.h disagrees with the SDK");
static_assert(static_cast<int>(kPcmInt32LSB) == ASIOSTInt32LSB,
              "pcmsamples.h disagrees with the SDK");
static_assert(static_cast<int>(kPcmFloat32LSB) == ASIOSTFloat32LSB,
              "pcmsamples.h disagrees with the SDK");
static_assert(static_cast<int>(kPcmFloat64LSB) == ASIOSTFloat64LSB,
              "pcmsamples.h disagrees with the SDK");
static_assert(static_cast<int>(kPcmInt32LSB16) == ASIOSTInt32LSB16,
              "pcmsamples.h disagrees with the SDK");
static_assert(static_cast<int>(kPcmInt32LSB18) == ASIOSTInt32LSB18,
              "pcmsamples.h disagrees with the SDK");
static_assert(static_cast<int>(kPcmInt32LSB20) == ASIOSTInt32LSB20,
              "pcmsamples.h disagrees with the SDK");
static_assert(static_cast<int>(kPcmInt32LSB24) == ASIOSTInt32LSB24,
              "pcmsamples.h disagrees with the SDK");

//------------------------------------------------------------------------
// The SDK's callbacks. Each is a thin forward to the one backend; none may do anything else,
// because bufferSwitch in particular may be called at interrupt time.
void asioBufferSwitch(long index, ASIOBool)
{
    if (gBackend)
        gBackend->bufferSwitch(index);
}

ASIOTime *asioBufferSwitchTimeInfo(ASIOTime *params, long index, ASIOBool)
{
    // Never asked for — this host answers 0 to kAsioSupportsTimeInfo, because it needs no
    // timestamps and the plain callback is one fewer thing to be wrong about. Installed anyway
    // because a driver is entitled to call whichever it was told about, and a null pointer here
    // would be a crash inside somebody else's code.
    if (gBackend)
        gBackend->bufferSwitch(index);
    return params;
}

void asioSampleRateDidChange(ASIOSampleRate rate)
{
    if (gBackend)
        gBackend->sampleRateChanged(static_cast<double>(rate));
}

long asioMessage(long selector, long value, void *, double *)
{
    return gBackend ? gBackend->message(selector, value) : 0;
}

ASIOCallbacks gCallbacks = {&asioBufferSwitch, &asioSampleRateDidChange, &asioMessage,
                            &asioBufferSwitchTimeInfo};

//------------------------------------------------------------------------
// The rates a guitar amp is ever asked for, in the order the picker should offer them. Not every
// rate a driver can do — a device slaved to an external clock accepts only the one it is locked to,
// and the point of the list is to find out which of these it will take.
const double kCandidateRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};

//------------------------------------------------------------------------
// A size the driver will actually accept, from what the picker asked for.
//
// The granularity rule is the SDK's: a granularity of -1 means the driver accepts powers of two
// between min and max, and any other value means multiples of it from min. Getting this wrong is
// not a silent matter — ASIOCreateBuffers fails — but landing on the nearest legal size below the
// request is better than failing, because a user who asked for 128 on hardware that does 160 wants
// the smallest it has rather than an error.
long legalBlockSize(long requested, long minSize, long maxSize, long granularity, long preferred)
{
    if (requested <= 0)
        return preferred;
    long size = std::min(std::max(requested, minSize), maxSize);
    if (granularity == -1) {
        // Powers of two. Walk up from min rather than computing a log, so a driver whose min is not
        // itself a power of two still produces one of ITS sizes.
        long power = minSize;
        long best = minSize;
        while (power <= maxSize) {
            if (power <= size)
                best = power;
            if (power > (1 << 24)) // nothing sane is anywhere near this; do not loop forever
                break;
            power *= 2;
        }
        return best;
    }
    if (granularity > 1) {
        const long steps = (size - minSize) / granularity;
        size = minSize + steps * granularity;
    }
    return std::min(std::max(size, minSize), maxSize);
}

} // namespace

//------------------------------------------------------------------------
// Main thread.
int AsioBackend::driverCount()
{
    // asioGetNumDev() reads the registry. No driver binary is opened, which is what makes it safe
    // to call before the user has chosen anything.
    if (!asioDrivers) {
        // The SDK's list object is created lazily by loadAsioDriver(). Creating it here would
        // duplicate that, so instead ask through the same door: an empty name loads nothing and
        // leaves the list constructed.
        char none[kAsioNameLength] = "";
        loadAsioDriver(none);
    }
    return asioDrivers ? static_cast<int>(asioDrivers->asioGetNumDev()) : 0;
}

//------------------------------------------------------------------------
bool AsioBackend::driverName(int index, std::string &out)
{
    out.clear();
    const int count = driverCount();
    if (index < 0 || index >= count || !asioDrivers)
        return false;
    char name[kAsioNameLength] = {};
    if (asioDrivers->asioGetDriverName(index, name, kAsioNameLength) != 0)
        return false;
    // The SDK writes at most 32 bytes and does not promise a terminator on a full one.
    name[kAsioNameLength - 1] = '\0';
    out = name;
    return !out.empty();
}

//------------------------------------------------------------------------
AsioBackend::DriverCapabilities AsioBackend::probe(const std::string &driverName,
                                                   void *systemWindow)
{
    DriverCapabilities caps;

    // The driver is a COM object, so the apartment has to exist before it is created. Probing runs
    // on the run loop, which is also where open() runs, so the same rule applies here.
    const HRESULT com = CoInitialize(nullptr);
    const bool ownCom = SUCCEEDED(com);

    std::string wanted = driverName;
    if (wanted.empty() && !AsioBackend::driverName(0, wanted)) {
        caps.error = "no ASIO driver is installed";
        if (ownCom)
            CoUninitialize();
        return caps;
    }

    char name[kAsioNameLength] = {};
    std::snprintf(name, sizeof(name), "%s", wanted.c_str());
    if (!loadAsioDriver(name)) {
        caps.error = "the driver could not be loaded";
        if (ownCom)
            CoUninitialize();
        return caps;
    }

    ASIODriverInfo info = {};
    info.asioVersion = 2;
    info.sysRef = systemWindow;
    if (ASIOInit(&info) != ASE_OK) {
        // The driver's own message, which is the only useful thing to show: "the device is in use
        // by another application" is what most of them actually say, and it names the real problem.
        info.errorMessage[sizeof(info.errorMessage) - 1] = '\0';
        caps.error = info.errorMessage[0] ? info.errorMessage : "the driver refused to initialise";
        ASIOExit();
        if (ownCom)
            CoUninitialize();
        return caps;
    }

    long inputs = 0;
    long outputs = 0;
    if (ASIOGetChannels(&inputs, &outputs) == ASE_OK) {
        caps.inputChannels = static_cast<int>(inputs);
        caps.outputChannels = static_cast<int>(outputs);
    }

    long minSize = 0;
    long maxSize = 0;
    long preferred = 0;
    long granularity = 0;
    if (ASIOGetBufferSize(&minSize, &maxSize, &preferred, &granularity) == ASE_OK) {
        caps.minBlock = static_cast<int>(minSize);
        caps.maxBlock = static_cast<int>(maxSize);
        caps.preferredBlock = static_cast<int>(preferred);
        caps.blockGranularity = static_cast<int>(granularity);
    }

    ASIOSampleRate current = 0.0;
    if (ASIOGetSampleRate(&current) == ASE_OK)
        caps.currentRate = static_cast<double>(current);

    for (const double rate : kCandidateRates) {
        if (ASIOCanSampleRate(static_cast<ASIOSampleRate>(rate)) == ASE_OK)
            caps.rates.push_back(rate);
    }

    caps.ok = caps.outputChannels >= 2 && caps.inputChannels >= 1;
    if (!caps.ok && caps.error.empty()) {
        // A perfectly good driver for something else. Said plainly rather than as a failure to
        // initialise, which is what it would otherwise look like.
        caps.error =
            "the device does not have one input and two outputs, which is what an amp needs";
    }

    ASIOExit();
    if (ownCom)
        CoUninitialize();
    return caps;
}

//------------------------------------------------------------------------
// Main thread.
bool AsioBackend::readFeedback(int index, Vst::ParamID &id, double &value, uint32_t &seq) const
{
    if (index < 0 || index >= mFeedbackCount.load(std::memory_order_acquire))
        return false;
    const FeedbackSlot &slot = mFeedback[index];
    // seq first, then the value it belongs to: the audio thread writes them the other way round, so
    // a value read after its sequence number is at least as new as that number says.
    seq = slot.seq.load(std::memory_order_acquire);
    id = slot.id.load(std::memory_order_relaxed);
    value = slot.value.load(std::memory_order_relaxed);
    return true;
}

//------------------------------------------------------------------------
AsioBackend::~AsioBackend()
{
    close();
}

//------------------------------------------------------------------------
// Loads the driver, initialises it, and settles the rate, the block size and the channel formats.
// Everything up to but not including the buffers, so that a failure at any step can be reported
// with the driver still loaded and its own error message still available.
bool AsioBackend::openDriver(std::string &error)
{
    std::string wanted = mSettings.driverName;
    if (wanted.empty() && !driverName(0, wanted)) {
        error = "no ASIO driver is installed";
        return false;
    }

    char name[kAsioNameLength] = {};
    std::snprintf(name, sizeof(name), "%s", wanted.c_str());
    if (!loadAsioDriver(name)) {
        error = "the driver could not be loaded: " + wanted;
        return false;
    }
    mDriverLoaded = true;

    ASIODriverInfo info = {};
    info.asioVersion = 2;
    info.sysRef = mSystemWindow;
    if (ASIOInit(&info) != ASE_OK) {
        info.errorMessage[sizeof(info.errorMessage) - 1] = '\0';
        error = info.errorMessage[0] ? info.errorMessage : "the driver refused to initialise";
        return false;
    }
    info.name[sizeof(info.name) - 1] = '\0';
    mOpenedDriver = info.name[0] ? info.name : wanted;

    long inputs = 0;
    long outputs = 0;
    if (ASIOGetChannels(&inputs, &outputs) != ASE_OK) {
        error = "the driver would not report its channel count";
        return false;
    }
    if (inputs < 1 || outputs < 2) {
        error = "the device does not have one input and two outputs, which is what an amp needs";
        return false;
    }
    if (mSettings.inputChannel < 0 || mSettings.inputChannel >= inputs ||
        mSettings.outputChannelL < 0 || mSettings.outputChannelL >= outputs ||
        mSettings.outputChannelR < 0 || mSettings.outputChannelR >= outputs) {
        // The saved settings name channels this device does not have — a different interface is
        // plugged in than the one the settings were written for. Fall back rather than refuse: the
        // first input and the first two outputs are what the device would have been opened with on
        // a fresh install, and the user can change them once they can see the window.
        std::fprintf(stderr,
                     "namp-rack: %s has %ld in / %ld out, which does not include the saved "
                     "channels; using 1 and 1/2\n",
                     mOpenedDriver.c_str(), inputs, outputs);
        mSettings.inputChannel = 0;
        mSettings.outputChannelL = 0;
        mSettings.outputChannelR = 1;
    }

    // THE RATE IS ASKED FOR, NEVER ASSUMED. A device slaved to an external clock, or shared with
    // another application, may refuse — in which case what it is already running at is the right
    // answer and the amp simply runs there.
    if (mSettings.sampleRate > 0.0) {
        const ASIOSampleRate wantedRate = static_cast<ASIOSampleRate>(mSettings.sampleRate);
        if (ASIOCanSampleRate(wantedRate) != ASE_OK || ASIOSetSampleRate(wantedRate) != ASE_OK) {
            std::fprintf(stderr,
                         "namp-rack: %s will not run at %.0f Hz; using whatever rate it is at\n",
                         mOpenedDriver.c_str(), mSettings.sampleRate);
        }
    }
    ASIOSampleRate actual = 0.0;
    if (ASIOGetSampleRate(&actual) != ASE_OK || actual <= 0.0) {
        error = "the driver would not report its sample rate";
        return false;
    }
    mSampleRate = static_cast<double>(actual);

    long minSize = 0;
    long maxSize = 0;
    long preferred = 0;
    long granularity = 0;
    if (ASIOGetBufferSize(&minSize, &maxSize, &preferred, &granularity) != ASE_OK) {
        error = "the driver would not report its buffer sizes";
        return false;
    }
    mDriverBlockSize = static_cast<int>(
        legalBlockSize(mSettings.blockSize, minSize, maxSize, granularity, preferred));
    if (mDriverBlockSize <= 0) {
        error = "the driver reported no usable buffer size";
        return false;
    }

    // The formats of the three channels this backend will actually use, asked per channel because
    // ASIO permits them to differ — and refused by name if any is one pcmsamples.h cannot carry,
    // rather than played as noise.
    struct Wanted {
        long channel;
        ASIOBool isInput;
        int *format;
    };
    const Wanted wanted3[kChannels] = {
        {mSettings.inputChannel, ASIOTrue, &mIn.format},
        {mSettings.outputChannelL, ASIOFalse, &mOut[0].format},
        {mSettings.outputChannelR, ASIOFalse, &mOut[1].format},
    };
    for (const Wanted &w : wanted3) {
        ASIOChannelInfo channelInfo = {};
        channelInfo.channel = w.channel;
        channelInfo.isInput = w.isInput;
        if (ASIOGetChannelInfo(&channelInfo) != ASE_OK) {
            error = "the driver would not describe one of its channels";
            return false;
        }
        if (!pcmFormatSupported(static_cast<int>(channelInfo.type))) {
            char detail[192];
            std::snprintf(detail, sizeof(detail),
                          "%s uses ASIO sample type %ld, which this build does not convert. The "
                          "big-endian and DSD formats are refused rather than guessed at.",
                          mOpenedDriver.c_str(), static_cast<long>(channelInfo.type));
            error = detail;
            return false;
        }
        *w.format = static_cast<int>(channelInfo.type);
    }
    return true;
}

//------------------------------------------------------------------------
bool AsioBackend::open(const char *, Vst::IAudioProcessor *processor, Vst::IComponent *component,
                       const MidiRoute *route)
{
    if (!processor || !component)
        return false;
    if (gBackend && gBackend != this) {
        // The SDK holds one driver in a global, so a second instance would fight the first over it.
        // Refuse rather than corrupt whichever one is already playing.
        std::fprintf(stderr, "namp-rack: an ASIO backend is already open in this process\n");
        return false;
    }

    mProcessor = processor;
    mComponent = component;
    mRoute = route;
    // Read once, here, while the processor is set up and not yet running.
    mPluginLatency = processor->getLatencySamples();

    // ASIO REQUIRES A SINGLE-THREADED APARTMENT, because the driver is instantiated with
    // CoCreateInstance. CoInitialize is CoInitializeEx(0, COINIT_APARTMENTTHREADED). RPC_E_CHANGED_
    // MODE means this thread is already in a multi-threaded apartment, which some hosts do and this
    // standalone does not — it is reported rather than worked around, because a driver created in
    // the wrong apartment is marshalled and its callback latency stops being predictable.
    const HRESULT com = CoInitialize(nullptr);
    if (com == RPC_E_CHANGED_MODE) {
        std::fprintf(stderr, "namp-rack: this thread is in a multi-threaded COM apartment; ASIO "
                             "needs a single-threaded one\n");
        return false;
    }
    mComInitialised = SUCCEEDED(com);

    std::string error;
    if (!openDriver(error)) {
        std::fprintf(stderr, "namp-rack: cannot open ASIO: %s\n", error.c_str());
        closeDriver();
        return false;
    }

    // ---- everything the audio thread will touch is allocated here ---------
    mBlockSize.store(mDriverBlockSize, std::memory_order_relaxed);
    mInputFloat.assign(static_cast<size_t>(mDriverBlockSize), 0.0f);
    for (std::vector<float> &buffer : mOutputFloat)
        buffer.assign(static_cast<size_t>(mDriverBlockSize), 0.0f);

    // bufferSamples is 0 for the same reason it is in the JACK backend: HostProcessData must NOT
    // own the sample buffers, because the bus pointers are aimed at ours each block. Passing a size
    // makes it allocate a buffer per channel and then delete[] whatever the pointers hold at close,
    // which by then is not what it allocated.
    if (!mProcessData.prepare(*component, 0, Vst::kSample32)) {
        std::fprintf(stderr, "namp-rack: cannot prepare the process buffers\n");
        closeDriver();
        return false;
    }
    mInputChanges.setMaxParameters(kMaxInputParameters);
    mOutputChanges.setMaxParameters(kMaxOutputParameters);
    mEvents.setMaxSize(kMaxEvents);

    mProcessData.numSamples = mDriverBlockSize;
    mProcessData.symbolicSampleSize = Vst::kSample32;
    mProcessData.inputParameterChanges = &mInputChanges;
    mProcessData.outputParameterChanges = &mOutputChanges;
    mProcessData.inputEvents = &mEvents;

    // The period a block has to fit inside, in performance-counter ticks. Read once here so the
    // audio thread never asks for the frequency, which is constant for the life of the system.
    LARGE_INTEGER frequency = {};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        mCounterFrequency = frequency.QuadPart;
        mBlockPeriodTicks = static_cast<int64_t>(static_cast<double>(mCounterFrequency) *
                                                 mDriverBlockSize / mSampleRate);
    } else {
        // Without a counter there is no self-measurement, and the driver's own overload reporting
        // is all that is left. Said out loud, because it changes what dropouts() means.
        mCounterFrequency = 0;
        mBlockPeriodTicks = 0;
        std::fprintf(stderr,
                     "namp-rack: no performance counter - late blocks will not be counted\n");
    }

    // ---- the driver's buffers, and the callbacks -------------------------
    ASIOBufferInfo infos[kChannels] = {};
    infos[0].isInput = ASIOTrue;
    infos[0].channelNum = mSettings.inputChannel;
    infos[1].isInput = ASIOFalse;
    infos[1].channelNum = mSettings.outputChannelL;
    infos[2].isInput = ASIOFalse;
    infos[2].channelNum = mSettings.outputChannelR;

    // Published BEFORE ASIOCreateBuffers, because that call installs the callbacks and a driver is
    // entitled to call them from within it.
    gBackend = this;

    ASIOError created = ASIOCreateBuffers(infos, kChannels, mDriverBlockSize, &gCallbacks);
    if (created != ASE_OK && mDriverBlockSize != static_cast<int>(0)) {
        // Some drivers report a legal range and then accept only their preferred size — a known
        // misbehaviour that the reference host implementations also work around, and worth working
        // around here because the alternative is a device that simply will not open.
        long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
        if (ASIOGetBufferSize(&minSize, &maxSize, &preferred, &granularity) == ASE_OK &&
            preferred > 0 && preferred != mDriverBlockSize) {
            std::fprintf(stderr, "namp-rack: %s refused %d frames; using its preferred %ld\n",
                         mOpenedDriver.c_str(), mDriverBlockSize, preferred);
            mDriverBlockSize = static_cast<int>(preferred);
            mBlockSize.store(mDriverBlockSize, std::memory_order_relaxed);
            mInputFloat.assign(static_cast<size_t>(mDriverBlockSize), 0.0f);
            for (std::vector<float> &buffer : mOutputFloat)
                buffer.assign(static_cast<size_t>(mDriverBlockSize), 0.0f);
            mProcessData.numSamples = mDriverBlockSize;
            if (mCounterFrequency > 0)
                mBlockPeriodTicks = static_cast<int64_t>(static_cast<double>(mCounterFrequency) *
                                                         mDriverBlockSize / mSampleRate);
            created = ASIOCreateBuffers(infos, kChannels, mDriverBlockSize, &gCallbacks);
        }
    }
    if (created != ASE_OK) {
        std::fprintf(stderr, "namp-rack: %s would not create buffers at %d frames\n",
                     mOpenedDriver.c_str(), mDriverBlockSize);
        gBackend = nullptr;
        closeDriver();
        return false;
    }
    mBuffersCreated = true;
    mIn.buffers[0] = infos[0].buffers[0];
    mIn.buffers[1] = infos[0].buffers[1];
    for (int channel = 0; channel < 2; ++channel) {
        mOut[channel].buffers[0] = infos[1 + channel].buffers[0];
        mOut[channel].buffers[1] = infos[1 + channel].buffers[1];
    }

    // ASIOOutputReady saves one block of output latency on drivers that can use it, and the SDK's
    // own contract is that a host asks ONCE and then either always calls it or never does.
    mUseOutputReady = ASIOOutputReady() == ASE_OK;

    // Whether the driver can tell us about overloads at all. See dropouts(): this is why that
    // number is not the driver's count alone.
    mDriverReportsOverloads = ASIOFuture(kAsioCanReportOverload, nullptr) == ASE_SUCCESS;

    // The footswitch. Not fatal: the amp plays and the bat switches still change channel.
    mMidi.open(mSettings.midiDevice);

    if (ASIOStart() != ASE_OK) {
        std::fprintf(stderr, "namp-rack: %s would not start\n", mOpenedDriver.c_str());
        gBackend = nullptr;
        closeDriver();
        return false;
    }
    mStarted = true;
    mOpen = true;

    long inputLatency = 0;
    long outputLatency = 0;
    if (ASIOGetLatencies(&inputLatency, &outputLatency) != ASE_OK) {
        inputLatency = 0;
        outputLatency = 0;
    }
    std::printf("namp-rack: ASIO %s at %.0f Hz, %d frames (device latency %ld in / %ld out, "
                "%.1f ms round trip)\n",
                mOpenedDriver.c_str(), mSampleRate, mDriverBlockSize, inputLatency, outputLatency,
                1000.0 * static_cast<double>(inputLatency + outputLatency) / mSampleRate);
    if (!mDriverReportsOverloads)
        std::printf("namp-rack: %s does not report overloads; dropouts are measured here instead\n",
                    mOpenedDriver.c_str());
    return true;
}

//------------------------------------------------------------------------
// Undoes exactly as much of open() as happened, in reverse. Separate from close() because open()
// calls it on every one of its own failure paths, and each of those has got a different distance
// through the sequence.
void AsioBackend::closeDriver()
{
    if (mStarted) {
        ASIOStop();
        mStarted = false;
    }
    if (mBuffersCreated) {
        // Only after ASIOStop: disposing the buffers under a running driver is a use-after-free
        // inside the driver, and the buffers are its memory rather than ours.
        ASIODisposeBuffers();
        mBuffersCreated = false;
    }
    if (mDriverLoaded) {
        ASIOExit(); // which on Windows also removes the current driver from the SDK's list
        mDriverLoaded = false;
    }
    mIn = ChannelBuffers();
    mOut[0] = ChannelBuffers();
    mOut[1] = ChannelBuffers();
    mUseOutputReady = false;
    mDriverReportsOverloads = false;
    mOpen = false;
}

//------------------------------------------------------------------------
void AsioBackend::close()
{
    // The MIDI device first, so no thread is pushing into a ring nobody will drain again.
    mMidi.close();

    closeDriver();

    // Cleared only after the driver is gone: until ASIODisposeBuffers has returned, a callback may
    // still be in flight and it finds the backend through this.
    if (gBackend == this)
        gBackend = nullptr;

    mProcessData.inputEvents = nullptr;
    mProcessData.unprepare();
    mProcessor = nullptr;
    mComponent = nullptr;
    mRoute = nullptr;
    mOpenedDriver.clear();

    if (mComInitialised) {
        // Must be the same thread that called CoInitialize, which it is: every method here except
        // the callbacks runs on the run loop.
        CoUninitialize();
        mComInitialised = false;
    }
}

//------------------------------------------------------------------------
// Main thread (producer).
bool AsioBackend::pushParameter(Vst::ParamID id, Vst::ParamValue value)
{
    const uint32_t write = mRingWrite.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) % kRingSize;
    if (next == mRingRead.load(std::memory_order_acquire))
        return false; // full; drop rather than block either thread
    mRing[write] = {id, value};
    mRingWrite.store(next, std::memory_order_release);
    return true;
}

//------------------------------------------------------------------------
// RT thread (consumer). addParameterData/addPoint only reuse the queues and points reserved during
// open(), so this does not allocate.
void AsioBackend::drainParameterRing()
{
    uint32_t read = mRingRead.load(std::memory_order_relaxed);
    const uint32_t write = mRingWrite.load(std::memory_order_acquire);
    while (read != write) {
        const Change &change = mRing[read];
        int32 index = 0;
        if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(change.id, index)) {
            int32 pointIndex = 0;
            queue->addPoint(0, change.value, pointIndex);
        }
        read = (read + 1) % kRingSize;
    }
    mRingRead.store(read, std::memory_order_release);
}

//------------------------------------------------------------------------
// RT thread. The same two shapes the JACK backend produces, from a different source: the messages
// whole for the rack, and the amp's own route decoded into parameter points and events.
//
// Everything lands at sample 0. On JACK that is a deliberate simplification of a real frame stamp;
// here there is no frame stamp to simplify — WinMM counts milliseconds on its own clock. See
// winmmmidi.h.
void AsioBackend::readMidi()
{
    mRackMidiCount = mMidi.drain(mRackMidi, NAMp::host::kMaxChunkMidi);
    if (!mRoute)
        return; // nothing decoded for the amp, but the rack already has it

    for (int32_t i = 0; i < mRackMidiCount; ++i) {
        const NAMp::host::RtMidiEvent &message = mRackMidi[i];
        const int status = message.status & kMidiStatusMask;
        const int channel = message.status & kMidiChannelMask;
        const int data1 = message.data1 & 0x7f;
        const int data2 = message.data2 & 0x7f;

        switch (status) {
            case kMidiControlChange: {
                const Vst::ParamID id = mRoute->ccParam(channel, data1);
                if (id == Vst::kNoParamId)
                    break;
                int32 index = 0;
                if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(id, index)) {
                    int32 point = 0;
                    queue->addPoint(0, data2 / 127.0, point);
                }
                break;
            }
            case kMidiProgramChange: {
                const Vst::ParamID id = mRoute->programParam(channel);
                if (id == Vst::kNoParamId)
                    break;
                int32 index = 0;
                if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(id, index)) {
                    int32 point = 0;
                    queue->addPoint(0, mRoute->programValue(channel, data1), point);
                }
                break;
            }
            case kMidiNoteOn:
            case kMidiNoteOff: {
                // A note on at velocity 0 is a note off, which is how most controllers send one.
                const bool on = (status == kMidiNoteOn) && data2 > 0;
                Vst::Event event = {};
                event.busIndex = 0;
                event.sampleOffset = 0;
                event.type = on ? static_cast<uint16>(Vst::Event::kNoteOnEvent)
                                : static_cast<uint16>(Vst::Event::kNoteOffEvent);
                if (on) {
                    event.noteOn.channel = static_cast<int16>(channel);
                    event.noteOn.pitch = static_cast<int16>(data1);
                    event.noteOn.velocity = static_cast<float>(data2) / 127.0f;
                    event.noteOn.noteId = -1;
                } else {
                    event.noteOff.channel = static_cast<int16>(channel);
                    event.noteOff.pitch = static_cast<int16>(data1);
                    event.noteOff.velocity = static_cast<float>(data2) / 127.0f;
                    event.noteOff.noteId = -1;
                }
                mEvents.addEvent(event); // full is a drop, not a growth
                break;
            }
            default:
                break;
        }
    }
}

//------------------------------------------------------------------------
// RT thread. Only stores into atomics — the controller is a UI-thread object and must never be
// called from here. Identical to the JACK backend's, because what the editor above reads is the
// same table read through the same interface.
void AsioBackend::publishFeedback()
{
    const int32 count = mOutputChanges.getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::IParamValueQueue *queue = mOutputChanges.getParameterData(i);
        if (!queue)
            continue;
        const int32 points = queue->getPointCount();
        if (points < 1)
            continue;
        int32 offset = 0;
        Vst::ParamValue value = 0.0;
        if (queue->getPoint(points - 1, offset, value) != kResultTrue)
            continue;
        const Vst::ParamID id = queue->getParameterId();

        int used = mFeedbackCount.load(std::memory_order_relaxed);
        int slot = -1;
        for (int s = 0; s < used; ++s) {
            if (mFeedback[s].id.load(std::memory_order_relaxed) == id) {
                slot = s;
                break;
            }
        }
        if (slot < 0) {
            if (used >= kFeedbackSlots)
                continue; // no room, and growing here would be an allocation on this thread
            mFeedback[used].id.store(id, std::memory_order_relaxed);
            slot = used;
            // Released AFTER the id is in place, so a reader that sees this count sees the id too.
            mFeedbackCount.store(used + 1, std::memory_order_release);
        }
        mFeedback[slot].value.store(value, std::memory_order_relaxed);
        // Released LAST, so a reader that sees this sequence number sees the value with it.
        mFeedback[slot].seq.fetch_add(1, std::memory_order_release);
    }
}

//------------------------------------------------------------------------
int AsioBackend::takeBufferSizeChange()
{
    const int size = mNewBlockSize.exchange(0, std::memory_order_acquire);
    if (size <= 0 || size == mBlockSize.load(std::memory_order_relaxed))
        return 0;
    return size;
}

//------------------------------------------------------------------------
// The driver's name is the whole point of this line: it is what the user chose, and on a machine with
// more than one interface it is the only way to tell from the window which one is sounding.
std::string AsioBackend::deviceSummary() const
{
    if (!isOpen())
        return std::string();
    char text[192];
    std::snprintf(text, sizeof(text), "ASIO %s, %.0f Hz, %d frames",
                  mOpenedDriver.empty() ? "(unnamed driver)" : mOpenedDriver.c_str(), mSampleRate,
                  mBlockSize.load(std::memory_order_relaxed));
    return std::string(text);
}

//------------------------------------------------------------------------
bool AsioBackend::takeDeviceReset()
{
    return mDeviceReset.exchange(false, std::memory_order_acquire);
}

//------------------------------------------------------------------------
bool AsioBackend::suspendProcessing()
{
    if (!mOpen)
        return true; // no audio thread to race with

    mSuspended.store(true, std::memory_order_release);

    // Two cycles, not one: the first may already have been inside the callback when the flag went
    // up, so only the second is guaranteed to have seen it.
    const uint32_t start = mCycle.load(std::memory_order_acquire);
    for (int attempt = 0; attempt < 200; ++attempt) { // ~2 s
        if (mCycle.load(std::memory_order_acquire) - start >= 2)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // The driver is not calling back — it was stopped behind our back, or the device went away. Say
    // so and leave the processor alone rather than reconfiguring it underneath a callback that
    // might still come back.
    mSuspended.store(false, std::memory_order_release);
    return false;
}

//------------------------------------------------------------------------
void AsioBackend::resumeProcessing(int blockSize)
{
    if (blockSize > 0)
        mBlockSize.store(blockSize, std::memory_order_relaxed);
    mSuspended.store(false, std::memory_order_release);
}

//------------------------------------------------------------------------
// The driver's thread, possibly at interrupt time. Nothing here may allocate, lock, log or block.
//
// A DRIVER RESET IS RECORDED AND NOTHING MORE. Reopening the device from inside the driver's own
// callback is exactly what the SDK says not to do, so the run loop takes it through
// takeDeviceReset().
long AsioBackend::message(long selector, long value)
{
    switch (selector) {
        case kAsioSelectorSupported:
            // Answering honestly matters: a driver that is told a selector is supported will use
            // it.
            switch (value) {
                case kAsioEngineVersion:
                case kAsioResetRequest:
                case kAsioResyncRequest:
                case kAsioLatenciesChanged:
                case kAsioOverload:
                    return 1;
                default:
                    return 0;
            }
        case kAsioEngineVersion:
            return 2; // ASIO 2, which is what ASIOOutputReady and the buffer contract above assume
        case kAsioResetRequest:
            mDeviceReset.store(true, std::memory_order_release);
            return 1;
        case kAsioResyncRequest:
            // The driver lost data and its timestamps are no longer trustworthy. Most drivers
            // report a glitch this way rather than through kAsioOverload, which is why both are
            // counted.
            mOverloads.fetch_add(1, std::memory_order_relaxed);
            return 1;
        case kAsioOverload:
            mOverloads.fetch_add(1, std::memory_order_relaxed);
            return 1;
        case kAsioLatenciesChanged:
            // The figure printed at open() is now stale. Nothing downstream compensates against it
            // — there is no graph here, only the device — so this is acknowledged and not acted on.
            return 1;
        case kAsioSupportsTimeInfo:
            // 0: this host uses the plain bufferSwitch. It needs no timestamps, and the buffer
            // index is the only thing the callback actually acts on.
            return 0;
        case kAsioSupportsTimeCode:
            return 0;
        default:
            return 0;
    }
}

//------------------------------------------------------------------------
// The driver's thread. A rate change under a running stream invalidates the processor's setup, the
// crossfade's ramp lengths and every model's Reset() — far more than a block-size change — so it is
// treated as a reopen, which is what it is.
void AsioBackend::sampleRateChanged(double)
{
    mDeviceReset.store(true, std::memory_order_release);
}

//------------------------------------------------------------------------
// The driver's real-time thread. Nothing here allocates, locks, or logs.
void AsioBackend::bufferSwitch(long doubleBufferIndex)
{
    // Raised for the whole callback, not per node — the same reasoning as the JACK backend's, and
    // the same one-increment cost when nothing is armed. It is what lets the host layer's
    // allocation counter tell an allocation made during a load from one made while audio is
    // running.
    const NAMp::host::RtScope rtScope;

    const int index = (doubleBufferIndex == 0) ? 0 : 1;
    void *outBuffers[2] = {mOut[0].buffers[index], mOut[1].buffers[index]};
    const void *inBuffer = mIn.buffers[index];
    if (!outBuffers[0] || !outBuffers[1] || !inBuffer)
        return;

    const int frames = mDriverBlockSize;

    // Timed with the performance counter, which does not enter the kernel on any Windows this
    // product runs on and does not allocate or lock. Two reads per block, to answer the one
    // question the live gate actually asks: did this fit?
    LARGE_INTEGER started = {};
    if (mBlockPeriodTicks > 0)
        QueryPerformanceCounter(&started);

    // Suspended: the UI thread is reconfiguring the processor and must not be raced. Silence is the
    // honest output — a reconfiguration already puts a gap in the audio, and stale samples in the
    // driver's buffer would be worse, because a driver buffer is not cleared between blocks.
    if (!mProcessor || mSuspended.load(std::memory_order_acquire)) {
        for (int channel = 0; channel < 2; ++channel) {
            std::memset(mOutputFloat[channel].data(), 0,
                        static_cast<size_t>(frames) * sizeof(float));
            floatToPcm(mOutputFloat[channel].data(), outBuffers[channel], mOut[channel].format,
                       frames);
        }
        if (mUseOutputReady)
            ASIOOutputReady();
        mCycle.fetch_add(1, std::memory_order_release);
        return;
    }

    pcmToFloat(inBuffer, mIn.format, mInputFloat.data(), frames);

    mInputChanges.clearQueue();
    mOutputChanges.clearQueue();
    mEvents.clear();
    drainParameterRing();
    readMidi();
    mProcessData.inputEvents = &mEvents;

    float *outL = mOutputFloat[0].data();
    float *outR = mOutputFloat[1].data();
    const float *in = mInputFloat.data();

    // Once per driver block, never per chunk: adopting a chain mid-block would run the first chunk
    // with one topology and the rest with another.
    if (mChain)
        mChain->beginBlock(mRackMidi, mRackMidiCount);

    // Loop, never clamp — the same reason the JACK backend loops. ASIO's own size is fixed for the
    // session, but the PROCESSOR's is whatever it was last set up for, and between a reset and the
    // run loop acting on it the two disagree. Handing a model more than it was Reset() with would
    // silently wipe its convolution history, and truncating would leave the rest of the driver's
    // buffer holding the previous block.
    const int chunk = mBlockSize.load(std::memory_order_relaxed);
    int done = 0;
    while (done < frames) {
        const int32 n = static_cast<int32>(std::min(chunk, frames - done));

        NAMp::host::ChainIo io;
        if (mChain) {
            mChain->beginChunk(in + done, outL + done, outR + done, n, io);
        } else {
            io.anchorIn = in + done;
            io.anchorOut[0] = outL + done;
            io.anchorOut[1] = outR + done;
        }

        if (mProcessData.inputs && mProcessData.inputs[0].numChannels > 0)
            mProcessData.inputs[0].channelBuffers32[0] = const_cast<float *>(io.anchorIn);
        if (mProcessData.outputs && mProcessData.outputs[0].numChannels > 1) {
            mProcessData.outputs[0].channelBuffers32[0] = io.anchorOut[0];
            mProcessData.outputs[0].channelBuffers32[1] = io.anchorOut[1];
        }
        mProcessData.numSamples = n;

        mProcessor->process(mProcessData);

        if (mChain)
            mChain->endChunk();

        publishFeedback();
        // The queued edits and the MIDI belong to the top of the block, not to every chunk of it.
        mInputChanges.clearQueue();
        mOutputChanges.clearQueue();
        mEvents.clear();
        mProcessData.inputEvents = nullptr;
        done += n;
    }

    for (int channel = 0; channel < 2; ++channel)
        floatToPcm(mOutputFloat[channel].data(), outBuffers[channel], mOut[channel].format, frames);

    // Tells the driver the output half is complete, which saves it a block of latency. Called for
    // every block or for none, per the SDK's contract.
    if (mUseOutputReady)
        ASIOOutputReady();

    if (mBlockPeriodTicks > 0) {
        LARGE_INTEGER finished = {};
        if (QueryPerformanceCounter(&finished) &&
            finished.QuadPart - started.QuadPart > mBlockPeriodTicks) {
            // Longer than the block's own period, which is the whole budget: the driver is playing
            // out one half while we fill the other, so exceeding it means the next half was not
            // ready. That is a dropout whatever the driver chooses to report.
            mLateBlocks.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Every exit from this callback bumps the cycle counter, including the early ones:
    // suspendProcessing() waits on it, and a path that skipped it would look like an audio thread
    // that had stopped responding.
    mCycle.fetch_add(1, std::memory_order_release);
}

} // namespace Rations
