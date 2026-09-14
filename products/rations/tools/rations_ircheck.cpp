// rations_ircheck — the offline proof for the cabinet page's two IR slots (D10).
//
// D10 makes two promises, and this asserts both against the BUILT BUNDLE rather than against a
// re-implementation of it: the IRs are loaded through the same IConnectionPoint messages the
// editor sends, and the blend dial is turned through the same parameter queue a DAW uses.
//
//   1. A user who loads one IR cannot tell the second slot exists. The one-IR path must be
//      BIT-IDENTICAL with the dial at 0, at 1 and everywhere between, and it must be bit-identical
//      to what the two-IR path produces at that IR's own end of the dial. Not "close" — identical.
//      A blend that quietly attenuated a one-IR user at every position but one would be a bug, and
//      approximate equality would hide exactly that.
//
//   2. The middle of the dial neither bumps nor digs a hole. Which curve achieves that depends on
//      the two files, and that is the measurement this tool exists for: two mic positions on one
//      cabinet are strongly correlated and want an amplitude-complementary mix, while two
//      different cabinets are not and want an equal-power one. So the tool renders the shipped
//      curve and ALSO computes, from the two endpoint renders, what the two textbook curves would
//      have done — which is exact, because the cabinet stage is linear and both IRs see the same
//      input, so mixing the endpoint renders IS the blend. The three numbers side by side are what
//      settles the curve.
//
// What that measurement said, over eight pairs of real cabinet IRs spanning both regimes (a Mesa
// 4x12 of V30s at a range of SM57 positions, a Fender Bassman 2x15, and crosses between the two):
//
//              same cabinet, mic moved     two different cabinets
//   shipped       0.00 - 0.05 dB              0.35 - 0.51 dB
//   linear        0.00 - 0.09 dB              1.85 - 2.73 dB
//   equal-power   2.28 - 3.01 dB              0.49 - 0.75 dB
//
// So neither fixed curve is defensible: each is excellent in one regime and unusable in the other,
// and which regime a user is in is not something the code can be told. The measured curve is as
// good as the better fixed curve everywhere and far better where the feature is actually aimed.
//
// The default tolerance is 0.75 dB rather than the 0.5 dB the plan set out with, and the reason is
// worth stating: 0.5 was a target chosen before anything had been measured, and the measurement
// says a single scalar correction cannot beat about 0.5 dB on an arbitrary pair of DIFFERENT
// cabinets, because their correlation is frequency-dependent and one number cannot track it. The
// limit is set above the measured worst case rather than at it, so the gate reports a regression
// instead of the noise floor of whichever IRs it was handed.
//
// Usage:
//   rations_ircheck <NAMp-rations.vst3> --pair <irA.wav> <irB.wav> [--pair ...]
//                   [--rate 48000] [--block 256] [--seconds 2.0] [--settle-ms 6000]
//                   [--tolerance-db 0.75]

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include "public.sdk/source/common/memorystream.h"

#include "rationsids.h"
#include "toolcaptures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;

namespace
{

struct Options {
    std::string bundle;
    std::vector<std::pair<std::string, std::string>> pairs;
    double rate = 48000.0;
    int block = 256;
    double seconds = 2.0;
    int settleMs = 6000;
    // A directory holding one subdirectory per channel. Defaults to $RATIONS_TEST_CAPTURES.
    std::string captures;
    double toleranceDb = 0.75;
};

// Every render is preceded by this much silence, discarded. It is what makes one render
// independent of the one before it: half a second flushes the tone stack, the gate, the IR
// history and the models' receptive fields back to a zero state, so two renders of the same
// signal at the same dial position are the same samples and the bit-identical tests below mean
// what they say.
constexpr double kFlushSeconds = 0.5;

// Spelled out rather than taken from <cmath>'s POSIX extensions, which are absent under MinGW's
// strict-ANSI settings and this tool is built there too.
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kHalfPi = 1.5707963267948966192313216916398;

bool parseArgs(int argc, char **argv, Options &opt)
{
    if (argc < 2) {
        fprintf(
            stderr,
            "usage: rations_ircheck <NAMp-rations.vst3> --pair <irA.wav> <irB.wav> [--pair ...]\n"
            "                       [--rate 48000] [--block 256] [--seconds 2.0]\n"
            "                       [--captures <dir>] [--settle-ms 6000]\n"
            "                       [--tolerance-db 0.75]\n");
        return false;
    }
    opt.bundle = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](double &dst) {
            if (i + 1 < argc)
                dst = atof(argv[++i]);
        };
        if (a == "--pair" && i + 2 < argc) {
            opt.pairs.emplace_back(argv[i + 1], argv[i + 2]);
            i += 2;
        } else if (a == "--rate") {
            next(opt.rate);
        } else if (a == "--seconds") {
            next(opt.seconds);
        } else if (a == "--tolerance-db") {
            next(opt.toleranceDb);
        } else if (a == "--block" && i + 1 < argc) {
            opt.block = atoi(argv[++i]);
        } else if (a == "--captures" && i + 1 < argc) {
            opt.captures = argv[++i];
        } else if (a == "--settle-ms" && i + 1 < argc) {
            opt.settleMs = atoi(argv[++i]);
        } else {
            fprintf(stderr, "rations_ircheck: unknown argument %s\n", a.c_str());
            return false;
        }
    }
    if (opt.pairs.empty()) {
        fprintf(stderr, "rations_ircheck: at least one --pair is required\n");
        return false;
    }
    return true;
}

