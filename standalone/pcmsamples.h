// PCM sample-format conversion for BOTH Windows audio backends, and the one piece of the Windows
// audio path that can be proved without Windows.
//
// WHY THIS IS ITS OWN HEADER, and why it includes nothing of either audio API's. An ASIO driver
// hands out its buffers in whatever format the hardware uses — ASIOGetChannelInfo reports it per
// channel, and it is emphatically NOT always float — and WASAPI in exclusive mode does exactly the
// same thing through a WAVEFORMATEXTENSIBLE. The layouts the two describe are the SAME small set of
// PCM layouts, so there is one conversion here and both backends use it: two copies of this
// arithmetic would be two chances to get it wrong, and only one of them could be tested. Every one
// of those formats has to be read into, and written out of, the float buffers the amp works in, and
// getting the scaling wrong by one bit or the sign wrong on the negative side is a distortion
// nobody notices in the code and everybody hears.
//
// A driver is not available on the development machine and cannot be, so the ONLY way to check this
// arithmetic before it reaches a user is to check it offline against values written out by hand.
// That is what tools/namp_audiocheck.cpp does, and it is why the format arrives here as a plain int
// rather than as an ASIOSampleType: this header compiles anywhere, including on the machine the
// gate runs on. asiobackend.cpp static_asserts that these constants are the SDK's own, so the two
// cannot drift apart.
//
// WHAT IS SUPPORTED, AND WHAT IS REFUSED. The little-endian integer and float formats, which are
// what x86 hardware reports, plus the four right-aligned 32-bit variants — which is also exactly
// what WASAPI expresses as wBitsPerSample 32 with wValidBitsPerSample 16, 18, 20 or 24. The
// big-endian ones are REFUSED by name rather than guessed at: they exist for big-endian hardware,
// this product is x86-64 Windows only, and a byte-swapping path that no machine here can exercise
// is a path that would be wrong for as long as it went unnoticed. DSD is refused for the same
// reason and a better one — it is a one-bit stream, not samples. A refused format is a named error
// at open() and a backend that does not start, never silence and never noise.
//
// ONE PCM BUFFER MAY BE INTERLEAVED, AND THAT IS WHY EVERY FUNCTION TAKES A STRIDE. ASIO hands out
// a separate buffer per channel, so its stride is 1. WASAPI hands out ONE buffer with the channels
// interleaved frame by frame, so its stride is the stream's channel count and the pointer is offset
// to the channel wanted. The float side is always contiguous, because that is what the amp works
// in. Passing the wrong stride is not subtle — it is the left channel played at half speed — but it
// is exactly the kind of thing a conversion written twice would get right once, which is the other
// reason there is only one of these.
//
// THE SCALING RULE, chosen once and applied everywhere. Signed n-bit integers run from -2^(n-1) to
// 2^(n-1)-1: the negative side has one more step than the positive one. Dividing by 2^(n-1) is
// therefore the only factor that maps the whole range into [-1, 1) without clipping, and it is what
// every audio API's own conversion uses. On the way out the same factor is used and the result is
// CLAMPED before rounding, because the amp can and does produce values past ±1 — a hosted plug-in
// is under no obligation to stay inside it — and an unclamped conversion wraps, which turns a loud
// moment into a full-scale square wave.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Rations
{

// The PCM layouts the Windows backends handle.
//
// THE NUMBERS ARE ASIO'S, and that is deliberate rather than accidental: one of the two callers
// gets its format as an ASIOSampleType and can pass it straight through, while the other translates
// a WAVEFORMATEXTENSIBLE into the same values. Named here rather than included, so this header
// needs no ASIO SDK and can be compiled and proved on a machine that has neither SDK nor driver;
// asiobackend.cpp static_asserts every one of them against the SDK's own enum, which is what keeps
// the two from drifting.
enum PcmSampleFormat {
    kPcmInt16LSB = 16,
    kPcmInt24LSB = 17,
    kPcmInt32LSB = 18,
    kPcmFloat32LSB = 19,
    kPcmFloat64LSB = 20,
    // 32-bit containers holding fewer significant bits, right-aligned. The name says how many bits
    // are used, so the scaling factor differs per variant while the container size does not.
    kPcmInt32LSB16 = 24,
    kPcmInt32LSB18 = 25,
    kPcmInt32LSB20 = 26,
    kPcmInt32LSB24 = 27,
};

// True for a format the two functions below can actually carry. Everything else is refused at
// open() with the number printed, so a report from a machine nobody here owns names the format.
inline bool pcmFormatSupported(int format)
{
    switch (format) {
        case kPcmInt16LSB:
        case kPcmInt24LSB:
        case kPcmInt32LSB:
        case kPcmFloat32LSB:
        case kPcmFloat64LSB:
        case kPcmInt32LSB16:
        case kPcmInt32LSB18:
        case kPcmInt32LSB20:
        case kPcmInt32LSB24:
            return true;
        default:
            return false;
    }
}

// Bytes one sample occupies in a driver buffer of this format. Needed to walk the buffer, and it is
// NOT derivable from the bit count: Int32LSB16 carries 16 significant bits in 4 bytes.
inline int pcmBytesPerSample(int format)
{
    switch (format) {
        case kPcmInt16LSB:
            return 2;
        case kPcmInt24LSB:
            return 3;
        case kPcmFloat64LSB:
            return 8;
        default:
            return 4;
    }
}

namespace detail
{

// The divisor that maps this format's full-scale integer to 1.0. A float format has none.
inline double pcmIntScale(int format)
{
    switch (format) {
        case kPcmInt16LSB:
        case kPcmInt32LSB16:
            return 32768.0; // 2^15
        case kPcmInt24LSB:
        case kPcmInt32LSB24:
            return 8388608.0; // 2^23
        case kPcmInt32LSB18:
            return 131072.0; // 2^17
        case kPcmInt32LSB20:
            return 524288.0; // 2^19
        case kPcmInt32LSB:
            return 2147483648.0; // 2^31
        default:
            return 0.0;
    }
}

// One 24-bit little-endian sample as a signed value. Read byte by byte rather than through a cast:
// a 3-byte sample is not 4-byte aligned in the driver's buffer, and reading it as an int32 would be
// both an unaligned load and three bytes past the last sample.
inline int32_t read24(const unsigned char *p)
{
    const uint32_t raw = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                         (static_cast<uint32_t>(p[2]) << 16);
    // Sign-extend from 24 bits. The shift-pair form is used rather than a conditional because it is
    // the one that is correct for every value including 0x800000 itself.
    return static_cast<int32_t>(raw << 8) >> 8;
}

inline void write24(unsigned char *p, int32_t v)
{
    p[0] = static_cast<unsigned char>(v & 0xff);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xff);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xff);
}

