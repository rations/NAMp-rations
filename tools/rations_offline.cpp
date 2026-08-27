// rations_offline — headless host for the built Rations bundle.
//
// Loads the plug-in the way a DAW does (module -> factory -> PlugProvider, which connects the
// component and controller so IConnectionPoint messages actually flow), renders a test signal,
// and reports what came out.
//
// This exists because "it compiles and the validator is happy" does not prove the DSP chain is
// wired up. It answers four questions that nothing else does: did the BUNDLED bank load, is the
// output finite and non-trivial, does an over-sized block get looped rather than truncated, and
// what does the whole chain cost in real time.
//
// The bundled bank is the point. Unlike its parent, this plug-in has no capture browser and no
// file-loading interface — the captures ship inside Contents/Resources/captures and are found by
// the same resource lookup that finds the art. So there is nothing for this tool to hand over,
// and instead it CHECKS: the bank-progress parameter the processor writes back through
// outputParameterChanges is read here, and a bank that failed to resolve reports zero.
//
// Usage:
//   rations_offline <Rations.vst3> [--rate 48000] [--block 256] [--seconds 2.0]
//                   [--gain 0.0] [--sweep] [--overrun N] [--settle-ms N]

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include "rationsids.h"

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
    double rate = 48000.0;
    int block = 256;
    double seconds = 2.0;
    // Deliberately hand process() more frames than setupProcessing promised. A host is not
    // supposed to, but some do, and the correct answer is to loop in sub-blocks. Clamping to the
    // promised size instead leaves the tail of the output buffer holding whatever was in it
    // before, which reads as a burst of stale audio rather than as a dropout.
    int overrun = 0;
    // Dial position, and whether to sweep it across the render. A sweep is the only way to
    // exercise the crossfade through a real host: it is what a user turning the dial looks like
    // from the plug-in's side.
    double gain = 0.0;
    bool sweep = false;
    // How long to let the worker build the bank before rendering. The build is asynchronous by
    // design so a DAW's message thread is never blocked; an offline render has to wait for it, or
    // it measures the ramped silence that precedes the first entry.
    int settleMs = 4000;
};

bool parseArgs(int argc, char **argv, Options &o)
{
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "rations_offline: %s needs a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--gain") {
            const char *v = next("--gain");
            if (!v)
                return false;
            o.gain = atof(v);
        } else if (a == "--sweep") {
            o.sweep = true;
        } else if (a == "--rate") {
            const char *v = next("--rate");
            if (!v)
                return false;
            o.rate = atof(v);
        } else if (a == "--block") {
            const char *v = next("--block");
            if (!v)
                return false;
            o.block = atoi(v);
        } else if (a == "--seconds") {
            const char *v = next("--seconds");
            if (!v)
                return false;
            o.seconds = atof(v);
        } else if (a == "--overrun") {
            const char *v = next("--overrun");
            if (!v)
                return false;
            o.overrun = atoi(v);
        } else if (a == "--settle-ms") {
            const char *v = next("--settle-ms");
            if (!v)
                return false;
            o.settleMs = atoi(v);
        } else if (positional == 0) {
            o.bundle = a;
            ++positional;
        } else {
            fprintf(stderr, "rations_offline: unexpected argument '%s'\n", a.c_str());
            return false;
        }
    }
    if (o.bundle.empty()) {
        fprintf(stderr, "usage: rations_offline <Rations.vst3> [--rate N] [--block N] "
                        "[--seconds S] [--gain 0..1] [--sweep] [--overrun N] [--settle-ms N]\n");
        return false;
    }
    if (o.block <= 0 || o.rate <= 0.0 || o.seconds <= 0.0) {
        fprintf(stderr, "rations_offline: rate, block and seconds must all be positive\n");
        return false;
    }
    return true;
}

// A pick attack followed by a sustained tone and a decay into near-silence. Constant amplitude
// proves nothing: the decay tail is where a DC step, a gate interaction or an unprimed model
// shows up.
void fillTestSignal(std::vector<float> &buf, double rate)
{
    const size_t n = buf.size();
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        double env;
        if (t < 0.002)
            env = t / 0.002; // attack
        else if (t < 0.5)
            env = 1.0;
        else
            env = std::exp(-(t - 0.5) * 6.0); // decay into the noise floor
        const double tone = 0.6 * std::sin(2.0 * M_PI * 110.0 * t) +
                            0.25 * std::sin(2.0 * M_PI * 220.0 * t) +
                            0.12 * std::sin(2.0 * M_PI * 330.0 * t);
        buf[i] = static_cast<float>(0.5 * env * tone);
    }
}

