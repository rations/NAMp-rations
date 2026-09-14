// WinMmMidiIn implementation. See winmmmidi.h for why MIDI is a third API here, why only one device
// is opened, and why the callback does nothing but store.

#include "winmmmidi.h"

#include <windows.h>
// After windows.h, which mmsystem.h expects to have been included first.
#include <mmsystem.h>

#include <cstdio>
#include <filesystem>

namespace Rations
{

namespace
{

// A short MIDI message arrives packed into dwParam1: status in the low byte, then the two data
// bytes. Read out by hand rather than through a union, because the layout is documented and a union
// would only hide which byte is which.
inline uint8_t statusOf(uintptr_t packed)
{
    return static_cast<uint8_t>(packed & 0xff);
}
inline uint8_t data1Of(uintptr_t packed)
{
    return static_cast<uint8_t>((packed >> 8) & 0x7f);
}
inline uint8_t data2Of(uintptr_t packed)
{
    return static_cast<uint8_t>((packed >> 16) & 0x7f);
}

//------------------------------------------------------------------------
// The OS's callback, on a thread WinMM owns, at interrupt time.
//
// A FREE FUNCTION rather than a static member, so winmmmidi.h needs no windows.h and nothing that
// includes it inherits one. The instance arrives as dwInstance, which is what midiInOpen's contract
// provides for and is the reason this class needs no global.
//
// MIM_DATA and nothing else. MIM_LONGDATA is a system-exclusive buffer, which requires a prepared
// header this class never supplies and carries nothing a footswitch or a controller pedal sends;
// MIM_ERROR and MIM_LONGERROR are malformed input, which is dropped rather than reported from a
// context that may not log. MIM_OPEN and MIM_CLOSE are lifecycle notices with nothing to do.
void CALLBACK midiInCallback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR)
{
    if (message != MIM_DATA)
        return;
    WinMmMidiIn *self = reinterpret_cast<WinMmMidiIn *>(instance);
    if (!self)
        return;
    self->push(statusOf(param1), data1Of(param1), data2Of(param1));
}

} // namespace

//------------------------------------------------------------------------
WinMmMidiIn::~WinMmMidiIn()
{
    close();
}

//------------------------------------------------------------------------
int WinMmMidiIn::deviceCount()
{
    return static_cast<int>(midiInGetNumDevs());
}

