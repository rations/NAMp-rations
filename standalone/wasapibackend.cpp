// WasapiBackend implementation. See wasapibackend.h for what is WASAPI's own, asiobackend.h for the
// parts both Windows backends share, and audiobackend.h for the contract.

// INITGUID FIRST, BEFORE EVERY INCLUDE, and it is load-bearing rather than conventional.
//
// This file names PKEY_Device_FriendlyName, the property key an endpoint's shown name is read from.
// propkeydef.h's DEFINE_PROPERTYKEY only DECLARES a key unless INITGUID is set, and — measured, by
// searching every archive in the MinGW sysroot for the symbol — nothing there defines it. Without
// this the link fails with an undefined reference to it.
//
// WHAT MADE THAT WORTH A COMMENT is when it appeared. This backend is a static library, and a
// static library's undefined references are not errors until something links it, so the file
// compiled clean for as long as no executable used it. The Windows standalone is what finally did,
// and the missing symbol surfaced there rather than here.
//
// Setting it in this file is safe, and would be safe in any number of files: with INITGUID the
// macro emits the definition as DECLSPEC_SELECTANY, which is precisely the attribute that lets
// duplicate definitions across translation units be merged rather than collide.
#define INITGUID

#include "wasapibackend.h"
#include "pcmsamples.h"

#include "host/hostapp.h"

#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstevents.h"

#include <windows.h>
// After windows.h, which all four of these expect to have been included first.
#include <avrt.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <thread>

using namespace Steinberg;

namespace Rations
{

namespace
{

// The same figures, for the same reasons, as the other two backends'. See jackclient.cpp.
constexpr int32 kMaxInputParameters = 64;
constexpr int32 kMaxOutputParameters = 8;
constexpr int32 kMaxEvents = 256;

constexpr int kMidiStatusMask = 0xf0;
constexpr int kMidiChannelMask = 0x0f;
constexpr int kMidiNoteOff = 0x80;
constexpr int kMidiNoteOn = 0x90;
constexpr int kMidiControlChange = 0xb0;
constexpr int kMidiProgramChange = 0xc0;

// A REFERENCE_TIME is 100-nanosecond units, which is what every duration in this API is expressed
// in.
constexpr double kRefTimesPerSecond = 10000000.0;

// How long the audio thread will wait for the render event before deciding the device has stopped
// answering. Generous: a period is a handful of milliseconds and even a badly behaved system does
// not pause for a fifth of a second, so anything past this is the device going away rather than a
// hiccup.
constexpr DWORD kEventTimeoutMs = 200;

// How long open() will wait for the thread to finish negotiating. Device activation talks to the
// audio service and can be slow on a cold system; this is not an audio-path number and generosity
// costs nothing but a slower failure.
constexpr DWORD kSetupTimeoutMs = 5000;

//------------------------------------------------------------------------
// A COM interface pointer released and nulled in one place, so the teardown paths below cannot
// release one twice or forget one. Deliberately not a smart pointer: these live in void* members of
// a header that must not include windows.h, and hiding that behind a template would only make the
// casts harder to see.
template <typename T> void releaseInterface(void *&pointer)
{
    if (pointer) {
        static_cast<T *>(pointer)->Release();
        pointer = nullptr;
    }
}

//------------------------------------------------------------------------
// A wide COM string as UTF-8, without ever throwing. std::filesystem::path is the conversion for
// the same reason it is everywhere else in this tree — libstdc++ uses UTF-8 for a path's narrow
// form whatever the code page — and a device whose name will not convert is better shown by its id
// than not shown.
std::string wideToUtf8(const wchar_t *w)
{
    if (!w)
        return {};
    try {
        return std::filesystem::path(w).string();
    } catch (...) {
        return {};
    }
}

//------------------------------------------------------------------------
// An endpoint's friendly name. Failure is not an error: the picker falls back to the id.
std::string endpointName(IMMDevice *device)
{
    if (!device)
        return {};
    IPropertyStore *store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store)
        return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
        name = wideToUtf8(value.pwszVal);
    PropVariantClear(&value);
    store->Release();
    return name;
}

//------------------------------------------------------------------------
// The pcmsamples.h layout a WAVEFORMATEX describes, or 0 for one this build will not carry.
//
// THE SUBFORMAT IS WHAT MATTERS, NOT wFormatTag. A modern endpoint reports WAVE_FORMAT_EXTENSIBLE
// and puts the real thing in SubFormat, and it is also the only place the VALID bit count lives — a
// 24-bits-in-32 stream is wBitsPerSample 32 with wValidBitsPerSample 24, and treating it as a
// full-scale 32-bit stream is 256 times too quiet.
int pcmFormatOf(const WAVEFORMATEX *format)
{
    if (!format)
        return 0;

    bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool isPcm = format->wFormatTag == WAVE_FORMAT_PCM;
    WORD validBits = format->wBitsPerSample;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE *extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        isFloat = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        isPcm = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
        if (extensible->Samples.wValidBitsPerSample > 0)
            validBits = extensible->Samples.wValidBitsPerSample;
    }

    if (isFloat) {
        if (format->wBitsPerSample == 32)
            return kPcmFloat32LSB;
        if (format->wBitsPerSample == 64)
            return kPcmFloat64LSB;
        return 0;
    }
    if (!isPcm)
        return 0;

    switch (format->wBitsPerSample) {
        case 16:
            return validBits == 16 ? kPcmInt16LSB : 0;
        case 24:
            // Packed: three bytes per sample, which is why pcmsamples.h reads it byte by byte.
            return validBits == 24 ? kPcmInt24LSB : 0;
        case 32:
            switch (validBits) {
                case 32:
                    return kPcmInt32LSB;
                case 24:
                    return kPcmInt32LSB24;
                case 20:
                    return kPcmInt32LSB20;
                case 18:
                    return kPcmInt32LSB18;
                case 16:
                    return kPcmInt32LSB16;
                default:
                    return 0;
            }
        default:
            return 0;
    }
}

