// rations_rtcheck — proves the audio path allocates nothing.
//
// "No allocation on the real-time thread" is a claim that inspection cannot settle: a std::function
// that captures one byte too many, a vector that grows on its first call, a model handed a block
// larger than the size it was Reset with — none of those are visible by reading. So this counts
// them, using the DSP core's own malloc/free interception harness.
//
// Six things are measured, and the middle ones are those most likely to be wrong:
//
//   1. The crossfade engine, sweeping the knob across a whole bank so branches bind, swap and
//      collapse while the count is running.
//   2. The engine through the resampler at 48 kHz, where the resampler is a straight call-through.
//   3. The engine through the resampler at 44.1 kHz, where it is not. ResamplingContainer takes its
//      block-processing callable as a std::function BY VALUE, and its own header warns that
//      captures can malloc. The plug-in relies on a one-reference capture fitting in libstdc++'s
//      small-object buffer. That is an assumption about a standard library implementation detail,
//      so it gets measured rather than asserted.
//   4. The channel rack, switching channels back and forth while the count runs. The catch-up
//      burst is still the audio thread: it feeds the incoming channel out of the input ring inside
//      the same process() call, so every rule that applies to the steady state applies to it. This
//      is also where a ring read, a fade buffer or a chunk stage that was sized wrong would show
//      up, because those only run during a switch.
//   5. The cabinet stage with both IR slots filled and the blend dial sweeping. AudioDSPTools
//      sizes an ImpulseResponse's history and output buffers on its FIRST Process call, which for
//      an IR loaded mid-session would be a malloc on the audio thread. The plug-in avoids that by
//      running each IR once off-thread when it loads it (the same pass that profiles it for the
//      blend). That is a claim about a lazy allocation inside someone else's class, which is
//      exactly the kind of claim inspection gets wrong, so it is counted here instead.
//   6. The MIDI learn table, matching a stream of every message type against every row. Small,
//      and counted anyway: a footswitch is evaluated on the audio thread, and this is the one
//      piece of RT code here whose natural expression - a struct with a string in it, looked up
//      in a map - would allocate.

#include "channelrack.h"
#include "crossfadeengine.h"
#include "engineconfig.h"
#include "irblend.h"
#include "midilearn.h"
#include "modelbank.h"
#include "nativeresampler.h"
#include "pedals/pedalchain.h"

#include "test/allocation_tracking.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr int kBlock = 512;

// Drives the engine directly, bypassing the resampler.
struct DirectDriver {
    Rations::CrossfadeEngine &engine;
    std::vector<NAM_SAMPLE> &in;
    std::vector<NAM_SAMPLE> &out;

    void run(int blocks)
    {
        NAM_SAMPLE *ip = in.data();
        NAM_SAMPLE *op = out.data();
        for (int b = 0; b < blocks; ++b) {
            engine.setPositionNorm(static_cast<double>(b) / static_cast<double>(blocks));
            engine.processNative(&ip, &op, kBlock);
        }
    }
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: rations_rtcheck <capture directory A> <capture directory B>\n"
                        "  two directories, because a channel switch needs somewhere to switch to "
                        "and the catch-up burst is part of the audio path\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string dirB = argv[2];

    Rations::ModelBank loader;
    loader.start();
    Rations::CrossfadeEngine engine;
    engine.setLoader(&loader);

    Rations::NativeResampler resampler48, resampler44;
    resampler48.configure(48000.0, kBlock);
    resampler44.configure(44100.0, kBlock);
    const int maxNative =
        std::max(resampler48.maxNativeBlock(kBlock), resampler44.maxNativeBlock(kBlock));
    engine.prepare(maxNative, Rations::kNativeSampleRate);

    loader.loadDirectory(dir, 1.0, Rations::engine::kChunk);

