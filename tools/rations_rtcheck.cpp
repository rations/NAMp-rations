// rations_rtcheck — proves the audio path allocates nothing.
//
// "No allocation on the real-time thread" is a claim that inspection cannot settle: a std::function
// that captures one byte too many, a vector that grows on its first call, a model handed a block
// larger than the size it was Reset with — none of those are visible by reading. So this counts
// them, using the DSP core's own malloc/free interception harness.
//
// Three things are measured, and the third is the one most likely to be wrong:
//
//   1. The crossfade engine, sweeping the knob across a whole bank so branches bind, swap and
//      collapse while the count is running.
//   2. The engine through the resampler at 48 kHz, where the resampler is a straight call-through.
//   3. The engine through the resampler at 44.1 kHz, where it is not. ResamplingContainer takes its
//      block-processing callable as a std::function BY VALUE, and its own header warns that
//      captures can malloc. The plug-in relies on a one-reference capture fitting in libstdc++'s
//      small-object buffer. That is an assumption about a standard library implementation detail,
//      so it gets measured rather than asserted.

#include "crossfadeengine.h"
#include "engineconfig.h"
#include "modelbank.h"
#include "nativeresampler.h"

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
    if (argc < 2) {
        fprintf(stderr, "usage: rations_rtcheck <capture directory>\n");
        return 2;
    }
    const std::string dir = argv[1];

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

    // Sweeping the position is the point: it binds branches, swaps them at integer crossings and
    // collapses them at rest. A static position would exercise almost none of the engine.
    allocation_tracking::run_allocation_test_no_allocations(
        nullptr, [&] { direct.run(400); }, nullptr, "crossfade engine, knob sweeping");

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

    // Teardown happens off the counted path, which is the point of the whole retirement design.
    loader.stop();
    Rations::ModelBank::destroyBank(engine.releaseBank());

    printf("\nPASSED - no allocations on the audio path\n");
    return 0;
}