//------------------------------------------------------------------------
// One candidate exclusive-mode format, filled in. Exclusive mode has no mixer to convert anything,
// so a format is either what the hardware does or it is refused — which means proposing several and
// asking.
void fillFormat(WAVEFORMATEXTENSIBLE &out, int channels, double rate, int bits, int validBits,
                bool isFloat)
{
    std::memset(&out, 0, sizeof(out));
    out.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    out.Format.nChannels = static_cast<WORD>(channels);
    out.Format.nSamplesPerSec = static_cast<DWORD>(rate);
    out.Format.wBitsPerSample = static_cast<WORD>(bits);
    out.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    out.Format.nAvgBytesPerSec = out.Format.nSamplesPerSec * out.Format.nBlockAlign;
    out.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    out.Samples.wValidBitsPerSample = static_cast<WORD>(validBits);
    // A channel mask is required for WAVE_FORMAT_EXTENSIBLE. Front left/right for two channels, and
    // for anything else the first N channels, which is what a driver expects when it is handed a
    // mask it did not choose.
    out.dwChannelMask =
        (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : ((1u << channels) - 1u);
    out.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

} // namespace

//------------------------------------------------------------------------
// Main thread, before open. See the header for why the block size is an estimate.
bool WasapiBackend::probeDefaults(double &sampleRate, int &blockSize)
{
    // Its own apartment, entered and left here, for the same reason enumerate() has one: this runs
    // before the audio thread exists and must not depend on what the calling thread has done about
    // COM. RPC_E_CHANGED_MODE means the thread is already in an apartment of the other kind, which
    // is fine for a query.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownCom = SUCCEEDED(com);

    bool ok = false;
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    WAVEFORMATEX *mixFormat = nullptr;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void **>(&enumerator))) &&
        enumerator) {
        // The saved endpoint if it is still there, the default otherwise — the same fallback open()
        // makes, so the numbers reported here describe the device that will actually be opened.
        if (!mSettings.renderDeviceId.empty()) {
            try {
                const std::wstring wide = std::filesystem::path(mSettings.renderDeviceId).wstring();
                if (FAILED(enumerator->GetDevice(wide.c_str(), &device)))
                    device = nullptr;
            } catch (...) {
                device = nullptr;
            }
        }
        if (!device && FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
            device = nullptr;

        // ACTIVATED BUT NOT INITIALISED. Activate hands back a client object; it is Initialize that
        // takes the device, and that is deliberately not called here.
        if (device &&
            SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void **>(&client))) &&
            client) {
            REFERENCE_TIME defaultPeriod = 0;
            REFERENCE_TIME minimumPeriod = 0;
            if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod)) &&
                SUCCEEDED(client->GetMixFormat(&mixFormat)) && mixFormat &&
                mixFormat->nSamplesPerSec > 0 && defaultPeriod > 0) {
                const double rate = static_cast<double>(mixFormat->nSamplesPerSec);
                // Rounded UP: a block size is a buffer to be sized, and one frame short of the
                // period is a buffer that overruns by one frame.
                const double frames =
                    std::ceil(rate * static_cast<double>(defaultPeriod) / kRefTimesPerSecond);
                if (frames >= 1.0 && frames < 65536.0) {
                    sampleRate = rate;
                    blockSize = static_cast<int>(frames);
                    ok = true;
                }
            }
        }
    }

    if (mixFormat)
        CoTaskMemFree(mixFormat);
    if (client)
        client->Release();
    if (device)
        device->Release();
    if (enumerator)
        enumerator->Release();
    if (ownCom)
        CoUninitialize();

    if (!ok)
        std::fprintf(stderr, "namp-rack: no usable WASAPI output endpoint to ask about\n");
    return ok;
}

//------------------------------------------------------------------------
// Main thread. Enumeration only: no stream is opened and no device is taken, so this is safe to
// call while something else is playing.
bool WasapiBackend::enumerate(bool capture, std::vector<DeviceEntry> &out)
{
    out.clear();

    // Its own apartment, entered and left here, because this is called from the picker and must not
    // depend on whatever the calling thread has done about COM. RPC_E_CHANGED_MODE means the thread
    // is already in an apartment of the other kind, which is fine for enumeration — WASAPI's
    // enumerator is usable from either — so it is not treated as a failure.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownCom = SUCCEEDED(com);

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr =
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        if (ownCom)
            CoUninitialize();
        return false;
    }

    const EDataFlow flow = capture ? eCapture : eRender;

    // The default endpoint, so the list can mark it. eConsole rather than eCommunications: this is
    // a musical instrument, not a headset, and the two roles can point at different devices.
    std::string defaultId;
    IMMDevice *defaultDevice = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDevice)) &&
        defaultDevice) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
            defaultId = wideToUtf8(id);
            CoTaskMemFree(id);
        }
        defaultDevice->Release();
    }

    // ACTIVE only. An unplugged or disabled endpoint still exists in the registry and offering one
    // would be offering something that cannot open.
    IMMDeviceCollection *collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) ||
        !collection) {
        enumerator->Release();
        if (ownCom)
            CoUninitialize();
        return false;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device)
            continue;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            DeviceEntry entry;
            entry.id = wideToUtf8(id);
            entry.name = endpointName(device);
            if (entry.name.empty())
                entry.name = entry.id; // better the id than a blank row
            entry.isDefault = !entry.id.empty() && entry.id == defaultId;
            if (!entry.id.empty())
                out.push_back(std::move(entry));
            CoTaskMemFree(id);
        }
        device->Release();
    }

    collection->Release();
    enumerator->Release();
    if (ownCom)
        CoUninitialize();

    // The default first, so the picker's first row is the one most machines should stay on.
    std::stable_sort(out.begin(), out.end(), [](const DeviceEntry &a, const DeviceEntry &b) {
        return a.isDefault > b.isDefault;
    });
    return true;
}

//------------------------------------------------------------------------
WasapiBackend::~WasapiBackend()
{
    close();
}

//------------------------------------------------------------------------
// Main thread.
bool WasapiBackend::readFeedback(int index, Vst::ParamID &id, double &value, uint32_t &seq) const
{
    if (index < 0 || index >= mFeedbackCount.load(std::memory_order_acquire))
        return false;
    const FeedbackSlot &slot = mFeedback[index];
    seq = slot.seq.load(std::memory_order_acquire);
    id = slot.id.load(std::memory_order_relaxed);
    value = slot.value.load(std::memory_order_relaxed);
    return true;
}