// A float sample as an integer in [lo, hi], clamped and rounded. See the scaling note at the top:
// the clamp is what keeps a hot signal from wrapping to the opposite polarity.
//
// IT IS ALSO THE LAST PLACE A NaN CAN BE STOPPED, and the structure below is what stops it. Casting
// a NaN or an out-of-range double to int32_t is undefined behaviour, and what it produces in
// practice is INT32_MIN — full-scale negative, on every sample, which is the loudest sound the
// hardware can make. So no comparison here assumes the value is a number: the in-range case is
// tested for POSITIVELY with two ordered comparisons, and a NaN, which satisfies neither, falls
// through to silence. The end-of-chain clamp in the host layer is the first line of this defence
// and this is the last, at the point where a float finally becomes a voltage.
//
// THIS FILE MUST NOT BE COMPILED WITH -ffast-math, for the same reason chainengine.cpp must not:
// -ffinite-math-only tells the compiler NaN cannot happen, which licenses it to treat the
// fallthrough as unreachable. The build asserts the flag's absence on the target that compiles
// this rather than leaving it to review.
inline int32_t floatToInt(float v, double scale, int32_t lo, int32_t hi)
{
    const double scaled = static_cast<double>(v) * scale;
    const double loD = static_cast<double>(lo);
    const double hiD = static_cast<double>(hi);
    if (scaled > loD && scaled < hiD) {
        // Rounded half away from zero, which is what every integer audio format expects and what a
        // plain cast — truncation toward zero — would get wrong on every negative sample.
        const double rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
        // Rounding can step onto the boundary itself, which is in range for lo but not for hi.
        if (rounded <= loD)
            return lo;
        if (rounded >= hiD)
            return hi;
        return static_cast<int32_t>(rounded);
    }
    if (scaled <= loD)
        return lo;
    if (scaled >= hiD)
        return hi;
    return 0; // not a number: no ordered comparison above was true
}

// The inclusive range a format's integer samples occupy. Computed rather than cast from the scale,
// because static_cast<int32_t>(2147483648.0) — which is what the full-scale 32-bit format's divisor
// is — is undefined behaviour in itself.
inline void pcmIntRange(double divisor, int32_t &lo, int32_t &hi)
{
    if (divisor >= 2147483648.0) {
        lo = INT32_MIN;
        hi = INT32_MAX;
        return;
    }
    lo = -static_cast<int32_t>(divisor);
    hi = static_cast<int32_t>(divisor) - 1;
}

} // namespace detail