    // Wait for the whole bank, so no model is built while the count is running.
    for (int i = 0; i < 600 && loader.progress() < 1.0f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.pollBank();
    if (engine.entryCount() == 0) {
        fprintf(stderr, "rations_rtcheck: no captures loaded from %s (%s)\n", dir.c_str(),
                loader.lastError().c_str());
        return 1;
    }
    printf("bank           %d captures, progress %.0f%%\n", engine.entryCount(),
           100.0 * loader.progress());
    printf("resampler      48000 Hz engaged=%d latency=%d | 44100 Hz engaged=%d latency=%d\n\n",
           resampler48.engaged() ? 1 : 0, resampler48.latency(), resampler44.engaged() ? 1 : 0,
           resampler44.latency());

    std::vector<NAM_SAMPLE> in(static_cast<size_t>(maxNative), 0.0);
    std::vector<NAM_SAMPLE> out(static_cast<size_t>(maxNative), 0.0);
    for (int i = 0; i < maxNative; ++i)
        in[static_cast<size_t>(i)] = 0.25 * std::sin(2.0 * M_PI * 110.0 * i / 48000.0);

    // Warm every lazily-sized buffer BEFORE counting, exactly as setActive() does in the plug-in.
    // Counting the first-call growth would measure the warm-up, not the steady state.
    DirectDriver direct{engine, in, out};
    direct.run(8);
    {
        NAM_SAMPLE *ip = in.data();
        NAM_SAMPLE *op = out.data();
        for (int b = 0; b < 8; ++b) {
            resampler48.process(&ip, &op, kBlock, engine);
            resampler44.process(&ip, &op, kBlock, engine);
        }
    }

    // First, prove the harness has teeth. A checker that cannot fail is not evidence of anything,
    // and operator-new interception is exactly the kind of thing that can be silently defeated by
    // a link order or an inlining decision.
    {
        allocation_tracking::g_allocation_count = 0;
        allocation_tracking::g_deallocation_count = 0;
        allocation_tracking::g_tracking_enabled = true;
        volatile auto *leak = new double[64];
        delete[] leak;
        allocation_tracking::g_tracking_enabled = false;
        if (allocation_tracking::g_allocation_count == 0) {
            fprintf(stderr, "FAIL: the allocation harness did not see a deliberate allocation, so "
                            "the results below would be meaningless\n");
            return 1;
        }
        printf("harness        sees a deliberate allocation (%d), so the counts below mean "
               "something\n\n",
               allocation_tracking::g_allocation_count);
    }

    printf("counting       crossfade engine, knob sweeping\n");
    // Sweeping the position is the point: it binds branches, swaps them at integer crossings and
    // collapses them at rest. A static position would exercise almost none of the engine.
    allocation_tracking::run_allocation_test_no_allocations(
        nullptr, [&] { direct.run(400); }, nullptr, "crossfade engine, knob sweeping");

    printf("counting       engine through the resampler at 48 kHz (bypassed)\n");
    allocation_tracking::run_allocation_test_no_allocations(
        nullptr,
        [&] {
            NAM_SAMPLE *ip = in.data();
            NAM_SAMPLE *op = out.data();
            for (int b = 0; b < 200; ++b) {
                engine.setPositionNorm(static_cast<double>(b) / 200.0);
                resampler48.process(&ip, &op, kBlock, engine);
            }
        },
        nullptr, "engine through the resampler at 48 kHz (bypassed)");

    printf("counting       engine through the resampler at 44.1 kHz (engaged)\n");
    allocation_tracking::run_allocation_test_no_allocations(
        nullptr,
        [&] {
            NAM_SAMPLE *ip = in.data();
            NAM_SAMPLE *op = out.data();
            for (int b = 0; b < 200; ++b) {
                engine.setPositionNorm(static_cast<double>(b) / 200.0);
                resampler44.process(&ip, &op, kBlock, engine);
            }
        },
        nullptr, "engine through the resampler at 44.1 kHz (engaged, std::function by value)");

    // --- the channel rack, switching -----------------------------------------------------------
    // Two channels loaded and a stomp every few blocks, so the count spans the whole switch: the
    // ring writes, the catch-up feed at kCatchupRatio, the fade, and a stomp that lands inside a
    // switch already in flight.
    {
        Rations::ChannelRack rack;
        rack.prepare(kBlock, Rations::kNativeSampleRate);
        rack.start();
        rack.loadChannel(Rations::kChannelClean, dir, /*isDirectory=*/true, 1.0,
                         Rations::engine::kChunk);
        rack.loadChannel(Rations::kChannelOd1, dirB, /*isDirectory=*/true, 1.0,
                         Rations::engine::kChunk);
        for (int i = 0; i < 1200 && rack.progress() < 1.0f; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (rack.progress() < 1.0f) {
            fprintf(stderr, "rations_rtcheck: the rack's banks never finished building\n");
            return 1;
        }

        // The stomp period is deliberately shorter than a whole switch takes, so some requests
        // land mid-catch-up and some mid-fade. Both are the paths that only exist here.
        auto drive = [&](int blocks) {
            NAM_SAMPLE *ip = in.data();
            NAM_SAMPLE *op = out.data();
            for (int b = 0; b < blocks; ++b) {
                const Rations::Channel want =
                    ((b / 3) % 2) ? Rations::kChannelOd1 : Rations::kChannelClean;
                rack.pollBanks();
                rack.setPositionNorm(Rations::kChannelClean, 0.0);
                rack.setPositionNorm(Rations::kChannelOd1, 0.0);
                // Both trims moving, so the per-channel level ramp is running on the audio path
                // rather than sitting at unity where it costs one compare and takes no branch.
                // Opposite directions, so a stomp that lands mid-drag mixes two ramps at once.
                const double t = static_cast<double>(b) / 40.0;
                rack.setLevel(Rations::kChannelClean,
                              0.25 + 0.5 * std::fabs(std::fmod(t, 2.0) - 1.0));
                rack.setLevel(Rations::kChannelOd1, 4.0 - 3.0 * std::fabs(std::fmod(t, 2.0) - 1.0));
                rack.requestChannel(want);
                rack.processNative(&ip, &op, kBlock);
            }
        };
        drive(8); // warm anything lazily sized before the count starts

        printf("counting       channel rack, stomping between two channels, trims moving\n");
        allocation_tracking::run_allocation_test_no_allocations(
            nullptr, [&] { drive(200); }, nullptr,
            "channel rack, stomping between two channels, trims moving");

        rack.stop();
        rack.releaseBanks();
    }

    // --- the cabinet, both slots filled ---------------------------------------------------------
    // The IRs are built from raw audio rather than read from files: ImpulseResponse takes an IRData
    // directly, so the test needs no WAV on disk and no assumption about what is installed. Two
    // different decaying shapes, because two IDENTICAL ones would leave the blend weights at the
    // trivial values and skip the branch that matters.
    {
        auto makeIr = [](double decay, double wobble) {
            dsp::ImpulseResponse::IRData d;
            d.mRawAudioSampleRate = Rations::kNativeSampleRate;
            d.mRawAudio.resize(2048);
            for (size_t i = 0; i < d.mRawAudio.size(); ++i) {
                const double t = static_cast<double>(i);
                d.mRawAudio[i] = static_cast<float>(
                    std::exp(-t * decay) * std::sin(t * wobble + 0.3) * (i ? 1.0 : 0.0) +
                    (i == 0 ? 1.0 : 0.0));
            }
            return std::make_unique<dsp::ImpulseResponse>(d, Rations::kNativeSampleRate);
        };
        auto irA = makeIr(0.004, 0.21);
        auto irB = makeIr(0.001, 0.07);

        std::vector<double> profileA(Rations::kIrProfileSamples, 0.0);
        std::vector<double> profileB(Rations::kIrProfileSamples, 0.0);
        std::vector<double> mix(static_cast<size_t>(kBlock), 0.0);
        double *mixPtr = mix.data();

        // Warm and profile off the counted path, which is what the plug-in does on its message
        // thread when an IR is loaded.
        {
            std::vector<double> stim(static_cast<size_t>(Rations::kIrProfileSamples) + kBlock, 0.0);
            Rations::fillIrProfileStimulus(stim.data(),
                                           static_cast<size_t>(Rations::kIrProfileSamples),
                                           Rations::kNativeSampleRate);
            auto profile = [&](dsp::ImpulseResponse *ir, std::vector<double> &dst) {
                size_t got = 0;
                for (size_t pos = 0; got < dst.size(); pos += kBlock) {
                    double *p = stim.data() + pos;
                    double **out = ir->Process(&p, 1, kBlock);
                    const size_t take = std::min(static_cast<size_t>(kBlock), dst.size() - got);
                    std::copy(out[0], out[0] + take, dst.begin() + static_cast<long>(got));
                    got += take;
                }
            };
            profile(irA.get(), profileA);
            profile(irB.get(), profileB);
        }
        const Rations::IrBlend blend =
            Rations::measureIrBlend(profileA.data(), profileB.data(), profileA.size());
        printf("cabinet        two IRs, rho %.3f, powers %.4g / %.4g\n", blend.rho, blend.powerA,
               blend.powerB);

        auto sweepCabinet = [&](int blocks) {
            double *ip = in.data();
            for (int b = 0; b < blocks; ++b) {
                const double x = static_cast<double>(b % 64) / 63.0;
                Rations::processCabinet(irA.get(), irB.get(), blend, x, &ip, kBlock, &mixPtr);
            }
        };
        sweepCabinet(8); // anything still lazily sized is sized now

        printf("counting       cabinet, both IR slots filled, blend sweeping\n");
        allocation_tracking::run_allocation_test_no_allocations(
            nullptr, [&] { sweepCabinet(200); }, nullptr,
            "cabinet, both IR slots filled, blend sweeping");
    }

    // --- 6. the MIDI learn table ------------------------------------------------------------
    //
    // Small, and counted anyway. A footswitch is evaluated on the audio thread, and the table is
    // the one piece of RT code in this plug-in whose natural expression - a struct with a string
    // in it, looked up in a map - would allocate. What ships is a packed word per row read out of
    // an atomic, and this is the check that it stayed that way rather than drifting back toward
    // the natural expression the next time a row type is added.
    {
        std::atomic<std::uint32_t> table[Rations::kMidiLearnRowCount];
        for (int r = 0; r < Rations::kMidiLearnRowCount; ++r) {
            Rations::MidiBinding b;
            b.msg = (r % 2) ? Rations::MidiMsg::ControlChange : Rations::MidiMsg::NoteOn;
            b.channel = (r % 2) ? Rations::kMidiAnyChannel : r;
            b.data1 = 60 + r;
            table[r].store(Rations::packBinding(b));
        }

        printf("counting       MIDI learn table, matching every message type\n");
        allocation_tracking::run_allocation_test_no_allocations(
            nullptr,
            [&] {
                volatile int hits = 0;
                for (int i = 0; i < 20000; ++i) {
                    const Rations::MidiMsg msg = static_cast<Rations::MidiMsg>(1 + (i % 3));
                    const int channel = (i % 17 == 0) ? Rations::kMidiAnyChannel : (i % 16);
                    const int data1 = i % 128;
                    for (int r = 0; r < Rations::kMidiLearnRowCount; ++r) {
                        const Rations::MidiBinding bound =
                            Rations::unpackBinding(table[r].load(std::memory_order_acquire));
                        if (Rations::bindingMatches(bound, msg, channel, data1))
                            hits = hits + 1;
                    }
                }
                (void)hits;
            },
            nullptr, "MIDI learn table, matching every message type");
    }

    // --- the pedalboard --------------------------------------------------------------------
    //
    // Both chains, every footswitch stomped and every knob moved on the counted path. The engage
    // ramp is the part worth counting: it copies each channel to a dry buffer on every block a
    // pedal is in or moving, and those buffers are sized in prepare() precisely so that copy never
    // becomes an allocation.
    //
    // Stomping matters as much as sweeping. A pedal that has just been switched off keeps being
    // processed until its ramp lands, and the transition where it stops is also where its state is
    // reset - so the stomping loop is what puts resetImpl() on the counted path, where a pedal
    // that cleared its state by reallocating a buffer instead of zeroing one would be caught.
    {
        constexpr int kBlock = 256;
        Rations::pedals::PedalChain chain;
        chain.prepare(48000.0, kBlock);

        std::vector<DSP_SAMPLE> pre(kBlock, 0.0);
        std::vector<DSP_SAMPLE> postL(kBlock, 0.0), postR(kBlock, 0.0);
        double plain[Rations::kPedalParamCount] = {};

        // Warm both chains with everything ENGAGED before counting, the way setActive() warms the
        // gate and the tone stack: anything a pedal sizes on its first processed block has to be
        // sized here rather than inside the count.
        for (int i = 0; i < Rations::kPedalParamCount; ++i)
            plain[i] = Rations::kPedalParams[i].def;
        for (int p = 0; p < Rations::kPedalCount; ++p)
            plain[Rations::pedalParamFirst(p)] = 1.0;
        chain.setParams(plain);
        for (int b = 0; b < 8; ++b) {
            chain.processPre(pre.data(), kBlock);
            chain.processPost(postL.data(), postR.data(), kBlock);
        }

        printf("counting       pedalboard, both chains, stomping and sweeping every control\n");
        allocation_tracking::run_allocation_test_no_allocations(
            nullptr,
            [&] {
                for (int b = 0; b < 400; ++b) {
                    for (int i = 0; i < Rations::kPedalParamCount; ++i) {
                        const Rations::PedalParamSpec &spec = Rations::kPedalParams[i];
                        // Sweep every continuous control across its whole range, and stomp every
                        // footswitch on a different period so the five are never all in the same
                        // state at once.
                        const double phase = static_cast<double>((b * (i + 3)) % 64) / 63.0;
                        plain[i] = (spec.kind == Rations::PedalParamKind::Range)
                                       ? spec.min + phase * (spec.max - spec.min)
                                       : ((b / (i + 1)) % 2 ? spec.max : spec.min);
                    }
                    chain.setTempo(b % 3 ? 120.0 : 0.0);
                    chain.setParams(plain);
                    for (int k = 0; k < kBlock; ++k) {
                        const double x = 0.2 * std::sin(0.05 * (b * kBlock + k));
                        pre[k] = x;
                        postL[k] = x;
                        postR[k] = x;
                    }
                    if (chain.preActive())
                        chain.processPre(pre.data(), kBlock);
                    if (chain.postActive())
                        chain.processPost(postL.data(), postR.data(), kBlock);
                }
            },
            nullptr, "pedalboard, both chains, stomping and sweeping every control");
    }

    // Teardown happens off the counted path, which is the point of the whole retirement design.
    loader.stop();
    Rations::ModelBank::destroyBank(engine.releaseBank());

    printf("\nPASSED - no allocations on the audio path\n");
    return 0;
}
