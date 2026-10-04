// rations_lampcheck — the offline proof of the input-level lamp.
//
// The lamp beside the Input knob is lit when the peak the sounding capture hears sits in a window
// at the top of what NAM captures are trained on (inputlevellamp.h has the rule and where each
// number comes from). Two things have to be right for it to mean anything, and this tool asserts
// both rather than reading them off the code:
//
//   1. THE DETECTOR. The window's edges, the hold, the hot hold, the empty-channel reset and the
//      sample-rate dependence of the hold, driven with exact synthetic peaks. No captures needed.
//   2. THE GAIN IT IS GIVEN. The lamp judges the model INPUT, so it is told the input calibration
//      gain of the capture that is sounding. That gain is checked against the level each file
//      states, read through the capture parser rather than through the engine: exactly 1.0 with
//      Calibrate off and for a capture stating no level, exactly the calibration offset for one
//      that does, the dominant capture's and never a blend of two while the dial crossfades
//      between captures stating different levels, and 0 when nothing is bound. Then once more
//      through the shipped ChannelRack, across a channel switch.
//
// Usage: rations_lampcheck --bank <dir> --plain <file.nam> [--block N]
//   --bank   a folder holding two neighbouring captures (in the bank's own order) that state
//            different input_level_dbu
//   --plain  one capture that states no input_level_dbu

#include "capturesource.h"
#include "channelrack.h"
#include "crossfadeengine.h"
#include "engineconfig.h"
#include "inputlevellamp.h"
#include "modelbank.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using Rations::InputLevelLamp;

constexpr double kNativeRate = Rations::kNativeSampleRate;
constexpr int kOutputModeNormalized = 1;
// The interface level calibration works against, as the settings page's value box holds it.
constexpr double kCalLevelDbu = 12.0;

int gFailures = 0;

bool check(bool condition, const char *what, const char *detail = nullptr)
{
    if (condition)
        printf("  ok    %s\n", what);
    else if (detail)
        printf("  FAIL  %s (%s)\n", what, detail);
    else
        printf("  FAIL  %s\n", what);
    if (!condition)
        ++gFailures;
    return condition;
}

double dbToGain(double db)
{
    return std::pow(10.0, db / 20.0);
}

bool closeTo(double a, double b)
{
    return std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b));
}

struct Options {
    std::string bank;
    std::string plain;
    int block = 128;
};

bool parseArgs(int argc, char **argv, Options &opt)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--bank" && i + 1 < argc)
            opt.bank = argv[++i];
        else if (a == "--plain" && i + 1 < argc)
            opt.plain = argv[++i];
        else if (a == "--block" && i + 1 < argc)
            opt.block = std::atoi(argv[++i]);
        else
            return false;
    }
    return !opt.bank.empty() && !opt.plain.empty() && opt.block > 0 && opt.block <= 4096;
}

// --- 1. the detector --------------------------------------------------------------------------

// One block of a constant magnitude: the exact peak, with no question of whether a sample landed
// on a crest.
void feed(InputLevelLamp &lamp, double peak, int n, double gain = 1.0)
{
    std::vector<double> x(static_cast<size_t>(n), peak);
    lamp.process(x.data(), n, gain);
}

// How many samples of silence after one in-window block the lamp stays lit for.
long long litFor(double rate, int block)
{
    InputLevelLamp lamp;
    lamp.reset(rate);
    feed(lamp, dbToGain(-3.0), block);
    long long samples = 0;
    while (lamp.lit() && samples < static_cast<long long>(rate * 10.0)) {
        feed(lamp, 0.0, block);
        samples += block;
    }
    return samples;
}