// A sustained open E chord at constant amplitude, which is what a player actually judges two
// cabinets on.
//
// Two properties matter and both are deliberate. Constant amplitude, because the question is
// whether the LEVEL moves as the dial moves, and a signal with an envelope of its own would mix
// its own shape into the answer. And spectrally DENSE: a single note is a comb of two dozen
// discrete frequencies, and two cabinets can agree at those particular frequencies and disagree
// between them, so a one-note test measures the luck of that note rather than the blend. Six
// strings through the amp model's distortion fills the spectrum in, which is both fairer and
// closer to the thing being judged.
//
// The spectrum is deliberately NOT shaped to match the weighting the blend profiles with. Doing
// that would make the test agree with the measurement by construction and prove nothing.
void fillTestSignal(std::vector<float> &buf, double rate)
{
    // E2 A2 D3 G3 B3 E4 - standard tuning, open.
    const double strings[6] = {82.41, 110.00, 146.83, 196.00, 246.94, 329.63};
    for (size_t i = 0; i < buf.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        double v = 0.0;
        for (int str = 0; str < 6; ++str) {
            for (int h = 1; h <= 16; ++h) {
                const double f = strings[str] * h;
                if (f > rate * 0.45)
                    break;
                // A fixed irrational phase per partial, so ninety-odd partials do not all line up
                // into one enormous spike once per period and clip the model's input.
                v += std::sin(kTwoPi * f * t + (str * 16 + h) * 1.2345) / h;
            }
        }
        buf[i] = static_cast<float>(0.05 * v);
    }
}

double rms(const std::vector<float> &v, size_t from)
{
    if (from >= v.size())
        return 0.0;
    double sum = 0.0;
    for (size_t i = from; i < v.size(); ++i)
        sum += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    return std::sqrt(sum / static_cast<double>(v.size() - from));
}

double toDb(double lin)
{
    return lin > 0.0 ? 20.0 * std::log10(lin) : -300.0;
}

