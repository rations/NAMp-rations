// WinMmMidiIn — MIDI input on Windows, which neither audio API carries.
//
// JACK delivers MIDI through the same callback as the audio, on the same clock, stamped with the
// frame it arrived at. Windows has no such thing: ASIO carries audio only and so does WASAPI, so
// the footswitch comes in through a THIRD API on a thread the operating system owns, and the
// messages have to cross into the audio thread the way everything else crosses a thread boundary
// here — a fixed-capacity lock-free ring, drained at the top of the block.
//
// ONE DEVICE, CHOSEN, AND NONE BY DEFAULT. That is deliberate, and it is the same decision the JACK
// backend already makes when it registers a MIDI port and pointedly does not connect it: which of a
// machine's MIDI sources is the footswitch is not something to guess at, and connecting the wrong
// one would have a keyboard changing amp channels. The user picks it on the setup page and the
// choice persists. It also makes the ring a genuine SPSC queue rather than a nearly-one — WinMM
// calls the callback on a thread per open device, so opening several would put several writers on
// one ring.
//
// WHAT THE CALLBACK MAY DO IS SEVERELY LIMITED, and the limit is Microsoft's rather than this
// project's: a midiInOpen callback runs at interrupt time and the documentation permits only a
// short list of calls from it — EnterCriticalSection/LeaveCriticalSection, PostMessage, SetEvent,
// the timeGetTime family and a couple of MIDI-out calls. No allocation, no other system call, and
// above all no logging. Storing into a pre-allocated ring with two atomics calls nothing at all,
// which is why that is what happens here and why a full ring drops the message rather than growing.
//
// TIMESTAMPS ARE NOT USED, AND THAT IS A DECISION RATHER THAN AN OMISSION. WinMM stamps each
// message with milliseconds since midiInStart, which is a different clock from the audio device's
// and is quantised a hundred times more coarsely than a block. Converting it into a frame offset
// would be arithmetic on two unrelated clocks, drifting, and wrong in a way nobody could see. So
// every message lands at frame 0 of the block it was drained in — which is exactly what the JACK
// backend already does for the amp's own route, on the grounds that a footswitch inside one block
// is under three milliseconds and no one plays tighter than that.

#pragma once

#include "host/pluginref.h" // RtMidiEvent, the form the rack takes messages in

#include <atomic>
#include <cstdint>
#include <string>

namespace Rations
{

//------------------------------------------------------------------------
class WinMmMidiIn
{
public:
    // No device open. Opening one is the user's act, and a machine with no MIDI at all is an
    // ordinary state — the bat switches still change channel.
    static constexpr int kNoDevice = -1;

    ~WinMmMidiIn();

    // --- what the picker shows (main thread) ------------------------------
    // How many MIDI inputs the machine has, and what each is called. The name comes back as UTF-8
    // from the WIDE call: midiInGetDevCapsA would render it in the process's ANSI code page, and an
    // interface named in anything outside it would come out as question marks.
    static int deviceCount();
    static bool deviceName(int index, std::string &out);

    // --- lifetime (main thread) -------------------------------------------
    // Opens and starts the device. False means it could not be opened, which is NOT fatal to
    // anything: the reason is printed once and the amp plays on with no footswitch. Opening a
    // second time closes the first.
    bool open(int deviceIndex);
    void close();

    bool isOpen() const
    {
        return mHandle != nullptr;
    }
    int deviceIndex() const
    {
        return mDeviceIndex;
    }

    // --- the audio thread -------------------------------------------------
    // Takes up to `max` messages into `out` and returns how many. Never blocks, never allocates.
    // Called once at the top of each block, before anything is processed.
    int drain(NAMp::host::RtMidiEvent *out, int max);

    // Messages dropped because the ring was full since open(). Read from anywhere. A footswitch
    // cannot produce this; a controller sweeping every CC it has while the audio thread is stalled
    // can, and if it ever reads non-zero the size below is the thing to look at.
    uint32_t dropped() const
    {
        return mDropped.load(std::memory_order_relaxed);
    }

    // --- the OS's MIDI thread ---------------------------------------------
    // One short message into the ring. PUBLIC ONLY BECAUSE THE CALLBACK IS A FREE
    // FUNCTION in the implementation file, which is what keeps windows.h out of this
    // header; nothing else calls it. Two atomic stores and no system call, which is all
    // a midiInOpen callback is allowed to be.
    void push(uint8_t status, uint8_t data1, uint8_t data2);

private:
    // SPSC ring: written only by the OS's MIDI thread, read only by the audio thread. 1024 short
    // messages is far more than a block can carry from any real controller; the ring exists so the
    // two threads never wait for each other, not to buffer a performance.
    static constexpr uint32_t kRingSize = 1024; // power of two
    struct Message {
        uint8_t status;
        uint8_t data1;
        uint8_t data2;
    };
    Message mRing[kRingSize] = {};
    std::atomic<uint32_t> mWrite{0};
    std::atomic<uint32_t> mRead{0};
    std::atomic<uint32_t> mDropped{0};

    void *mHandle = nullptr; // HMIDIIN, kept as void* so this header pulls in no windows.h
    int mDeviceIndex = kNoDevice;
};

} // namespace Rations