void checkDetector(int block)
{
    printf("--- 1. the detector ---\n");
    const double below = dbToGain(InputLevelLamp::kLitAboveDb);
    const double top = dbToGain(InputLevelLamp::kLitAtMostDb);

    auto litAfterOne = [&](double peak, double gain = 1.0) {
        InputLevelLamp lamp;
        lamp.reset(kNativeRate);
        feed(lamp, peak, block, gain);
        return lamp.lit();
    };
    check(!litAfterOne(dbToGain(-7.0)), "-7 dBFS is too quiet: off");
    check(!litAfterOne(below), "exactly -6.0 dBFS is still too quiet: off (Amped's edge)");
    check(litAfterOne(below * 1.0001), "just above -6.0 dBFS: lit");
    check(litAfterOne(dbToGain(-5.0)), "-5 dBFS: lit");
    check(litAfterOne(top), "exactly 0.0 dBFS: lit (the top edge is inclusive)");
    check(!litAfterOne(top * 1.0001), "just above 0.0 dBFS is too hot: off");
    check(!litAfterOne(dbToGain(1.0)), "+1 dBFS: off");

    {
        // A sine whose crest lands on a sample, so its peak is its amplitude.
        std::vector<double> x(static_cast<size_t>(block));
        for (int i = 0; i < block; ++i)
            x[static_cast<size_t>(i)] = dbToGain(-5.0) * std::sin(2.0 * M_PI * i / 32.0);
        InputLevelLamp lamp;
        lamp.reset(kNativeRate);
        lamp.process(x.data(), block, 1.0);
        check(lamp.lit(), "a -5 dBFS sine lights it, not only a constant");
    }

    check(litAfterOne(dbToGain(-8.0), dbToGain(3.0)),
          "the gain is part of the peak: -8 dBFS under +3 dB of calibration is lit");
    check(!litAfterOne(dbToGain(-8.0), dbToGain(1.0)), "... and under +1 dB is not");
    check(!litAfterOne(dbToGain(-3.0), 0.0), "gain 0 (no capture hearing it) is off");

    {
        const long long hold = std::llround(InputLevelLamp::kHoldSeconds * kNativeRate);
        const long long held = litFor(kNativeRate, block);
        char detail[96];
        snprintf(detail, sizeof detail, "lit for %lld samples of silence, hold is %lld", held,
                 hold);
        check(held >= hold && held < hold + block,
              "the hold keeps it lit through silence for the hold time, to the block", detail);
        const long long held44 = litFor(44100.0, block), held96 = litFor(96000.0, block);
        snprintf(detail, sizeof detail, "%lld at 44.1 kHz, %lld at 96 kHz", held44, held96);
        check(held44 >= 44100 && held44 < 44100 + block && held96 >= 96000 &&
                  held96 < 96000 + block,
              "the hold is counted at the rate reset() was given", detail);
    }

    {
        InputLevelLamp lamp;
        lamp.reset(kNativeRate);
        feed(lamp, dbToGain(-3.0), block);
        feed(lamp, dbToGain(2.0), block);
        check(!lamp.lit(), "a hot block takes a lit lamp off at once");
        const long long hold = std::llround(InputLevelLamp::kHoldSeconds * kNativeRate);
        long long dark = 0;
        bool stayedDark = true;
        while (dark < hold - block) {
            feed(lamp, dbToGain(-3.0), block);
            dark += block;
            stayedDark = stayedDark && !lamp.lit();
        }
        check(stayedDark, "... and in-window playing does not relight it inside the hot hold");
        int blocksToRelight = 0;
        while (!lamp.lit() && blocksToRelight < 4) {
            feed(lamp, dbToGain(-3.0), block);
            ++blocksToRelight;
        }
        check(lamp.lit() && blocksToRelight <= 2, "... and relights once the hot hold is over");
    }

    {
        InputLevelLamp lamp;
        lamp.reset(kNativeRate);
        feed(lamp, dbToGain(-3.0), block);
        feed(lamp, dbToGain(-3.0), block, 0.0);
        check(!lamp.lit(), "gain 0 takes a lit lamp off at once, without waiting for the hold");
    }

    {
        std::vector<double> x(static_cast<size_t>(block), dbToGain(-3.0));
        x[5] = std::numeric_limits<double>::quiet_NaN();
        InputLevelLamp lamp;
        lamp.reset(kNativeRate);
        lamp.process(x.data(), block, 1.0);
        check(lamp.lit(), "a NaN sample is ignored, not propagated");
        x[5] = std::numeric_limits<double>::infinity();
        lamp.process(x.data(), block, 1.0);
        check(!lamp.lit(), "an infinite sample reads as hot");
    }
}