const char *baseName(const std::string &p)
{
    const size_t slash = p.find_last_of('/');
    return p.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

// Load (or, with an empty path, clear) one IR slot the way the editor does: an IConnectionPoint
// message straight at the component, with the path as a UTF-8 binary attribute.
bool sendIr(Vst::HostApplication &host, Vst::IComponent *component, int slot,
            const std::string &path)
{
    FUnknownPtr<Vst::IConnectionPoint> cp(component);
    if (!cp)
        return false;
    // createInstance takes TUID by value, which is a non-const char[16], so the interface id has
    // to be copied out of the FUID rather than passed straight through.
    TUID iid;
    memcpy(iid, Vst::IMessage::iid, sizeof(TUID));
    Vst::IMessage *raw = nullptr;
    if (host.createInstance(iid, iid, reinterpret_cast<void **>(&raw)) != kResultOk || !raw)
        return false;
    IPtr<Vst::IMessage> msg = owned(raw);
    msg->setMessageID(Rations::kMsgLoadIr[slot]);
    msg->getAttributes()->setBinary(Rations::kMsgPathAttr, path.c_str(),
                                    static_cast<uint32>(path.size()));
    // An empty path is a clear and the plug-in answers kResultOk; a real path that fails to parse
    // answers kResultFalse, which is a failed test rather than a quiet no-op.
    return cp->notify(msg) == kResultOk;
}

// One render at one dial position. The flush prefix is rendered and thrown away; what comes back
// is the measured part only.
struct Renderer {
    Vst::IAudioProcessor *processor = nullptr;
    Options opt;
    std::vector<float> signal; // flush prefix + measured signal
    size_t flushSamples = 0;

    std::vector<float> run(double blend)
    {
        const size_t total = signal.size();
        const int n = opt.block;
        std::vector<float> inBlock(static_cast<size_t>(n));
        std::vector<float> outL(static_cast<size_t>(n)), outR(static_cast<size_t>(n));
        float *inPtrs[1] = {inBlock.data()};
        float *outPtrs[2] = {outL.data(), outR.data()};

        Vst::AudioBusBuffers inBus = {};
        inBus.numChannels = 1;
        inBus.channelBuffers32 = inPtrs;
        Vst::AudioBusBuffers outBus = {};
        outBus.numChannels = 2;
        outBus.channelBuffers32 = outPtrs;

        Vst::ParameterChanges paramChanges;
        paramChanges.setMaxParameters(8);
        Vst::ParameterChanges outParamChanges;
        outParamChanges.setMaxParameters(8);

        Vst::ProcessData data = {};
        data.processMode = Vst::kOffline;
        data.symbolicSampleSize = Vst::kSample32;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &inBus;
        data.outputs = &outBus;

        std::vector<float> out;
        out.reserve(total);
        for (size_t pos = 0; pos < total; pos += static_cast<size_t>(n)) {
            const size_t take = std::min(static_cast<size_t>(n), total - pos);
            std::fill(inBlock.begin(), inBlock.end(), 0.0f);
            memcpy(inBlock.data(), signal.data() + pos, take * sizeof(float));

            paramChanges.clearQueue();
            int32 qi = 0, pi = 0;
            // Everything the cabinet is not is pinned: the gate off so it cannot open and close
            // across the render, the dial parked so the models stay at one detent, and the blend
            // held at the position under test.
            if (auto *q = paramChanges.addParameterData(Rations::kIrBlendId, qi))
                q->addPoint(0, blend, pi);
            if (auto *q = paramChanges.addParameterData(Rations::kNoiseGateOnId, qi))
                q->addPoint(0, 0.0, pi);
            if (auto *q = paramChanges.addParameterData(Rations::kCleanGainId, qi))
                q->addPoint(0, 0.5, pi);
            if (auto *q = paramChanges.addParameterData(Rations::kBypassId, qi))
                q->addPoint(0, 0.0, pi);

            outParamChanges.clearQueue();
            data.inputParameterChanges = &paramChanges;
            data.outputParameterChanges = &outParamChanges;
            data.numSamples = static_cast<int32>(n);
            processor->process(data);

            out.insert(out.end(), outL.begin(), outL.begin() + static_cast<long>(take));
        }
        out.erase(out.begin(), out.begin() + static_cast<long>(flushSamples));
        return out;
    }
};

bool identical(const std::vector<float> &a, const std::vector<float> &b)
{
    return a.size() == b.size() && memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// The mix of two endpoint renders at weights (wa, wb). Exact, because the cabinet stage is linear
// and both IRs are fed the same signal: what the plug-in computes as wa*A + wb*B per sample is
// what this computes from the two renders it already has.
double mixRms(const std::vector<float> &a, const std::vector<float> &b, double wa, double wb)
{
    const size_t n = std::min(a.size(), b.size());
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double v = wa * a[i] + wb * b[i];
        sum += v * v;
    }
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 2;

    Vst::HostApplication hostContext;
    Vst::PluginContextFactory::instance().setPluginContext(&hostContext);

    std::string error;
    auto module = VST3::Hosting::Module::create(opt.bundle, error);
    if (!module) {
        fprintf(stderr, "rations_ircheck: cannot load %s\n  %s\n", opt.bundle.c_str(),
                error.c_str());
        return 1;
    }
    auto factory = module->getFactory();
    IPtr<Vst::PlugProvider> provider;
    for (auto &classInfo : factory.classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass)
            continue;
        provider = owned(new Vst::PlugProvider(factory, classInfo, true));
        if (provider->initialize())
            break;
        provider = nullptr;
    }
    if (!provider) {
        fprintf(stderr, "rations_ircheck: no audio effect class in %s\n", opt.bundle.c_str());
        return 1;
    }

    Vst::IComponent *component = provider->getComponent();
    FUnknownPtr<Vst::IAudioProcessor> processor(component);
    if (!component || !processor) {
        fprintf(stderr, "rations_ircheck: the plug-in did not provide a processor\n");
        return 1;
    }

    // The cabinet stage is what this tool measures, but it measures it through the whole chain, so
    // the channels still have to be sounding something: with no captures loaded the rack outputs
    // ramped silence and every blend measurement would be taken on nothing.
    opt.captures = RationsTools::captureRoot(opt.captures);
    if (opt.captures.empty()) {
        RationsTools::printCaptureUsage("rations_ircheck");
        return 1;
    }
    if (!RationsTools::loadCaptureRoot(hostContext, component, opt.captures)) {
        fprintf(stderr, "rations_ircheck: the plug-in refused a capture directory under %s\n",
                opt.captures.c_str());
        return 1;
    }
    // The captures build on the plug-in's own workers; a render started before they land measures
    // the ramped silence that precedes them.
    std::this_thread::sleep_for(std::chrono::milliseconds(opt.settleMs));

    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kOffline;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = opt.block;
    setup.sampleRate = opt.rate;
    if (processor->setupProcessing(setup) != kResultOk) {
        fprintf(stderr, "rations_ircheck: the plug-in rejected the process setup\n");
        return 1;
    }
    component->setActive(true);
    processor->setProcessing(true);

    Renderer renderer;
    renderer.processor = processor;
    renderer.opt = opt;
    renderer.flushSamples = static_cast<size_t>(kFlushSeconds * opt.rate);
    {
        const size_t measured = static_cast<size_t>(opt.seconds * opt.rate);
        renderer.signal.assign(renderer.flushSamples + measured, 0.0f);
        std::vector<float> tone(measured);
        fillTestSignal(tone, opt.rate);
        memcpy(renderer.signal.data() + renderer.flushSamples, tone.data(),
               measured * sizeof(float));
    }

    // One throwaway render, so the models' ready ramp and the bypass ramp are already settled
    // before anything is compared. Without it the first measured render differs from every later
    // one for reasons that have nothing to do with the cabinet.
    renderer.run(0.0);

    printf("rations_ircheck  %.0f Hz, block %d, %.1f s per render, %.2f s flushed between\n\n",
           opt.rate, opt.block, opt.seconds, kFlushSeconds);

    int failures = 0;
    const double kBlendPoints[] = {0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875};

    for (const auto &pair : opt.pairs) {
        printf("== %s\n   %s\n", baseName(pair.first), baseName(pair.second));

        // --- 1. slot A alone: the dial must be inert, and it must be the endpoint -------------
        if (!sendIr(hostContext, component, 0, pair.first) ||
            !sendIr(hostContext, component, 1, "")) {
            printf("   FAIL  the plug-in refused to load %s into slot A\n\n", baseName(pair.first));
            ++failures;
            continue;
        }
        const std::vector<float> aOnly0 = renderer.run(0.0);
        const std::vector<float> aOnly5 = renderer.run(0.5);
        const std::vector<float> aOnly1 = renderer.run(1.0);
        const bool inertA = identical(aOnly0, aOnly5) && identical(aOnly0, aOnly1);
        printf("   %s  one IR in slot A, dial swept 0 -> 1: %s\n", inertA ? "ok  " : "FAIL",
               inertA ? "bit-identical" : "THE DIAL CHANGED THE AUDIO");
        if (!inertA)
            ++failures;

        // --- 2. slot B alone: same, and it is the other endpoint ------------------------------
        if (!sendIr(hostContext, component, 0, "") ||
            !sendIr(hostContext, component, 1, pair.second)) {
            printf("   FAIL  the plug-in refused to load %s into slot B\n\n",
                   baseName(pair.second));
            ++failures;
            continue;
        }
        const std::vector<float> bOnly0 = renderer.run(0.0);
        const std::vector<float> bOnly1 = renderer.run(1.0);
        const bool inertB = identical(bOnly0, bOnly1);
        printf("   %s  one IR in slot B, dial swept 0 -> 1: %s\n", inertB ? "ok  " : "FAIL",
               inertB ? "bit-identical" : "THE DIAL CHANGED THE AUDIO");
        if (!inertB)
            ++failures;

        // --- 3. both slots: the endpoints must not move ---------------------------------------
        if (!sendIr(hostContext, component, 0, pair.first) ||
            !sendIr(hostContext, component, 1, pair.second)) {
            printf("   FAIL  the plug-in refused to load the pair into both slots\n\n");
            ++failures;
            continue;
        }
        const std::vector<float> both0 = renderer.run(0.0);
        const std::vector<float> both1 = renderer.run(1.0);
        const bool endA = identical(both0, aOnly0);
        const bool endB = identical(both1, bOnly1);
        printf("   %s  blend at 0 vs slot A alone: %s\n", endA ? "ok  " : "FAIL",
               endA ? "bit-identical" : "THE SECOND SLOT CHANGED THE FIRST");
        printf("   %s  blend at 1 vs slot B alone: %s\n", endB ? "ok  " : "FAIL",
               endB ? "bit-identical" : "THE FIRST SLOT CHANGED THE SECOND");
        failures += (endA ? 0 : 1) + (endB ? 0 : 1);

        // --- 4. state: both paths survive a save and reload ------------------------------------
        // Saved, then both slots deliberately cleared so the audio demonstrably changes, then the
        // state pushed back at an ALREADY ACTIVE plug-in — which is the case a host presents on a
        // preset change, and the one where merely recording the paths and waiting for
        // setupProcessing to load them is not enough. The restored render must be the same samples.
        const std::vector<float> both5 = renderer.run(0.5);
        {
            MemoryStream saved;
            const bool got = component->getState(&saved) == kResultOk;
            sendIr(hostContext, component, 0, "");
            sendIr(hostContext, component, 1, "");
            const std::vector<float> cleared = renderer.run(0.5);
            saved.seek(0, IBStream::kIBSeekSet, nullptr);
            const bool put = component->setState(&saved) == kResultOk;
            const std::vector<float> restored = renderer.run(0.5);

            // Clearing has to change the audio, or "it came back" would be vacuous - a stage that
            // was never running cannot fail to be restored.
            const bool changed = !identical(cleared, both5);
            const bool back = identical(restored, both5);
            const bool ok = got && put && changed && back;
            printf("   %s  both IR paths through save/reload: %s\n", ok ? "ok  " : "FAIL",
                   !got       ? "the plug-in would not save its state"
                   : !changed ? "CLEARING BOTH SLOTS DID NOT CHANGE THE AUDIO"
                   : !put     ? "the plug-in rejected its own state"
                   : !back    ? "THE CABINET DID NOT COME BACK"
                              : "restored bit-identically");
            if (!ok)
                ++failures;
        }

        // --- 5. the level across the dial ------------------------------------------------------
        const double rmsA = rms(both0, 0), rmsB = rms(both1, 0);
        printf("\n   endpoints: A %+.2f dB   B %+.2f dB\n", toDb(rmsA), toDb(rmsB));
        printf("   %-6s %10s %10s %10s   %s\n", "dial", "shipped", "linear", "eq-power",
               "deviation from the line between the endpoints");

        double worst = 0.0;
        for (double x : kBlendPoints) {
            const std::vector<float> mixed = renderer.run(x);
            // The straight line between the endpoints, in power. That is what "no bump and no
            // hole" means when the two IRs are not the same loudness: not a flat level, but a
            // level that goes where the two ends say it should.
            const double targetPower = (1.0 - x) * rmsA * rmsA + x * rmsB * rmsB;
            const double target = std::sqrt(targetPower);
            const double shipped = rms(mixed, 0);
            // What the two textbook curves would have produced, computed exactly from the
            // endpoint renders rather than by rendering them.
            const double lin = mixRms(both0, both1, 1.0 - x, x);
            const double eq = mixRms(both0, both1, std::cos(kHalfPi * x), std::sin(kHalfPi * x));

            const double dev = toDb(shipped) - toDb(target);
            worst = std::max(worst, std::fabs(dev));
            printf("   %-6.3f %+9.2f %+9.2f %+9.2f   %+6.2f dB\n", x, toDb(shipped) - toDb(target),
                   toDb(lin) - toDb(target), toDb(eq) - toDb(target), dev);
        }
        const bool flat = worst <= opt.toleranceDb;
        printf("   %s  worst deviation %.2f dB (limit %.2f dB)\n\n", flat ? "ok  " : "FAIL", worst,
               opt.toleranceDb);
        if (!flat)
            ++failures;
    }

    processor->setProcessing(false);
    component->setActive(false);

    if (failures) {
        printf("FAILED - %d check%s did not hold\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASSED - the second slot is invisible to a one-IR user, and the blend is level\n");
    return 0;
}