//------------------------------------------------------------------------
bool WasapiBackend::open(const char *, Vst::IAudioProcessor *processor, Vst::IComponent *component,
                         const MidiRoute *route)
{
    if (!processor || !component)
        return false;
    if (mThread.joinable())
        return false; // already open; close() first

    mProcessor = processor;
    mComponent = component;
    mRoute = route;
    mPluginLatency = processor->getLatencySamples();
    mStop.store(false, std::memory_order_relaxed);
    mSetupOk.store(false, std::memory_order_relaxed);
    mSetupError.clear();

    // Manual-reset events, because each is waited on once and its state has to survive the wait.
    mSetupDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    mGoAhead = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!mSetupDone || !mGoAhead) {
        std::fprintf(stderr, "namp-rack: cannot create the audio thread's events\n");
        close();
        return false;
    }

    // Everything VST3 that does not need the device's numbers. prepare() reads the component's bus
    // arrangement, which is a main-thread call, so it happens here rather than on the audio thread
    // — and bufferSamples is 0 for the same reason as in the other two backends: the bus pointers
    // are aimed at our own float buffers each block, and a HostProcessData that thought it owned
    // them would delete[] the wrong memory at close.
    if (!mProcessData.prepare(*component, 0, Vst::kSample32)) {
        std::fprintf(stderr, "namp-rack: cannot prepare the process buffers\n");
        close();
        return false;
    }
    mInputChanges.setMaxParameters(kMaxInputParameters);
    mOutputChanges.setMaxParameters(kMaxOutputParameters);
    mEvents.setMaxSize(kMaxEvents);
    mProcessData.symbolicSampleSize = Vst::kSample32;
    mProcessData.inputParameterChanges = &mInputChanges;
    mProcessData.outputParameterChanges = &mOutputChanges;
    mProcessData.inputEvents = &mEvents;

    // The thread enters the apartment and negotiates; see the header on why that is its job and not
    // this one's.
    mThread = std::thread([this] { audioThread(); });

    if (WaitForSingleObject(static_cast<HANDLE>(mSetupDone), kSetupTimeoutMs) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "namp-rack: the audio device did not answer in %lu ms\n",
                     static_cast<unsigned long>(kSetupTimeoutMs));
        close();
        return false;
    }
    if (!mSetupOk.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "namp-rack: cannot open WASAPI: %s\n",
                     mSetupError.empty() ? "no reason given" : mSetupError.c_str());
        close();
        return false;
    }

    // ---- every allocation, here on the main thread, now that the sizes are known -----
    const int block = mBlockSize.load(std::memory_order_relaxed);
    mInputFloat.assign(static_cast<size_t>(block), 0.0f);
    for (std::vector<float> &buffer : mOutputFloat)
        buffer.assign(static_cast<size_t>(block), 0.0f);

    mRingCapacity = block * kRingBlocks;
    mCaptureRing.assign(static_cast<size_t>(mRingCapacity), 0.0f);
    mRingWritePos = 0;
    mRingReadPos = 0;
    mRingFill = 0;

    mProcessData.numSamples = block;

    // The footswitch. Not fatal.
    mMidi.open(mSettings.midiDevice);

    SetEvent(static_cast<HANDLE>(mGoAhead));

    std::printf("namp-rack: WASAPI %s at %.0f Hz, %d frames (%s mode)\n", mRenderName.c_str(),
                mSampleRate, block, mOpenedExclusive ? "exclusive" : "shared");
    if (mCaptureName != mRenderName)
        std::printf("namp-rack: input from %s\n", mCaptureName.c_str());
    if (mTwoClocks)
        std::printf("namp-rack: input and output are different devices, so their clocks drift; "
                    "corrections are counted and shown on the setup page\n");
    return true;
}

//------------------------------------------------------------------------
void WasapiBackend::close()
{
    mMidi.close();

    if (mThread.joinable()) {
        mStop.store(true, std::memory_order_release);
        // Both events, because the thread may be waiting on either one: the render event in the
        // loop, or the go-ahead if open() failed after the thread had already finished negotiating.
        if (mRenderEvent)
            SetEvent(static_cast<HANDLE>(mRenderEvent));
        if (mGoAhead)
            SetEvent(static_cast<HANDLE>(mGoAhead));
        mThread.join();
    }
    mRunning.store(false, std::memory_order_release);

    if (mSetupDone) {
        CloseHandle(static_cast<HANDLE>(mSetupDone));
        mSetupDone = nullptr;
    }
    if (mGoAhead) {
        CloseHandle(static_cast<HANDLE>(mGoAhead));
        mGoAhead = nullptr;
    }

    mProcessData.inputEvents = nullptr;
    mProcessData.unprepare();
    mProcessor = nullptr;
    mComponent = nullptr;
    mRoute = nullptr;
    mCaptureName.clear();
    mRenderName.clear();
    mOpenedExclusive = false;
    mTwoClocks = false;
}

//------------------------------------------------------------------------
// Main thread (producer).
bool WasapiBackend::pushParameter(Vst::ParamID id, Vst::ParamValue value)
{
    const uint32_t write = mParamWrite.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) % kParamRingSize;
    if (next == mParamRead.load(std::memory_order_acquire))
        return false;
    mParamRing[write] = {id, value};
    mParamWrite.store(next, std::memory_order_release);
    return true;
}

//------------------------------------------------------------------------
int WasapiBackend::takeBufferSizeChange()
{
    const int size = mNewBlockSize.exchange(0, std::memory_order_acquire);
    if (size <= 0 || size == mBlockSize.load(std::memory_order_relaxed))
        return 0;
    return size;
}

//------------------------------------------------------------------------
// The share mode and the two-clock case are here because both are things the user did not ask for and
// needs to know: exclusive mode may have been refused by whatever else holds the device, and a rig
// with the guitar on an interface and the sound on the laptop's speakers is running two crystals. The
// drift corrections are what that second one costs, so they are shown where it is named.
std::string WasapiBackend::deviceSummary() const
{
    if (!isOpen())
        return std::string();
    char text[256];
    const uint32_t drift = driftCorrections();
    if (mTwoClocks)
        std::snprintf(text, sizeof(text),
                      "WASAPI %s, %.0f Hz, %d frames, two clocks (%u drift correction%s)",
                      mOpenedExclusive ? "exclusive" : "shared", mSampleRate,
                      mBlockSize.load(std::memory_order_relaxed), drift, drift == 1 ? "" : "s");
    else
        std::snprintf(text, sizeof(text), "WASAPI %s, %.0f Hz, %d frames",
                      mOpenedExclusive ? "exclusive" : "shared", mSampleRate,
                      mBlockSize.load(std::memory_order_relaxed));
    return std::string(text);
}