// --- 2. the gain the lamp is given --------------------------------------------------------------

// The input level each capture states, in the bank's own order, read through the capture parser
// and not through the engine. NaN for a capture that states none.
std::vector<double> statedLevels(const std::string &dir)
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".nam")
            files.push_back(it->path());
    std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) {
        return Rations::captureFilenameLess(a.filename().string(), b.filename().string());
    });
    std::vector<double> levels;
    for (const fs::path &f : files) {
        Rations::CaptureSource source;
        std::string error;
        if (!Rations::loadCaptureSource(f, source, error)) {
            fprintf(stderr, "rations_lampcheck: %s: %s\n", f.string().c_str(), error.c_str());
            levels.push_back(std::numeric_limits<double>::quiet_NaN());
            continue;
        }
        // The largest variant, which is the one Slim 1.0 builds.
        const auto &level = source.submodels.back().metadata.input_level;
        levels.push_back(level ? *level : std::numeric_limits<double>::quiet_NaN());
    }
    return levels;
}

// The stated level goes through a float on its way to the engine: the DSP core keeps a model's
// input level in a float member (nam::DSP::Level, in NAM/dsp.h), so 16.3 dBu reaches the gain as
// the nearest float to it, about 1e-7 dB away. The engine's gain is what actually scales the model
// input, so it is the right number for the lamp too; the expectation rounds the same way, which is
// what lets the comparison stay at 1e-12 instead of being loosened to hide the float.
double expectedGain(double level, bool calibrate)
{
    if (!calibrate || std::isnan(level))
        return 1.0;
    return dbToGain(kCalLevelDbu - static_cast<double>(static_cast<float>(level)));
}

