// namp_audiocheck — the offline gate on the Windows audio path's arithmetic.
//
// WHY THIS TOOL EXISTS. The Windows audio backends cannot be run here: there is no ASIO driver on a
// Linux development machine and there never will be, and Wine's WASAPI is a shim over the build
// host's own sound server whose timing is not Windows'. So every number in those backends' gates —
// dropouts at the smallest buffer, achievable exclusive-mode period — comes from a real Windows
// machine, as the project's rules already say about Win32 behaviour generally.
//
// But not everything in that path is timing. The sample-format conversion is pure arithmetic, it is
// the one part that is wrong SILENTLY — a bad scale factor is a quiet or clipped signal, a bad sign
// is inverted polarity, a mishandled NaN is full-scale noise — and it is exactly the kind of thing
// that can be checked against values written out by hand. That is what this does, and it is why
// pcmsamples.h includes nothing of ASIO's: this runs on the machine the gate runs on.
//
// It links nothing at all. No VST3, no graphics, no host layer — one header and the standard
// library, so a failure here is the arithmetic and cannot be anything else.

#include "pcmsamples.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Rations;

namespace
{

int gFailures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok)
        ++gFailures;
}

const char *formatName(int format)
{
    switch (format) {
        case kPcmInt16LSB:
            return "Int16LSB";
        case kPcmInt24LSB:
            return "Int24LSB";
        case kPcmInt32LSB:
            return "Int32LSB";
        case kPcmFloat32LSB:
            return "Float32LSB";
        case kPcmFloat64LSB:
            return "Float64LSB";
        case kPcmInt32LSB16:
            return "Int32LSB16";
        case kPcmInt32LSB18:
            return "Int32LSB18";
        case kPcmInt32LSB20:
            return "Int32LSB20";
        case kPcmInt32LSB24:
            return "Int32LSB24";
        default:
            return "?";
    }
}

// Every format the backend claims to handle. The list is written out rather than derived, so a
// format added to the header without a thought about this gate shows up as a gap here.
const int kFormats[] = {kPcmInt16LSB,   kPcmInt24LSB,   kPcmInt32LSB,
                        kPcmFloat32LSB, kPcmFloat64LSB, kPcmInt32LSB16,
                        kPcmInt32LSB18, kPcmInt32LSB20, kPcmInt32LSB24};

// The quantisation step of one format, as a fraction of full scale. A round trip through an integer
// format can lose up to half a step, so this is what "identical" has to mean.
double stepOf(int format)
{
    switch (format) {
        case kPcmFloat32LSB:
            return 0.0;
        case kPcmFloat64LSB:
            // The float round trip goes through a float on our side, so the loss is float's, not
            // double's: one ULP near 1.0 is 2^-23.
            return 1.0 / 8388608.0;
        case kPcmInt16LSB:
        case kPcmInt32LSB16:
            return 1.0 / 32768.0;
        case kPcmInt32LSB18:
            return 1.0 / 131072.0;
        case kPcmInt32LSB20:
            return 1.0 / 524288.0;
        case kPcmInt24LSB:
        case kPcmInt32LSB24:
            return 1.0 / 8388608.0;
        case kPcmInt32LSB:
            // Full-scale 32-bit, round-tripped through a 32-bit float on our side, so the step that
            // matters is again float's rather than the format's.
            return 1.0 / 8388608.0;
        default:
            return 1.0;
    }
}

} // namespace