//------------------------------------------------------------------------
bool WasapiBackend::takeDeviceReset()
{
    return mDeviceReset.exchange(false, std::memory_order_acquire);
}

//------------------------------------------------------------------------
bool WasapiBackend::suspendProcessing()
{
    if (!isOpen())
        return true;

    mSuspended.store(true, std::memory_order_release);
    const uint32_t start = mCycle.load(std::memory_order_acquire);
    for (int attempt = 0; attempt < 200; ++attempt) { // ~2 s
        if (mCycle.load(std::memory_order_acquire) - start >= 2)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    mSuspended.store(false, std::memory_order_release);
    return false;
}

//------------------------------------------------------------------------
void WasapiBackend::resumeProcessing(int blockSize)
{
    if (blockSize > 0)
        mBlockSize.store(blockSize, std::memory_order_relaxed);
    mSuspended.store(false, std::memory_order_release);
}

//------------------------------------------------------------------------
// RT thread. Identical to the other backends', deliberately.
void WasapiBackend::drainParameterRing()
{
    uint32_t read = mParamRead.load(std::memory_order_relaxed);
    const uint32_t write = mParamWrite.load(std::memory_order_acquire);
    while (read != write) {
        const Change &change = mParamRing[read];
        int32 index = 0;
        if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(change.id, index)) {
            int32 pointIndex = 0;
            queue->addPoint(0, change.value, pointIndex);
        }
        read = (read + 1) % kParamRingSize;
    }
    mParamRead.store(read, std::memory_order_release);
}

//------------------------------------------------------------------------
// RT thread.
void WasapiBackend::readMidi()
{
    mRackMidiCount = mMidi.drain(mRackMidi, NAMp::host::kMaxChunkMidi);
    if (!mRoute)
        return;

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
                mEvents.addEvent(event);
                break;
            }
            default:
                break;
        }
    }
}

//------------------------------------------------------------------------
// RT thread. Only stores into atomics.
void WasapiBackend::publishFeedback()
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
                continue;
            mFeedback[used].id.store(id, std::memory_order_relaxed);
            slot = used;
            mFeedbackCount.store(used + 1, std::memory_order_release);
        }
        mFeedback[slot].value.store(value, std::memory_order_relaxed);
        mFeedback[slot].seq.fetch_add(1, std::memory_order_release);
    }
}

//------------------------------------------------------------------------
// The audio thread, and everything COM. See the header: the apartment lives here because this is a
// thread this class owns outright, and neither the ASIO backend's single-threaded requirement nor a
// hosted plug-in's editor can have got to it first.
void WasapiBackend::audioThread()
{
    // MULTI-THREADED APARTMENT, which is what WASAPI's own samples use and what lets these
    // interfaces be created and called without marshalling.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    mComInitialised = SUCCEEDED(com);

    std::string error;
    const bool ok = setUpDevices(error);
    mSetupError = error;
    // Released AFTER the error string and every negotiated number is in place, so open() reading
    // this sees all of them.
    mSetupOk.store(ok, std::memory_order_release);
    SetEvent(static_cast<HANDLE>(mSetupDone));

    if (!ok) {
        releaseDevices();
        if (mComInitialised) {
            CoUninitialize();
            mComInitialised = false;
        }
        return;
    }

    // open() allocates the buffers now that it knows the sizes, and releases this.
    WaitForSingleObject(static_cast<HANDLE>(mGoAhead), INFINITE);
    if (mStop.load(std::memory_order_acquire)) {
        releaseDevices();
        if (mComInitialised) {
            CoUninitialize();
            mComInitialised = false;
        }
        return;
    }

    // Pro Audio priority, which is what the scheduler needs to be told before a thread with a
    // millisecond deadline is trusted with one. Not fatal if it is refused — the audio is more
    // likely to glitch, which is exactly what dropouts() will then say.
    DWORD taskIndex = 0;
    mMmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!mMmcssHandle)
        std::fprintf(stderr, "namp-rack: could not raise the audio thread's priority\n");

    IAudioClient *capture = static_cast<IAudioClient *>(mCaptureClient);
    IAudioClient *render = static_cast<IAudioClient *>(mRenderClient);

    // Capture first, so that by the time render asks for a block there is already input in the ring
    // rather than a block of silence at the very start.
    if (capture)
        capture->Start();
    if (render)
        render->Start();
    mRunning.store(true, std::memory_order_release);

    while (!mStop.load(std::memory_order_acquire)) {
        const DWORD waited =
            WaitForSingleObject(static_cast<HANDLE>(mRenderEvent), kEventTimeoutMs);
        if (mStop.load(std::memory_order_acquire))
            break;
        if (waited == WAIT_TIMEOUT) {
            // The device has stopped asking for audio. On Windows that is almost always the
            // endpoint going away — unplugged, or the default device changed under us — and the
            // answer is a reopen rather than a retry, so it is reported and the loop ends.
            std::fprintf(stderr, "namp-rack: the audio device stopped responding\n");
            mDeviceReset.store(true, std::memory_order_release);
            break;
        }
        if (waited != WAIT_OBJECT_0)
            break;
        runBlock();
    }

    mRunning.store(false, std::memory_order_release);
    if (render)
        render->Stop();
    if (capture)
        capture->Stop();

    if (mMmcssHandle) {
        AvRevertMmThreadCharacteristics(static_cast<HANDLE>(mMmcssHandle));
        mMmcssHandle = nullptr;
    }

    // Released on the thread that created them, and before the apartment they live in is left.
    releaseDevices();
    if (mComInitialised) {
        CoUninitialize();
        mComInitialised = false;
    }
}