// Last value the processor wrote for one parameter this block, or `fallback` if it wrote none.
double readOutputParam(Vst::ParameterChanges &changes, Vst::ParamID id, double fallback)
{
    for (int32 i = 0; i < changes.getParameterCount(); ++i) {
        Vst::IParamValueQueue *q = changes.getParameterData(i);
        if (!q || q->getParameterId() != id)
            continue;
        const int32 points = q->getPointCount();
        if (points <= 0)
            continue;
        int32 offset = 0;
        Vst::ParamValue value = 0.0;
        if (q->getPoint(points - 1, offset, value) == kResultTrue)
            return value;
    }
    return fallback;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 2;

    // The host context must be published BEFORE the plug-in is instantiated: allocateMessage()
    // asks it for IMessage instances, so without one every processor->controller message is
    // silently dropped.
    Vst::HostApplication hostContext;
    Vst::PluginContextFactory::instance().setPluginContext(&hostContext);

    std::string error;
    auto module = VST3::Hosting::Module::create(opt.bundle, error);
    if (!module) {
        fprintf(stderr, "rations_offline: cannot load %s\n  %s\n", opt.bundle.c_str(),
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
        fprintf(stderr, "rations_offline: no audio effect class in %s\n", opt.bundle.c_str());
        return 1;
    }

    Vst::IComponent *component = provider->getComponent();
    Vst::IEditController *controller = provider->getController();
    FUnknownPtr<Vst::IAudioProcessor> processor(component);
    if (!component || !controller || !processor) {
        fprintf(stderr, "rations_offline: the plug-in did not provide all three parts\n");
        return 1;
    }

    // The bank is requested in initialize() and built on the plug-in's worker thread, so there is
    // nothing to hand over — only something to wait for.
    std::this_thread::sleep_for(std::chrono::milliseconds(opt.settleMs));

    // --- process setup ---------------------------------------------------------------------
    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kOffline;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = opt.block;
    setup.sampleRate = opt.rate;
    if (processor->setupProcessing(setup) != kResultOk) {
        fprintf(stderr, "rations_offline: the plug-in rejected the process setup\n");
        return 1;
    }
    component->setActive(true);
    processor->setProcessing(true);

    const uint32 latency = processor->getLatencySamples();

    // --- render ------------------------------------------------------------------------------
    const size_t total = static_cast<size_t>(opt.seconds * opt.rate);
    std::vector<float> input(total), output(total, 0.0f);
    fillTestSignal(input, opt.rate);

    // The buffers the host hands over may be larger than the size it declared, if we are
    // deliberately over-running to check the sub-block loop.
    const int callBlock = opt.overrun > 0 ? opt.overrun : opt.block;
    std::vector<float> inBlock(static_cast<size_t>(callBlock));
    std::vector<float> outBlockL(static_cast<size_t>(callBlock));
    std::vector<float> outBlockR(static_cast<size_t>(callBlock));
    float *inPtrs[1] = {inBlock.data()};
    float *outPtrs[2] = {outBlockL.data(), outBlockR.data()};

    Vst::AudioBusBuffers inBus = {};
    inBus.numChannels = 1;
    inBus.channelBuffers32 = inPtrs;
    Vst::AudioBusBuffers outBus = {};
    outBus.numChannels = 2;
    outBus.channelBuffers32 = outPtrs;

    // The dial travels the same way it does from a DAW: as a parameter change queue, one point
    // per block. That is deliberately coarse — the engine's slew limiter is what turns
    // block-granular automation into a smooth position, and testing it any other way would not
    // exercise that.
    Vst::ParameterChanges paramChanges;
    paramChanges.setMaxParameters(4);
    Vst::ParameterChanges outParamChanges;
    outParamChanges.setMaxParameters(8);

    Vst::ProcessData data = {};
    data.processMode = Vst::kOffline;
    data.symbolicSampleSize = Vst::kSample32;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;

    double bankProgress = 0.0;
    double activeIndex = 0.0;

    const clock_t t0 = clock();
    for (size_t pos = 0; pos < total; pos += static_cast<size_t>(callBlock)) {
        const size_t n = std::min(static_cast<size_t>(callBlock), total - pos);
        // Poison the output buffers. If the plug-in only fills part of an over-sized block, the
        // poison survives into the result and the stale-tail check below catches it.
        std::fill(outBlockL.begin(), outBlockL.end(), 7.0f);
        std::fill(outBlockR.begin(), outBlockR.end(), 7.0f);
        std::fill(inBlock.begin(), inBlock.end(), 0.0f);
        memcpy(inBlock.data(), input.data() + pos, n * sizeof(float));

        const double sweepPos =
            opt.sweep ? static_cast<double>(pos) / static_cast<double>(total) : opt.gain;
        paramChanges.clearQueue();
        int32 queueIndex = 0;
        if (Vst::IParamValueQueue *q =
                paramChanges.addParameterData(Rations::kCleanGainId, queueIndex)) {
            int32 pointIndex = 0;
            q->addPoint(0, sweepPos, pointIndex);
        }
        outParamChanges.clearQueue();
        data.inputParameterChanges = &paramChanges;
        data.outputParameterChanges = &outParamChanges;

        data.numSamples = static_cast<int32>(n);
        if (processor->process(data) != kResultOk) {
            fprintf(stderr, "rations_offline: process() failed at sample %zu\n", pos);
            return 1;
        }
        memcpy(output.data() + pos, outBlockL.data(), n * sizeof(float));

        // What the plug-in says about its own bank. This is the only route the information takes
        // — the processor never calls the controller directly — so reading it here is reading
        // exactly what the editor would show.
        bankProgress = readOutputParam(outParamChanges, Rations::kBankProgressId, bankProgress);
        activeIndex = readOutputParam(outParamChanges, Rations::kActiveIndexId, activeIndex);
    }
    const double wallMs = 1000.0 * static_cast<double>(clock() - t0) / CLOCKS_PER_SEC;

    processor->setProcessing(false);
    component->setActive(false);

    // --- report ------------------------------------------------------------------------------
    size_t nonFinite = 0;
    double peak = 0.0, sumSq = 0.0, inSumSq = 0.0, maxDiff = 0.0;
    for (size_t i = 0; i < total; ++i) {
        const double y = output[i];
        if (!std::isfinite(y)) {
            ++nonFinite;
            continue;
        }
        peak = std::max(peak, std::fabs(y));
        sumSq += y * y;
        inSumSq += static_cast<double>(input[i]) * input[i];
        maxDiff = std::max(maxDiff, std::fabs(y - input[i]));
    }
    const double outRms = std::sqrt(sumSq / static_cast<double>(total));
    const double inRms = std::sqrt(inSumSq / static_cast<double>(total));
    // Largest single-sample jump: a hard model switch or a hard bypass shows up here as an
    // outlier far above the signal's own slew.
    double maxStep = 0.0;
    for (size_t i = 1; i < total; ++i)
        if (std::isfinite(output[i]) && std::isfinite(output[i - 1]))
            maxStep = std::max(maxStep, std::fabs(static_cast<double>(output[i]) - output[i - 1]));

    // Poison left behind means part of an over-sized block was never written.
    size_t stale = 0;
    for (size_t i = 0; i < total; ++i)
        if (output[i] == 7.0f)
            ++stale;

    printf("rate           %.0f Hz, declared block %d, actual block %d, %.2f s\n", opt.rate,
           opt.block, callBlock, opt.seconds);
    printf("latency        %u samples\n", latency);
    printf("bundled bank   %.0f%% built, active capture index %.3f\n", bankProgress * 100.0,
           activeIndex);
    printf("input  rms     %.6f\n", inRms);
    printf("output rms     %.6f   peak %.6f\n", outRms, peak);
    printf("max |out-in|   %.6f\n", maxDiff);
    printf("max step       %.6f\n", maxStep);
    printf("non-finite     %zu\n", nonFinite);
    printf("unwritten      %zu\n", stale);
    printf("wall           %.1f ms  (RTF %.4f)\n", wallMs, wallMs / (opt.seconds * 1000.0));

    int failures = 0;
    if (nonFinite) {
        fprintf(stderr, "FAIL: %zu non-finite output samples\n", nonFinite);
        ++failures;
    }
    if (stale) {
        fprintf(stderr,
                "FAIL: %zu output samples were never written — an over-sized block was "
                "truncated instead of looped\n",
                stale);
        ++failures;
    }
    if (outRms <= 1e-9) {
        fprintf(stderr, "FAIL: output is silent\n");
        ++failures;
    }
    // The captures ship in the bundle, so unlike the parent plug-in there is no "no capture was
    // given" case to excuse an empty bank. A zero here means the resource lookup did not find
    // Contents/Resources/captures, which is a broken build rather than a missing argument.
    if (bankProgress <= 0.0) {
        fprintf(stderr, "FAIL: the bundled bank reports nothing built — check that the bundle "
                        "has Contents/Resources/captures\n");
        ++failures;
    }
    if (maxDiff <= 1e-6) {
        fprintf(stderr, "FAIL: output is identical to the input — the chain is not in circuit\n");
        ++failures;
    }
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