bool waitForBank(Rations::ModelBank &loader)
{
    for (int i = 0; i < 1200 && loader.progress() < 1.0f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return loader.progress() >= 1.0f;
}

struct Engine {
    Rations::ModelBank loader;
    Rations::CrossfadeEngine engine;
    std::vector<double> in, out;

    explicit Engine(int block) : in(static_cast<size_t>(block)), out(static_cast<size_t>(block))
    {
        for (int i = 0; i < block; ++i)
            in[static_cast<size_t>(i)] = 0.1 * std::sin(2.0 * M_PI * 110.0 * i / kNativeRate);
        engine.setLoader(&loader);
        engine.prepare(block, kNativeRate);
        loader.start();
    }
    ~Engine()
    {
        loader.stop();
        Rations::ModelBank::destroyBank(engine.releaseBank());
    }
    void run(double pos)
    {
        engine.pollBank();
        engine.setPositionNorm(pos);
        NAM_SAMPLE *ip = in.data(), *op = out.data();
        engine.processNative(&ip, &op, static_cast<int>(in.size()));
    }
    // Long enough for the slew to arrive and the detent to settle.
    void settle(double pos)
    {
        for (int i = 0; i < static_cast<int>(kNativeRate) / static_cast<int>(in.size()); ++i)
            run(pos);
    }
};

void checkEngine(const Options &opt)
{
    printf("--- 2. the gain the lamp is given: one channel ---\n");
    const std::vector<double> levels = statedLevels(opt.bank);
    const int count = static_cast<int>(levels.size());

    // Two neighbours whose gains differ with Calibrate on: the crossing the dial test needs.
    int pair = -1;
    for (int i = 0; i + 1 < count && pair < 0; ++i)
        if (!closeTo(expectedGain(levels[i], true), expectedGain(levels[i + 1], true)))
            pair = i;
    if (!check(count >= 2 && pair >= 0,
               "the bank has neighbouring captures stating different input levels")) {
        return;
    }
    printf("        captures %d and %d state %.3f and %.3f dBu, against %.1f dBu\n", pair, pair + 1,
           levels[pair], levels[pair + 1], kCalLevelDbu);

    {
        Engine e(opt.block);
        e.run(0.0);
        check(e.engine.dominantInputGain() == 0.0, "an engine with no capture assigned gives 0");

        e.engine.setOutputMode(kOutputModeNormalized, kCalLevelDbu, true);
        e.loader.loadDirectory(opt.bank, 1.0, Rations::engine::kChunk);
        // Whatever blocks run before the first entry is built must give 0; how many there are is
        // up to the worker, so it is reported rather than required.
        int building = 0;
        bool zeroWhileBuilding = true;
        while (!e.engine.playable() && building < 100000) {
            e.run(0.0);
            if (!e.engine.playable()) {
                ++building;
                zeroWhileBuilding = zeroWhileBuilding && e.engine.dominantInputGain() == 0.0;
            }
        }
        char detail[64];
        snprintf(detail, sizeof detail, "%d blocks ran before the first capture was built",
                 building);
        check(zeroWhileBuilding, "a channel still building gives 0", detail);
        printf("        measured: %s\n", detail);
        if (!check(waitForBank(e.loader), "the bank builds"))
            return;

        const double norm = 1.0 / static_cast<double>(count - 1);
        bool restOn = true, restOff = true;
        for (int i = 0; i < count; ++i) {
            e.engine.setOutputMode(kOutputModeNormalized, kCalLevelDbu, true);
            e.settle(i * norm);
            const double got = e.engine.dominantInputGain();
            const double want = expectedGain(levels[i], true);
            if (!closeTo(got, want))
                printf(
                    "        capture %d (index %.0f sounding): gain %.17g, its level asks %.17g\n",
                    i, e.engine.activeIndexNorm() * (count - 1), got, want);
            restOn = restOn && closeTo(got, want);
            e.engine.setOutputMode(kOutputModeNormalized, kCalLevelDbu, false);
            e.run(i * norm);
            restOff = restOff && e.engine.dominantInputGain() == 1.0;
        }
        check(restOff, "Calibrate off: every capture gives exactly 1.0");
        check(restOn, "Calibrate on: every capture gives the offset its own stated level asks for");

        e.engine.setOutputMode(kOutputModeNormalized, kCalLevelDbu, true);
        e.settle(pair * norm);
        const double at12 = e.engine.dominantInputGain();
        e.engine.setOutputMode(kOutputModeNormalized, kCalLevelDbu + 3.0, true);
        e.run(pair * norm);
        const double at15 = e.engine.dominantInputGain();
        check(!std::isnan(levels[pair]) ? closeTo(at15 / at12, dbToGain(3.0)) : at15 == at12,
              "moving the interface level by 3 dB moves the gain by exactly 3 dB");

        // The dial swept slowly across the pair and back, so the crossfade spends many blocks with
        // both captures bound. At every block the gain must be the one the sounding capture asks
        // for: the same capture activeIndexNorm() names, and never a blend of the two.
        e.engine.setOutputMode(kOutputModeNormalized, kCalLevelDbu, true);
        e.settle(pair * norm);
        const double gLow = expectedGain(levels[pair], true);
        const double gHigh = expectedGain(levels[pair + 1], true);
        const int steps = 2 * static_cast<int>(kNativeRate) / opt.block;
        bool agrees = true, sawLow = false, sawHigh = false, sawBoth = false;
        for (int pass = 0; pass < 2; ++pass) {
            for (int s = 0; s <= steps; ++s) {
                const double t = static_cast<double>(s) / steps;
                const double pos = (pair + (pass == 0 ? t : 1.0 - t)) * norm;
                e.run(pos);
                const int sounding =
                    static_cast<int>(std::lround(e.engine.activeIndexNorm() * (count - 1)));
                const double g = e.engine.dominantInputGain();
                agrees = agrees && closeTo(g, expectedGain(levels[sounding], true));
                if (e.engine.boundBranches() == 2) {
                    sawBoth = true;
                    sawLow = sawLow || closeTo(g, gLow);
                    sawHigh = sawHigh || closeTo(g, gHigh);
                }
            }
        }
        check(sawBoth && sawLow && sawHigh,
              "the sweep crossed the pair with both captures bound, and saw each one's gain");
        check(agrees, "every block gives the gain of the capture activeIndexNorm() names");
    }

    {
        Engine e(opt.block);
        e.engine.setOutputMode(kOutputModeNormalized, kCalLevelDbu, true);
        e.loader.loadFile(opt.plain, 1.0, Rations::engine::kChunk);
        if (check(waitForBank(e.loader), "the plain capture builds")) {
            e.settle(0.0);
            check(e.engine.dominantInputGain() == 1.0,
                  "Calibrate on, a capture stating no level: exactly 1.0, nothing guessed");
        }
    }
}

void checkRack(const Options &opt)
{
    printf("--- 3. the gain the lamp is given: the rack ---\n");
    const std::vector<double> levels = statedLevels(opt.bank);
    if (levels.empty())
        return;
    const double g0 = expectedGain(levels[0], true);

    Rations::ChannelRack rack;
    rack.prepare(opt.block, kNativeRate);
    rack.setOutputMode(kOutputModeNormalized, kCalLevelDbu, true);
    rack.start();

    std::vector<double> in(static_cast<size_t>(opt.block)), out(in.size());
    for (int i = 0; i < opt.block; ++i)
        in[static_cast<size_t>(i)] = 0.1 * std::sin(2.0 * M_PI * 110.0 * i / kNativeRate);
    auto run = [&](Rations::Channel want) {
        rack.pollBanks();
        for (int c = 0; c < Rations::kChannelCount; ++c)
            rack.setPositionNorm(static_cast<Rations::Channel>(c), 0.0);
        rack.requestChannel(want);
        NAM_SAMPLE *ip = in.data(), *op = out.data();
        rack.processNative(&ip, &op, opt.block);
    };
    // Run until the rack sounds `want`, checking the lamp's gain at every block from the first
    // one in which it does.
    auto switchTo = [&](Rations::Channel want, double expected) {
        bool arrived = false, right = true;
        for (int b = 0; b < 2000; ++b) {
            run(want);
            if (rack.soundingChannel() == want) {
                arrived = true;
                right = right && closeTo(rack.soundingInputGain(), expected);
            }
        }
        return arrived && right;
    };

    run(Rations::kChannelClean);
    check(rack.soundingInputGain() == 0.0, "a rack with nothing loaded gives 0");

    rack.loadChannel(Rations::kChannelClean, opt.bank, true, 1.0, Rations::engine::kChunk);
    rack.loadChannel(Rations::kChannelCrunch, opt.plain, false, 1.0, Rations::engine::kChunk);
    for (int i = 0; i < 1200 && rack.progress() < 1.0f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (check(rack.progress() >= 1.0f, "both channels build")) {
        check(switchTo(Rations::kChannelClean, g0),
              "sounding the bank: the gain of its first capture's stated level");
        check(switchTo(Rations::kChannelCrunch, 1.0),
              "switched to the plain capture: 1.0 from the block the switch sounds it");
        check(switchTo(Rations::kChannelClean, g0), "and back: the bank's gain again");
    }

    rack.stop();
    rack.releaseBanks();
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        fprintf(stderr, "usage: rations_lampcheck --bank <dir> --plain <file.nam> [--block N]\n"
                        "  --bank   a folder with two neighbouring captures stating different\n"
                        "           input_level_dbu\n"
                        "  --plain  a capture stating no input_level_dbu\n");
        return 2;
    }

    checkDetector(opt.block);
    checkEngine(opt);
    checkRack(opt);

    printf("\n%s\n", gFailures == 0 ? "rations_lampcheck: PASSED" : "rations_lampcheck: FAILED");
    return gFailures == 0 ? 0 : 1;
}