//------------------------------------------------------------------------
// The audio thread, before the loop. Activates both endpoints, negotiates a format and a period,
// and publishes the numbers open() needs to size its buffers.
bool WasapiBackend::setUpDevices(std::string &error)
{
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr =
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        error = "the audio endpoint enumerator could not be created";
        return false;
    }
    mEnumerator = enumerator;

    // A named endpoint, or the system default. Falling back to the default when a saved id no
    // longer exists is the right behaviour rather than a refusal: the interface the settings were
    // written for is simply not plugged in, and the user needs a window and some sound before they
    // can say so.
    auto activate = [&](bool capture, const std::string &wantedId, void *&deviceOut,
                        std::string &nameOut) -> bool {
        IMMDevice *device = nullptr;
        if (!wantedId.empty()) {
            try {
                const std::wstring wide = std::filesystem::path(wantedId).wstring();
                if (FAILED(enumerator->GetDevice(wide.c_str(), &device)))
                    device = nullptr;
            } catch (...) {
                device = nullptr;
            }
            if (!device)
                std::fprintf(stderr,
                             "namp-rack: the saved %s device is not available; using the default\n",
                             capture ? "input" : "output");
        }
        if (!device) {
            if (FAILED(enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole,
                                                           &device)) ||
                !device)
                return false;
        }
        nameOut = endpointName(device);
        deviceOut = device;
        return true;
    };

    if (!activate(true, mSettings.captureDeviceId, mCaptureDevice, mCaptureName)) {
        error = "this machine has no audio input";
        return false;
    }
    if (!activate(false, mSettings.renderDeviceId, mRenderDevice, mRenderName)) {
        error = "this machine has no audio output";
        return false;
    }

    // Whether the two are one piece of hardware, which is what decides whether their clocks drift
    // at all. Compared by ENDPOINT ID rather than by friendly name: two endpoints of one interface
    // have different ids, so this is deliberately a comparison of the DEVICE part — and since an
    // id's structure is not documented, the honest test is whether the names match, which is what
    // Windows itself shows the user. A false "two clocks" costs a counter that never fires; a false
    // "one clock" would cost a drift that is never corrected, so the doubt is resolved the safe
    // way.
    mTwoClocks = mCaptureName.empty() || mRenderName.empty() || mCaptureName != mRenderName;

    // ---- the clients ------------------------------------------------------
    IAudioClient *capture = nullptr;
    IAudioClient *render = nullptr;
    if (FAILED(static_cast<IMMDevice *>(mCaptureDevice)
                   ->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&capture))) ||
        !capture) {
        error = "the input device could not be activated";
        return false;
    }
    mCaptureClient = capture;
    if (FAILED(static_cast<IMMDevice *>(mRenderDevice)
                   ->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&render))) ||
        !render) {
        error = "the output device could not be activated";
        return false;
    }
    mRenderClient = render;

    // ---- the format and the period ---------------------------------------
    // initialiseStream does one client. It is a lambda rather than a method because it needs to be
    // able to REPLACE the client it was given: AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED is answered by
    // throwing the client away and activating a new one, which is what the platform documents.
    struct Negotiated {
        int format = 0;
        int channels = 0;
        double rate = 0.0;
        int frames = 0;
    };

    auto initialiseStream = [&](bool capture_, void *&clientSlot, void *device, bool exclusive,
                                double wantedRate, Negotiated &out, std::string &why) -> bool {
        IAudioClient *client = static_cast<IAudioClient *>(clientSlot);
        if (!client)
            return false;

        REFERENCE_TIME defaultPeriod = 0;
        REFERENCE_TIME minimumPeriod = 0;
        if (FAILED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
            why = "the device would not report its period";
            return false;
        }

        WAVEFORMATEX *mixFormat = nullptr;
        if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
            why = "the device would not report its format";
            return false;
        }

        WAVEFORMATEXTENSIBLE chosen = {};
        const WAVEFORMATEX *chosenFormat = nullptr;

        if (!exclusive) {
            // SHARED MODE MUST USE THE MIX FORMAT. The mixer owns the rate and the layout, so a
            // request for anything else is not a preference that can be honoured — it is a format
            // the client is not allowed to ask for.
            chosenFormat = mixFormat;
        } else {
            // Exclusive mode has no mixer to convert anything, so a format is either what the
            // hardware does or it is refused. Propose, in the order a guitar amp should prefer
            // them: the deepest integer format first, because that is what interfaces actually run,
            // then float, then narrower.
            const double rate =
                wantedRate > 0.0 ? wantedRate : static_cast<double>(mixFormat->nSamplesPerSec);
            struct Candidate {
                int bits;
                int validBits;
                bool isFloat;
            };
            const Candidate candidates[] = {
                {32, 32, false}, {32, 24, false}, {24, 24, false}, {32, 32, true}, {16, 16, false},
            };
            // The device's own channel count first, then plain stereo: an interface may insist on
            // presenting all of its inputs at once in exclusive mode.
            const int channelCounts[] = {static_cast<int>(mixFormat->nChannels), 2};
            for (const int channels : channelCounts) {
                if (channels < 1)
                    continue;
                for (const Candidate &candidate : candidates) {
                    fillFormat(chosen, channels, rate, candidate.bits, candidate.validBits,
                               candidate.isFloat);
                    if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &chosen.Format,
                                                  nullptr) == S_OK &&
                        pcmFormatOf(&chosen.Format) != 0) {
                        chosenFormat = &chosen.Format;
                        break;
                    }
                }
                if (chosenFormat)
                    break;
            }
            if (!chosenFormat) {
                why = "the device accepts no exclusive-mode format this build can carry";
                CoTaskMemFree(mixFormat);
                return false;
            }
        }

        const int pcm = pcmFormatOf(chosenFormat);
        if (pcm == 0) {
            char detail[160];
            std::snprintf(detail, sizeof(detail),
                          "the device's format (tag %u, %u bits) is not one this build converts",
                          static_cast<unsigned>(chosenFormat->wFormatTag),
                          static_cast<unsigned>(chosenFormat->wBitsPerSample));
            why = detail;
            CoTaskMemFree(mixFormat);
            return false;
        }

        // The period asked for. In exclusive event-driven mode the buffer duration and the
        // periodicity MUST be the same value, which is what makes each wake-up exactly one block.
        REFERENCE_TIME period = defaultPeriod;
        if (mSettings.blockSize > 0) {
            period = static_cast<REFERENCE_TIME>(kRefTimesPerSecond * mSettings.blockSize /
                                                 chosenFormat->nSamplesPerSec);
            if (exclusive && period < minimumPeriod)
                period = minimumPeriod;
        }

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        HRESULT init = E_FAIL;

        if (!exclusive) {
            // IAudioClient3's engine period, where it exists, is what makes shared mode usable for
            // an instrument rather than merely possible: it asks the audio engine for its smallest
            // period instead of accepting the default one. Windows 10 and later; older systems fall
            // through to the plain call below, which is not an error.
            IAudioClient3 *client3 = nullptr;
            if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3),
                                                 reinterpret_cast<void **>(&client3))) &&
                client3) {
                UINT32 defaultFrames = 0, fundamental = 0, minFrames = 0, maxFrames = 0;
                if (SUCCEEDED(client3->GetSharedModeEnginePeriod(
                        chosenFormat, &defaultFrames, &fundamental, &minFrames, &maxFrames))) {
                    UINT32 wanted = (mSettings.blockSize > 0)
                                        ? static_cast<UINT32>(mSettings.blockSize)
                                        : defaultFrames;
                    wanted = std::min(std::max(wanted, minFrames), maxFrames);
                    // The engine only accepts multiples of the fundamental period.
                    if (fundamental > 0) {
                        const UINT32 steps = (wanted - minFrames) / fundamental;
                        wanted = minFrames + steps * fundamental;
                        wanted = std::min(std::max(wanted, minFrames), maxFrames);
                    }
                    init =
                        client3->InitializeSharedAudioStream(flags, wanted, chosenFormat, nullptr);
                }
                client3->Release();
            }
        }

        if (FAILED(init)) {
            init = client->Initialize(
                exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED, flags,
                exclusive ? period : 0, exclusive ? period : 0, chosenFormat, nullptr);
        }

        if (init == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // NOT AN ERROR BUT A NEGOTIATION, and the platform documents the dance: ask the client
            // what size it would have used, work the period back out of it, throw the client away —
            // an Initialize that failed this way leaves it unusable — activate a fresh one, and try
            // again with the aligned figure.
            UINT32 alignedFrames = 0;
            if (SUCCEEDED(client->GetBufferSize(&alignedFrames)) && alignedFrames > 0) {
                period = static_cast<REFERENCE_TIME>(
                    kRefTimesPerSecond * alignedFrames / chosenFormat->nSamplesPerSec + 0.5);
                client->Release();
                clientSlot = nullptr;
                IAudioClient *replacement = nullptr;
                if (SUCCEEDED(static_cast<IMMDevice *>(device)->Activate(
                        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                        reinterpret_cast<void **>(&replacement))) &&
                    replacement) {
                    clientSlot = replacement;
                    client = replacement;
                    init = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, period, period,
                                              chosenFormat, nullptr);
                }
            }
        }

        CoTaskMemFree(mixFormat);
        // chosenFormat may have pointed INTO mixFormat, so nothing may read it past here.
        chosenFormat = nullptr;

        if (FAILED(init)) {
            char detail[160];
            std::snprintf(detail, sizeof(detail), "%s could not be initialised (0x%08lx)",
                          capture_ ? "the input" : "the output", static_cast<unsigned long>(init));
            why = detail;
            return false;
        }

        client = static_cast<IAudioClient *>(clientSlot);
        UINT32 frames = 0;
        if (FAILED(client->GetBufferSize(&frames)) || frames == 0) {
            why = "the device would not report its buffer size";
            return false;
        }

        out.format = pcm;
        out.channels = chosen.Format.nChannels ? chosen.Format.nChannels : 0;
        out.frames = static_cast<int>(frames);
        // Re-read rather than assumed: shared mode used the mix format, and exclusive may have
        // landed on a candidate with a different channel count than the first one tried.
        WAVEFORMATEX *actual = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&actual)) && actual) {
            if (!exclusive) {
                out.channels = actual->nChannels;
                out.rate = static_cast<double>(actual->nSamplesPerSec);
            }
            CoTaskMemFree(actual);
        }
        if (out.rate <= 0.0)
            out.rate = static_cast<double>(chosen.Format.nSamplesPerSec);
        if (out.channels <= 0)
            out.channels = 2;
        return true;
    };

    // EXCLUSIVE FIRST, THEN SHARED. Both streams have to end up in the same mode: a shared render
    // beside an exclusive capture would be two different negotiations of the same rate, and the
    // ring between them assumes one.
    Negotiated captureOut;
    Negotiated renderOut;
    bool exclusive = mSettings.exclusive;
    std::string why;
    bool ready = false;

    for (int attempt = 0; attempt < 2 && !ready; ++attempt) {
        captureOut = Negotiated();
        renderOut = Negotiated();
        why.clear();
        const double wantedRate = exclusive ? mSettings.sampleRate : 0.0;
        if (!exclusive && mSettings.sampleRate > 0.0)
            std::fprintf(stderr, "namp-rack: shared mode uses the mixer's rate, not %.0f Hz\n",
                         mSettings.sampleRate);

        if (initialiseStream(false, mRenderClient, mRenderDevice, exclusive, wantedRate, renderOut,
                             why) &&
            initialiseStream(true, mCaptureClient, mCaptureDevice, exclusive, renderOut.rate,
                             captureOut, why)) {
            ready = true;
            break;
        }

        if (exclusive) {
            // The usual reasons are another application holding the device and a format the
            // hardware will not take unconverted. Both are ordinary, and shared mode is what the
            // fallback is for — but the demotion is SAID, because it is a latency difference the
            // player will feel and should be able to explain.
            std::fprintf(
                stderr, "namp-rack: exclusive mode was refused (%s); falling back to shared mode\n",
                why.empty() ? "no reason given" : why.c_str());
            exclusive = false;
            // Both clients are in an unknown state after a failed Initialize, so they are thrown
            // away and re-activated rather than reused.
            releaseInterface<IAudioClient>(mCaptureClient);
            releaseInterface<IAudioClient>(mRenderClient);
            IAudioClient *again = nullptr;
            if (SUCCEEDED(static_cast<IMMDevice *>(mCaptureDevice)
                              ->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void **>(&again))))
                mCaptureClient = again;
            again = nullptr;
            if (SUCCEEDED(static_cast<IMMDevice *>(mRenderDevice)
                              ->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void **>(&again))))
                mRenderClient = again;
            continue;
        }
        break;
    }

    if (!ready) {
        error = why.empty() ? "the device could not be initialised" : why;
        return false;
    }

    if (captureOut.rate != renderOut.rate) {
        // Two endpoints running at different rates cannot be joined by a ring — that needs a
        // resampler, and R16 says why there is not one. Refused with both numbers named, which is
        // what tells the user what to change.
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "the input is at %.0f Hz and the output at %.0f Hz; set both the same in "
                      "Windows' own sound settings",
                      captureOut.rate, renderOut.rate);
        error = detail;
        return false;
    }
    if (mSettings.inputChannel < 0 || mSettings.inputChannel >= captureOut.channels) {
        std::fprintf(stderr, "namp-rack: the input has %d channel(s); using the first\n",
                     captureOut.channels);
        mSettings.inputChannel = 0;
    }
    if (renderOut.channels < 2) {
        error = "the output device is mono, and the amp is stereo out";
        return false;
    }

    mOpenedExclusive = exclusive;
    mCaptureFormat = captureOut.format;
    mCaptureChannels = captureOut.channels;
    mRenderFormat = renderOut.format;
    mRenderChannels = renderOut.channels;
    mRenderFrames = renderOut.frames;
    mSampleRate = renderOut.rate;

    // THE BLOCK IS THE RENDER ENDPOINT'S WHOLE BUFFER, which is the most a single wake-up can ask
    // for. In exclusive event-driven mode that IS the period; in shared mode it is more, and each
    // wake-up asks for as much as the padding allows, bounded by this.
    mBlockSize.store(mRenderFrames, std::memory_order_relaxed);

    // ---- the events -------------------------------------------------------
    mRenderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!mRenderEvent) {
        error = "the render event could not be created";
        return false;
    }
    if (FAILED(static_cast<IAudioClient *>(mRenderClient)
                   ->SetEventHandle(static_cast<HANDLE>(mRenderEvent)))) {
        error = "the output device would not take an event handle";
        return false;
    }
    // The capture stream is event-driven too, because AUDCLNT_STREAMFLAGS_EVENTCALLBACK obliges a
    // handle — but nothing ever waits on it. Render is the master and capture is drained from the
    // render event, so this handle exists to satisfy the contract and for no other reason.
    mCaptureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!mCaptureEvent) {
        error = "the capture event could not be created";
        return false;
    }
    if (FAILED(static_cast<IAudioClient *>(mCaptureClient)
                   ->SetEventHandle(static_cast<HANDLE>(mCaptureEvent)))) {
        error = "the input device would not take an event handle";
        return false;
    }

    IAudioCaptureClient *capturePort = nullptr;
    if (FAILED(static_cast<IAudioClient *>(mCaptureClient)
                   ->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void **>(&capturePort))) ||
        !capturePort) {
        error = "the input device would not hand over a capture port";
        return false;
    }
    mCapture = capturePort;

    IAudioRenderClient *renderPort = nullptr;
    if (FAILED(static_cast<IAudioClient *>(mRenderClient)
                   ->GetService(__uuidof(IAudioRenderClient),
                                reinterpret_cast<void **>(&renderPort))) ||
        !renderPort) {
        error = "the output device would not hand over a render port";
        return false;
    }
    mRender = renderPort;

    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(static_cast<IAudioClient *>(mRenderClient)->GetStreamLatency(&latency)))
        std::printf("namp-rack: output stream latency %.1f ms\n",
                    1000.0 * static_cast<double>(latency) / kRefTimesPerSecond);
    return true;
}