// One channel of a device buffer into floats. `src` points at this channel's first sample and
// `stride` says how many samples to skip between frames — 1 for a per-channel buffer, the channel
// count for an interleaved one. Nothing is allocated and nothing is locked: this runs on the audio
// thread.
inline void pcmToFloat(const void *src, int format, float *dst, int frames, int stride = 1)
{
    const unsigned char *in = static_cast<const unsigned char *>(src);
    const size_t step =
        static_cast<size_t>(pcmBytesPerSample(format)) * static_cast<size_t>(stride);
    switch (format) {
        case kPcmFloat32LSB:
            if (stride == 1) {
                // Already the format the amp works in. memcpy rather than a loop, and NOT a pointer
                // alias: the device's buffer is not guaranteed to be aligned for float access, and
                // on the platforms where that matters it is a fault rather than a slowdown.
                std::memcpy(dst, in, static_cast<size_t>(frames) * sizeof(float));
                return;
            }
            for (int i = 0; i < frames; ++i)
                std::memcpy(&dst[i], in + static_cast<size_t>(i) * step, sizeof(float));
            return;
        case kPcmFloat64LSB: {
            for (int i = 0; i < frames; ++i) {
                double v = 0.0;
                std::memcpy(&v, in + static_cast<size_t>(i) * step, sizeof(double));
                dst[i] = static_cast<float>(v);
            }
            return;
        }
        case kPcmInt16LSB: {
            const double scale = 1.0 / detail::pcmIntScale(format);
            for (int i = 0; i < frames; ++i) {
                int16_t v = 0;
                std::memcpy(&v, in + static_cast<size_t>(i) * step, sizeof(int16_t));
                dst[i] = static_cast<float>(static_cast<double>(v) * scale);
            }
            return;
        }
        case kPcmInt24LSB: {
            const double scale = 1.0 / detail::pcmIntScale(format);
            for (int i = 0; i < frames; ++i)
                dst[i] = static_cast<float>(
                    static_cast<double>(detail::read24(in + static_cast<size_t>(i) * step)) *
                    scale);
            return;
        }
        default: {
            // The 32-bit container family: full-scale Int32LSB and the four right-aligned variants.
            // They differ only in the divisor.
            const double divisor = detail::pcmIntScale(format);
            if (divisor <= 0.0) {
                // An unsupported format must never reach here — open() refuses them — but silence
                // is the only safe thing to produce if one ever did.
                std::memset(dst, 0, static_cast<size_t>(frames) * sizeof(float));
                return;
            }
            const double scale = 1.0 / divisor;
            for (int i = 0; i < frames; ++i) {
                int32_t v = 0;
                std::memcpy(&v, in + static_cast<size_t>(i) * step, sizeof(int32_t));
                dst[i] = static_cast<float>(static_cast<double>(v) * scale);
            }
            return;
        }
    }
}

// Floats into one channel of a device buffer, the exact inverse of the above.
inline void floatToPcm(const float *src, void *dst, int format, int frames, int stride = 1)
{
    unsigned char *out = static_cast<unsigned char *>(dst);
    const size_t step =
        static_cast<size_t>(pcmBytesPerSample(format)) * static_cast<size_t>(stride);
    switch (format) {
        case kPcmFloat32LSB:
            if (stride == 1) {
                std::memcpy(out, src, static_cast<size_t>(frames) * sizeof(float));
                return;
            }
            for (int i = 0; i < frames; ++i)
                std::memcpy(out + static_cast<size_t>(i) * step, &src[i], sizeof(float));
            return;
        case kPcmFloat64LSB: {
            for (int i = 0; i < frames; ++i) {
                const double v = static_cast<double>(src[i]);
                std::memcpy(out + static_cast<size_t>(i) * step, &v, sizeof(double));
            }
            return;
        }
        case kPcmInt16LSB: {
            const double scale = detail::pcmIntScale(format);
            for (int i = 0; i < frames; ++i) {
                const int16_t v =
                    static_cast<int16_t>(detail::floatToInt(src[i], scale, -32768, 32767));
                std::memcpy(out + static_cast<size_t>(i) * step, &v, sizeof(int16_t));
            }
            return;
        }
        case kPcmInt24LSB: {
            const double scale = detail::pcmIntScale(format);
            for (int i = 0; i < frames; ++i)
                detail::write24(out + static_cast<size_t>(i) * step,
                                detail::floatToInt(src[i], scale, -8388608, 8388607));
            return;
        }
        default: {
            const double divisor = detail::pcmIntScale(format);
            if (divisor <= 0.0) {
                // Per frame, not one memset: with a stride, a contiguous clear would silence the
                // channels either side of this one as well. The float side above has no such
                // problem, because the float side is always contiguous.
                const int32_t zero = 0;
                for (int i = 0; i < frames; ++i)
                    std::memcpy(out + static_cast<size_t>(i) * step, &zero, sizeof(int32_t));
                return;
            }
            // The right-aligned variants are clamped to THEIR OWN range, not to int32's: writing a
            // full-scale int32 into an Int32LSB16 channel is 65536 times too loud, and what the
            // hardware does with the excess is its own business.
            int32_t lo = 0;
            int32_t hi = 0;
            detail::pcmIntRange(divisor, lo, hi);
            for (int i = 0; i < frames; ++i) {
                const int32_t v = detail::floatToInt(src[i], divisor, lo, hi);
                std::memcpy(out + static_cast<size_t>(i) * step, &v, sizeof(int32_t));
            }
            return;
        }
    }
}

} // namespace Rations