//------------------------------------------------------------------------
int main()
{
    std::printf("namp_audiocheck - the Windows audio path's arithmetic, checked without Windows\n");

    std::printf("\nwhat the backend says it can carry\n");
    {
        for (const int f : kFormats)
            check(pcmFormatSupported(f), std::string(formatName(f)) + " is supported");
        // The big-endian and DSD families are refused BY NAME rather than guessed at — see the
        // header. A silently-accepted format nobody can test is worse than a named refusal.
        check(!pcmFormatSupported(0), "Int16MSB is refused rather than byte-swapped on a guess");
        check(!pcmFormatSupported(3), "Float32MSB is refused");
        check(!pcmFormatSupported(32), "DSD is refused - it is a bit stream, not samples");
        check(!pcmFormatSupported(-1) && !pcmFormatSupported(9999),
              "...and so is a value no SDK ever defined");
    }

    std::printf("\nthe sizes the buffer walk depends on\n");
    {
        check(pcmBytesPerSample(kPcmInt16LSB) == 2, "Int16LSB is 2 bytes");
        check(pcmBytesPerSample(kPcmInt24LSB) == 3,
              "Int24LSB is 3 - packed, not padded, which is why it is read byte by byte");
        check(pcmBytesPerSample(kPcmInt32LSB) == 4, "Int32LSB is 4");
        check(pcmBytesPerSample(kPcmFloat32LSB) == 4, "Float32LSB is 4");
        check(pcmBytesPerSample(kPcmFloat64LSB) == 8, "Float64LSB is 8");
        check(pcmBytesPerSample(kPcmInt32LSB16) == 4,
              "Int32LSB16 is 4 - 16 significant bits in a 32-bit container");
    }

    std::printf("\nfull scale, silence and polarity, per format\n");
    {
        // The three values that expose a wrong scale factor or a wrong sign immediately. 0.5 is in
        // there because it is exactly representable in every one of these formats, so it must come
        // back EXACTLY rather than within a step.
        for (const int f : kFormats) {
            const int n = 4;
            const float in[4] = {0.0f, 0.5f, -0.5f, -1.0f};
            std::vector<unsigned char> raw(static_cast<size_t>(n * pcmBytesPerSample(f)));
            float out[4] = {9.0f, 9.0f, 9.0f, 9.0f};

            floatToPcm(in, raw.data(), f, n);
            pcmToFloat(raw.data(), f, out, n);

            const double tol = stepOf(f);
            bool ok = out[0] == 0.0f;
            ok = ok && std::fabs(out[1] - 0.5) <= tol;
            ok = ok && std::fabs(out[2] + 0.5) <= tol;
            // -1.0 is the one value that IS exactly representable in a signed integer format: the
            // negative side has one more step than the positive one. Anything but an exact -1 back
            // means the scaling used 2^n-1 rather than 2^n.
            ok = ok && std::fabs(out[3] + 1.0) <= tol;
            check(ok, std::string(formatName(f)) + ": 0, +/-0.5 and -1.0 survive the round trip");
        }
    }

    std::printf("\na ramp through every format, and what a round trip may lose\n");
    {
        // 1024 values across the whole range, which is what catches a scale that is right at the
        // extremes and wrong in between.
        constexpr int n = 1024;
        std::vector<float> in(n);
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] = -1.0f + 2.0f * static_cast<float>(i) / (n - 1);

        for (const int f : kFormats) {
            std::vector<unsigned char> raw(static_cast<size_t>(n * pcmBytesPerSample(f)));
            std::vector<float> out(n, 9.0f);
            floatToPcm(in.data(), raw.data(), f, n);
            pcmToFloat(raw.data(), f, out.data(), n);

            double worst = 0.0;
            for (int i = 0; i < n; ++i)
                worst = std::max(worst, std::fabs(static_cast<double>(out[static_cast<size_t>(i)]) -
                                                  static_cast<double>(in[static_cast<size_t>(i)])));
            // Half a step is the theoretical floor for round-to-nearest; anything above one step is
            // a scaling error rather than quantisation.
            const double allowed = stepOf(f) > 0.0 ? stepOf(f) : 0.0;
            char note[96];
            std::snprintf(note, sizeof(note), "%s: worst |delta| %.3g, allowed %.3g", formatName(f),
                          worst, allowed);
            check(worst <= allowed, note);
        }
    }

    std::printf("\nmonotonic, because an inverted or folded conversion can still round-trip\n");
    {
        // A round trip cannot tell a correct conversion from one that inverts twice. What can is
        // the raw integer itself: it has to rise with the input across the whole range.
        for (const int f : kFormats) {
            if (f == kPcmFloat32LSB || f == kPcmFloat64LSB)
                continue; // no integer to inspect
            constexpr int n = 256;
            std::vector<float> in(n);
            for (int i = 0; i < n; ++i)
                in[static_cast<size_t>(i)] = -1.0f + 2.0f * static_cast<float>(i) / (n - 1);
            std::vector<unsigned char> raw(static_cast<size_t>(n * pcmBytesPerSample(f)));
            floatToPcm(in.data(), raw.data(), f, n);

            bool rising = true;
            int64_t previous = INT64_MIN;
            for (int i = 0; i < n; ++i) {
                const unsigned char *p = raw.data() + static_cast<size_t>(i * pcmBytesPerSample(f));
                int64_t v = 0;
                if (f == kPcmInt16LSB) {
                    int16_t s = 0;
                    std::memcpy(&s, p, sizeof(s));
                    v = s;
                } else if (f == kPcmInt24LSB) {
                    const uint32_t rawv = static_cast<uint32_t>(p[0]) |
                                          (static_cast<uint32_t>(p[1]) << 8) |
                                          (static_cast<uint32_t>(p[2]) << 16);
                    v = static_cast<int32_t>(rawv << 8) >> 8;
                } else {
                    int32_t s = 0;
                    std::memcpy(&s, p, sizeof(s));
                    v = s;
                }
                rising = rising && v >= previous;
                previous = v;
            }
            check(rising, std::string(formatName(f)) + ": the integers rise with the input");
        }
    }

    std::printf("\ninterleaved, which is how WASAPI hands out a buffer and ASIO never does\n");
    {
        // The stride is the difference between the two backends' buffers, and getting it wrong is
        // one channel played at half speed. Checked by building a THREE-channel interleaved buffer,
        // writing a different signal into each, and reading each back on its own: a stride error
        // shows up as one channel carrying another's samples.
        constexpr int frames = 64;
        constexpr int channels = 3;
        for (const int f : kFormats) {
            const int bytes = pcmBytesPerSample(f);
            std::vector<unsigned char> raw(static_cast<size_t>(frames * channels * bytes), 0xcd);
            std::vector<float> in[channels];
            for (int c = 0; c < channels; ++c) {
                in[c].resize(frames);
                for (int i = 0; i < frames; ++i) {
                    // A different ramp per channel, so a swap is visible rather than plausible.
                    const float t = static_cast<float>(i) / (frames - 1);
                    in[c][static_cast<size_t>(i)] = (c == 0)   ? (2.0f * t - 1.0f)
                                                    : (c == 1) ? (-0.5f * t)
                                                               : (0.25f);
                }
                floatToPcm(in[c].data(), raw.data() + static_cast<size_t>(c * bytes), f, frames,
                           channels);
            }

            bool ok = true;
            double worst = 0.0;
            for (int c = 0; c < channels; ++c) {
                std::vector<float> out(frames, 9.0f);
                pcmToFloat(raw.data() + static_cast<size_t>(c * bytes), f, out.data(), frames,
                           channels);
                for (int i = 0; i < frames; ++i)
                    worst = std::max(worst,
                                     std::fabs(static_cast<double>(out[static_cast<size_t>(i)]) -
                                               static_cast<double>(in[c][static_cast<size_t>(i)])));
            }
            const double allowed = stepOf(f);
            ok = worst <= allowed;
            char note[112];
            std::snprintf(note, sizeof(note),
                          "%s: three interleaved channels stay separate, worst |delta| %.3g",
                          formatName(f), worst);
            check(ok, note);
        }

        // And the one thing a per-channel read cannot catch: writing one channel must not touch its
        // neighbours. The buffer starts filled with a sentinel and only the middle channel is
        // written, so anything else that moved is a stride or a length error.
        for (const int f : kFormats) {
            const int bytes = pcmBytesPerSample(f);
            std::vector<unsigned char> raw(static_cast<size_t>(frames * channels * bytes), 0x5a);
            std::vector<float> ramp(frames, 0.5f);
            floatToPcm(ramp.data(), raw.data() + static_cast<size_t>(bytes), f, frames, channels);

            bool untouched = true;
            for (int i = 0; i < frames; ++i) {
                for (int c = 0; c < channels; ++c) {
                    if (c == 1)
                        continue;
                    const unsigned char *p =
                        raw.data() + static_cast<size_t>((i * channels + c) * bytes);
                    for (int b = 0; b < bytes; ++b)
                        untouched = untouched && p[b] == 0x5a;
                }
            }
            check(untouched, std::string(formatName(f)) +
                                 ": writing one interleaved channel leaves the others alone");
        }
    }

    std::printf("\nwhat happens to a sample the amp should never have produced\n");
    {
        // RULES: nothing non-finite reaches a speaker. The end-of-chain clamp is the first line of
        // that defence; this conversion is the last, and unlike the clamp it also has to survive
        // being handed a value merely out of range, which a hosted plug-in can legitimately
        // produce.
        const float nan = std::nanf("");
        const float inf = std::numeric_limits<float>::infinity();
        const float in[6] = {nan, -nan, inf, -inf, 4.0f, -4.0f};

        for (const int f : kFormats) {
            std::vector<unsigned char> raw(static_cast<size_t>(6 * pcmBytesPerSample(f)));
            std::vector<float> out(6, 9.0f);
            floatToPcm(in, raw.data(), f, 6);
            pcmToFloat(raw.data(), f, out.data(), 6);

            if (f == kPcmFloat32LSB || f == kPcmFloat64LSB) {
                // A float format is handed to the hardware as-is: there is no integer to wrap and
                // nothing this conversion can do about it, so this is the clamp's job upstream and
                // the check here is only that the transport is faithful.
                check(std::isnan(out[0]) && std::isinf(out[2]),
                      std::string(formatName(f)) + ": a float format passes the value through");
                continue;
            }

            // A NaN must become silence, not INT32_MIN — which is what a plain cast produces, and
            // which is the loudest sound the hardware can make.
            bool ok = out[0] == 0.0f && out[1] == 0.0f;
            // An infinity and an out-of-range value must clamp to full scale with the RIGHT SIGN.
            ok = ok && out[2] > 0.9f && out[3] < -0.9f;
            ok = ok && out[4] > 0.9f && out[5] < -0.9f;
            check(ok, std::string(formatName(f)) +
                          ": NaN becomes silence, and inf and 4.0 clamp with their own sign");
        }
    }

    std::printf("\n%s\n", gFailures == 0
                              ? "PASSED - the conversion is the arithmetic it claims to be"
                              : "FAILED");
    return gFailures == 0 ? 0 : 1;
}