//------------------------------------------------------------------------
// The audio thread. Every interface released once, and the events with them.
void WasapiBackend::releaseDevices()
{
    releaseInterface<IAudioRenderClient>(mRender);
    releaseInterface<IAudioCaptureClient>(mCapture);
    releaseInterface<IAudioClient>(mRenderClient);
    releaseInterface<IAudioClient>(mCaptureClient);
    releaseInterface<IMMDevice>(mRenderDevice);
    releaseInterface<IMMDevice>(mCaptureDevice);
    releaseInterface<IMMDeviceEnumerator>(mEnumerator);
    if (mRenderEvent) {
        CloseHandle(static_cast<HANDLE>(mRenderEvent));
        mRenderEvent = nullptr;
    }
    if (mCaptureEvent) {
        CloseHandle(static_cast<HANDLE>(mCaptureEvent));
        mCaptureEvent = nullptr;
    }
}

//------------------------------------------------------------------------
// The audio thread. Everything WASAPI has captured since the last block, into the ring.
//
// AND THE DRIFT CORRECTION, which is the whole of R16 in one place. Above the high-water mark the
// oldest frames are discarded: the ring is a latency the player hears, so letting it fill is worse
// than dropping. It cannot happen at all with both endpoints on one device.
void WasapiBackend::drainCapture()
{
    IAudioCaptureClient *capture = static_cast<IAudioCaptureClient *>(mCapture);
    if (!capture || mRingCapacity <= 0)
        return;

    const int channels = mCaptureChannels > 0 ? mCaptureChannels : 1;
    const int bytes = pcmBytesPerSample(mCaptureFormat);

    for (;;) {
        UINT32 packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0)
            break;

        BYTE *data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        const HRESULT hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY)
            break;
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                mDeviceReset.store(true, std::memory_order_release);
            break;
        }
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            // The system lost input between this packet and the last. Counted rather than smoothed:
            // it is exactly the kind of thing the live gate is asking about.
            mCaptureGlitches.fetch_add(1, std::memory_order_relaxed);
        }

        for (UINT32 i = 0; i < frames; ++i) {
            if (mRingFill >= mRingCapacity) {
                // The ring is completely full, which means the reader has stopped — suspended, or
                // stalled. Drop the newest rather than the oldest here: what is already in the ring
                // is what the amp is about to play.
                break;
            }
            float sample = 0.0f;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data) {
                const unsigned char *src =
                    data + static_cast<size_t>(i) * static_cast<size_t>(channels * bytes) +
                    static_cast<size_t>(mSettings.inputChannel * bytes);
                pcmToFloat(src, mCaptureFormat, &sample, 1, channels);
            }
            mCaptureRing[static_cast<size_t>(mRingWritePos)] = sample;
            mRingWritePos = (mRingWritePos + 1) % mRingCapacity;
            ++mRingFill;
        }
        capture->ReleaseBuffer(frames);
    }

    // The high-water mark: one block of target fill plus one of tolerance. Past it, the two clocks
    // have drifted apart far enough that the extra frames are pure latency, so the OLDEST go.
    const int block = mBlockSize.load(std::memory_order_relaxed);
    const int highWater = block * 2;
    if (mRingFill > highWater) {
        const int excess = mRingFill - block;
        mRingReadPos = (mRingReadPos + excess) % mRingCapacity;
        mRingFill -= excess;
        mDriftCorrections.fetch_add(1, std::memory_order_relaxed);
    }
}