//------------------------------------------------------------------------
bool WinMmMidiIn::deviceName(int index, std::string &out)
{
    out.clear();
    if (index < 0 || index >= deviceCount())
        return false;

    MIDIINCAPSW caps = {};
    if (midiInGetDevCapsW(static_cast<UINT_PTR>(index), &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        return false;

    // szPname is a fixed WCHAR[32] and Windows TRUNCATES a longer name into it — a documented limit
    // of this API, not something this code can widen. It is also not guaranteed to be terminated
    // when it fills the array, so the length is bounded here rather than trusted.
    size_t length = 0;
    while (length < MAXPNAMELEN && caps.szPname[length] != L'\0')
        ++length;

    // std::filesystem::path is the wide-to-UTF-8 conversion, as it is everywhere else in this tree:
    // libstdc++ uses UTF-8 for a path's narrow form whatever the active code page, so a device
    // named in a script the code page cannot express still reaches the picker intact. It throws on
    // a sequence it cannot convert, and a device with an unreadable name should be listed by number
    // rather than not listed at all.
    try {
        out = std::filesystem::path(std::wstring(caps.szPname, length)).string();
    } catch (...) {
        out.clear();
    }
    if (out.empty()) {
        char fallback[32];
        std::snprintf(fallback, sizeof(fallback), "MIDI input %d", index);
        out = fallback;
    }
    return true;
}

//------------------------------------------------------------------------
bool WinMmMidiIn::open(int deviceIndex)
{
    close();
    if (deviceIndex == kNoDevice)
        return false; // not an error: no footswitch is the default state
    if (deviceIndex < 0 || deviceIndex >= deviceCount()) {
        std::fprintf(stderr, "namp-rack: MIDI input %d does not exist - no footswitch\n",
                     deviceIndex);
        return false;
    }

    HMIDIIN handle = nullptr;
    const MMRESULT result = midiInOpen(&handle, static_cast<UINT>(deviceIndex),
                                       reinterpret_cast<DWORD_PTR>(&midiInCallback),
                                       reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR || !handle) {
        // MIDIERR_NODEVICE and MMSYSERR_ALLOCATED are the two a user will actually meet: the device
        // was unplugged since the list was drawn, or another program has it open. Neither is fatal
        // to the amp, so both are said once and carried on from.
        std::fprintf(stderr,
                     "namp-rack: cannot open MIDI input %d (error %u) - no footswitch. Another "
                     "program may have it open.\n",
                     deviceIndex, static_cast<unsigned>(result));
        return false;
    }

    // The ring is emptied BEFORE the device is started, not after: once midiInStart returns, the
    // callback may be running, and resetting the indices under it would lose or duplicate messages.
    mRead.store(0, std::memory_order_relaxed);
    mWrite.store(0, std::memory_order_relaxed);
    mDropped.store(0, std::memory_order_relaxed);

    if (midiInStart(handle) != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "namp-rack: cannot start MIDI input %d - no footswitch\n",
                     deviceIndex);
        midiInClose(handle);
        return false;
    }

    mHandle = handle;
    mDeviceIndex = deviceIndex;

    std::string name;
    deviceName(deviceIndex, name);
    std::printf("namp-rack: MIDI in: %s\n", name.c_str());
    return true;
}

//------------------------------------------------------------------------
void WinMmMidiIn::close()
{
    if (!mHandle) {
        mDeviceIndex = kNoDevice;
        return;
    }
    HMIDIIN handle = static_cast<HMIDIIN>(mHandle);
    // THE ORDER IS LOAD-BEARING and it is the reason this is not just midiInClose. midiInClose
    // returns MIDIERR_STILLPLAYING if the device is running, so stopping first is what makes the
    // close succeed at all; and midiInReset returns any buffer the driver still holds, which is
    // what guarantees the callback has finished before the handle is released. Closing a running
    // device leaves a thread calling into `this` after it has been destroyed.
    midiInStop(handle);
    midiInReset(handle);
    midiInClose(handle);
    mHandle = nullptr;
    mDeviceIndex = kNoDevice;
}

//------------------------------------------------------------------------
// The OS's MIDI thread. Two atomic stores, and nothing else may be added here — see the header on
// what a midiInOpen callback is permitted to do.
void WinMmMidiIn::push(uint8_t status, uint8_t data1, uint8_t data2)
{
    const uint32_t write = mWrite.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) & (kRingSize - 1);
    if (next == mRead.load(std::memory_order_acquire)) {
        mDropped.fetch_add(1, std::memory_order_relaxed);
        return; // full: drop rather than block a thread that must not block
    }
    mRing[write] = {status, data1, data2};
    mWrite.store(next, std::memory_order_release);
}

//------------------------------------------------------------------------
// The audio thread.
int WinMmMidiIn::drain(NAMp::host::RtMidiEvent *out, int max)
{
    if (!out || max <= 0)
        return 0;

    int count = 0;
    uint32_t read = mRead.load(std::memory_order_relaxed);
    const uint32_t write = mWrite.load(std::memory_order_acquire);
    while (read != write && count < max) {
        const Message &message = mRing[read];
        NAMp::host::RtMidiEvent &event = out[count++];
        // Frame 0 for every message. See the header: WinMM's stamp is a millisecond count on a
        // different clock from the audio device's, and converting it would be arithmetic across two
        // unrelated clocks.
        event.frame = 0;
        event.status = message.status;
        event.data1 = message.data1;
        event.data2 = message.data2;
        read = (read + 1) & (kRingSize - 1);
    }
    // Published even when the loop stopped on `max`: what was consumed is consumed, and the rest
    // stays for the next block rather than being lost.
    mRead.store(read, std::memory_order_release);
    return count;
}

} // namespace Rations