//------------------------------------------------------------------------
// The audio thread. `frames` from the ring, padding the front with silence if there are not that
// many. Returns how many were real, which is what tells the caller an underrun happened.
//
// THE PADDING GOES IN FRONT, not behind: the samples that ARE there are the most recent, and
// putting the gap before them keeps the input continuous from that point rather than tearing it in
// two.
int WasapiBackend::takeFromRing(float *dst, int frames)
{
    const int available = std::min(mRingFill, frames);
    const int missing = frames - available;
    if (missing > 0)
        std::memset(dst, 0, static_cast<size_t>(missing) * sizeof(float));
    for (int i = 0; i < available; ++i) {
        dst[missing + i] = mCaptureRing[static_cast<size_t>(mRingReadPos)];
        mRingReadPos = (mRingReadPos + 1) % mRingCapacity;
    }
    mRingFill -= available;
    return available;
}

//------------------------------------------------------------------------
// The audio thread, once per render event. Nothing here allocates, locks, or logs.
void WasapiBackend::runBlock()
{
    const NAMp::host::RtScope rtScope;

    IAudioClient *renderClient = static_cast<IAudioClient *>(mRenderClient);
    IAudioRenderClient *render = static_cast<IAudioRenderClient *>(mRender);
    if (!renderClient || !render)
        return;

    // How much of the render buffer is free. In exclusive event-driven mode this is the whole
    // buffer every time; in shared mode it is whatever the engine has consumed.
    UINT32 padding = 0;
    const HRESULT paddingResult = renderClient->GetCurrentPadding(&padding);
    if (FAILED(paddingResult)) {
        if (paddingResult == AUDCLNT_E_DEVICE_INVALIDATED)
            mDeviceReset.store(true, std::memory_order_release);
        mCycle.fetch_add(1, std::memory_order_release);
        return;
    }
    const int block = mBlockSize.load(std::memory_order_relaxed);
    int frames = mRenderFrames - static_cast<int>(padding);
    frames = std::min(frames, block);
    if (frames <= 0) {
        // Nothing to fill yet. Not an error and not a dropout — the event fires again.
        mCycle.fetch_add(1, std::memory_order_release);
        return;
    }

    // Capture is drained even when suspended, so the ring does not overflow while the UI
    // reconfigures and so the input is current again the moment processing resumes.
    drainCapture();

    BYTE *out = nullptr;
    const HRESULT got = render->GetBuffer(static_cast<UINT32>(frames), &out);
    if (FAILED(got) || !out) {
        if (got == AUDCLNT_E_DEVICE_INVALIDATED)
            mDeviceReset.store(true, std::memory_order_release);
        mCycle.fetch_add(1, std::memory_order_release);
        return;
    }

    if (!mProcessor || mSuspended.load(std::memory_order_acquire)) {
        // AUDCLNT_BUFFERFLAGS_SILENT rather than a memset: it tells the engine the buffer is silent
        // and saves it reading what we did not write.
        render->ReleaseBuffer(static_cast<UINT32>(frames), AUDCLNT_BUFFERFLAGS_SILENT);
        mCycle.fetch_add(1, std::memory_order_release);
        return;
    }

    // THE START-UP TRANSIENT IS NOT AN UNDERRUN. Capture and render are started a few milliseconds
    // apart, so the first wake-up or two have nothing recorded yet. Until the ring has held a whole
    // block once, the output is silence and nothing is counted; after that a short read is real.
    if (!mPrimed) {
        if (mRingFill >= frames) {
            mPrimed = true;
        } else {
            render->ReleaseBuffer(static_cast<UINT32>(frames), AUDCLNT_BUFFERFLAGS_SILENT);
            mCycle.fetch_add(1, std::memory_order_release);
            return;
        }
    }

    const int real = takeFromRing(mInputFloat.data(), frames);
    if (real < frames) {
        // The ring could not fill a block, so part of what the amp just heard was silence this
        // process invented — the reader has got ahead of the writer. With both endpoints on one
        // device that cannot happen once primed, because one device has one clock; with two it is
        // the drift running the other way from the correction in drainCapture().
        mUnderruns.fetch_add(1, std::memory_order_relaxed);
    }

    mInputChanges.clearQueue();
    mOutputChanges.clearQueue();
    mEvents.clear();
    drainParameterRing();
    readMidi();
    mProcessData.inputEvents = &mEvents;

    float *outL = mOutputFloat[0].data();
    float *outR = mOutputFloat[1].data();
    const float *in = mInputFloat.data();

    if (mChain)
        mChain->beginBlock(mRackMidi, mRackMidiCount);

    // The same chunk loop, for the same reason: the processor is set up for whatever size it was
    // last given, and handing a model more than it was Reset() with would silently wipe its
    // convolution history.
    const int chunk = block;
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
        mInputChanges.clearQueue();
        mOutputChanges.clearQueue();
        mEvents.clear();
        mProcessData.inputEvents = nullptr;
        done += n;
    }

    // Into the interleaved render buffer. Channels past the first two are silenced rather than left
    // holding whatever the engine last put there — a multi-channel output would otherwise play the
    // previous block on its surround channels.
    const int channels = mRenderChannels > 0 ? mRenderChannels : 2;
    const int bytes = pcmBytesPerSample(mRenderFormat);
    if (channels > 2)
        std::memset(out, 0, static_cast<size_t>(frames * channels * bytes));
    floatToPcm(mOutputFloat[0].data(), out, mRenderFormat, frames, channels);
    floatToPcm(mOutputFloat[1].data(), out + static_cast<size_t>(bytes), mRenderFormat, frames,
               channels);

    render->ReleaseBuffer(static_cast<UINT32>(frames), 0);
    mCycle.fetch_add(1, std::memory_order_release);
}

} // namespace Rations
