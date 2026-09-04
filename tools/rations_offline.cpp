// rations_offline — headless host for the built Rations bundle.
//
// Loads the plug-in the way a DAW does (module -> factory -> PlugProvider, which connects the
// component and controller so IConnectionPoint messages actually flow), renders a test signal,
// and reports what came out.
//
// This exists because "it compiles and the validator is happy" does not prove the DSP chain is
// wired up. It answers four questions that nothing else does: did the banks load, is the output
// finite and non-trivial, does an over-sized block get looped rather than truncated, and what does
// the whole chain cost in real time.
//
// The banks are HANDED OVER now rather than found. This plug-in used to ship four of them inside
// its own bundle, so a headless host got them for free and this tool only had to check that the
// resource lookup had worked; it ships none, and every bank is a folder the user picks. So the
// four are loaded here through the same IConnectionPoint messages the settings page sends, and
// bank progress is still read back out of the parameter the processor writes — the difference is
// that a zero now means the directory was wrong rather than that the build was.
//
// Usage:
//   rations_offline <NAMp-rations.vst3> --captures <dir> [--rate 48000] [--block 256]
//                   [--seconds 2.0] [--gain 0.0] [--sweep] [--overrun N] [--settle-ms N]
//                   [--dump <file>] [--save-state <file>] [--load-state <file>]
//
// --dump writes the rendered output as raw little-endian float32, which is how one build is
// compared against another BIT FOR BIT rather than by eye over six printed digits. A change that
// is meant to alter nothing — a refactor, or a stage added and left switched off — is proved by
// `cmp` on two dumps, not by two summaries that agree.

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"

#include "public.sdk/source/common/memorystream.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include "rationsids.h"
#include "toolcaptures.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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
    // A directory holding one subdirectory per channel. Defaults to $RATIONS_TEST_CAPTURES.
    std::string captures;
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
    // Where to write the raw float32 render, if anywhere. This is how one build is compared
    // against another bit for bit; see the usage note at the top.
    std::string dump;
    // A state blob to write after the render, and one to push at the plug-in before it.
    std::string saveState;
    std::string loadState;
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
        } else if (a == "--captures") {
            const char *v = next("--captures");
            if (!v)
                return false;
            o.captures = v;
        } else if (a == "--save-state") {
            const char *v = next("--save-state");
            if (!v)
                return false;
            o.saveState = v;
        } else if (a == "--load-state") {
            const char *v = next("--load-state");
            if (!v)
                return false;
            o.loadState = v;
        } else if (a == "--dump") {
            const char *v = next("--dump");
            if (!v)
                return false;
            o.dump = v;
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
        fprintf(stderr, "usage: rations_offline <NAMp-rations.vst3> --captures <dir> [--rate N] "
                        "[--block N] [--seconds S] [--gain 0..1] [--sweep] [--overrun N] "
                        "[--settle-ms N]\n");
        return false;
    }
    o.captures = RationsTools::captureRoot(o.captures);
    if (o.captures.empty()) {
        RationsTools::printCaptureUsage("rations_offline");
        return false;
    }
    if (o.block <= 0 || o.rate <= 0.0 || o.seconds <= 0.0) {
        fprintf(stderr, "rations_offline: rate, block and seconds must all be positive\n");
        return false;
    }
    return true;
}

// Every loudness stated by the captures in one channel's directory.
//
// The Normalized assertion below is defined against these: that mode brings each capture's own
// measured loudness to a fixed target, so the gain it applies is exactly target - loudness, and a
// test that measured the move without knowing the loudness could only assert that SOMETHING
// happened.
//
// All of them rather than the one the dial is parked on, and that is deliberate. Knowing which
// entry is entry 0 means reproducing captureFilenameLess, and the only honest ways to do that are
// to duplicate it - which is what this tree avoids on principle - or to link the DSP into a tool
// whose entire point is that it drives the BUILT BUNDLE and links none of it. Matching against the
// whole set costs nothing in strictness: adjacent captures of one amp differ in measured loudness
// by more than a decibel, so a set of eight spans a range no wrong constant lands inside by
// accident, and the tolerance below is 0.02 dB.
std::vector<double> statedLoudness(const std::string &channelDir)
{
    std::vector<double> out;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(channelDir, ec)) {
        if (ec)
            break;
        if (!e.is_regular_file(ec))
            continue;
        std::string ext = e.path().extension().string();
        for (char &ch : ext)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        if (ext != ".nam")
            continue;
        std::ifstream in(e.path());
        if (!in)
            continue;
        nlohmann::json j;
        try {
            in >> j;
        } catch (...) {
            continue; // a capture the plug-in would skip too
        }
        const auto meta = j.value("metadata", nlohmann::json::object());
        const auto it = meta.find("loudness");
        if (it != meta.end() && !it->is_null())
            out.push_back(it->get<double>());
    }
    return out;
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

    // Hand over the four banks, then wait. The load is posted to each channel's own worker, which
    // is what makes the wait one bank's build time rather than four.
    if (!RationsTools::loadCaptureRoot(hostContext, component, opt.captures)) {
        fprintf(stderr, "rations_offline: the plug-in refused a capture directory under %s\n",
                opt.captures.c_str());
        return 1;
    }
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

    // A state blob from another build, pushed at an ALREADY ACTIVE plug-in. That ordering is the
    // point: it is what a host does on a preset change, and it exercises setState's reload paths
    // rather than letting setupProcessing quietly do the work afterwards.
    if (!opt.loadState.empty()) {
        FILE *f = std::fopen(opt.loadState.c_str(), "rb");
        if (!f) {
            fprintf(stderr, "FAIL: cannot open --load-state file %s\n", opt.loadState.c_str());
            return 1;
        }
        std::vector<char> blob;
        char buf[4096];
        size_t got = 0;
        while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
            blob.insert(blob.end(), buf, buf + got);
        std::fclose(f);

        MemoryStream stream(blob.data(), static_cast<TSize>(blob.size()));
        stream.seek(0, IBStream::kIBSeekSet, nullptr);
        const tresult put = component->setState(&stream);
        int32 version = blob.size() >= 4 ? *reinterpret_cast<const int32 *>(blob.data()) : -1;
        printf("load state     %zu bytes, version %d -> %s\n", blob.size(), version,
               put == kResultOk ? "accepted" : "REJECTED");
        if (put != kResultOk) {
            fprintf(stderr, "FAIL: the plug-in rejected a state blob it must be able to read\n");
            return 1;
        }
        // A bank named by the blob is loaded asynchronously, exactly as it is at startup, so the
        // settle wait has to happen after this and not only after the capture messages.
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.settleMs));
    }

    const uint32 latency = processor->getLatencySamples();

    // REPORTED LATENCY MUST NOT MOVE WHEN A FOOTSWITCH IS STOMPED, and this is the one place that
    // can be checked, because it is a question about the built bundle rather than about the DSP:
    // rations_pedalcheck links the pedal sources directly and never sees getLatencySamples at all.
    //
    // The Boost's 4x oversampler costs 4.4 base-rate samples whether or not the pedal is engaged,
    // because its half-band filters run either way, so the figure is reported unconditionally. A
    // latency that changed under a stomp would make the host recompute its delay compensation
    // mid-song, and some hosts glitch when it does.
    //
    // BOTH DIRECTIONS ARE STOMPED, and that is not thoroughness for its own sake. The first
    // version of this only switched the pedal ON, and a deliberately broken build - one reporting
    // the oversampler's latency only while the Boost was engaged - PASSED it, because switching on
    // is the state in which a conditional report and an unconditional one agree. The bug worth
    // catching is the latency DROPPING when the pedal is switched off.
    {
        std::vector<float> quietIn(static_cast<size_t>(opt.block), 0.0f);
        std::vector<float> quietOutL(static_cast<size_t>(opt.block), 0.0f);
        std::vector<float> quietOutR(static_cast<size_t>(opt.block), 0.0f);
        auto latencyAfterStomp = [&](double on) {
            Vst::ParameterChanges stomp;
            int32 qi = 0;
            if (Vst::IParamValueQueue *q = stomp.addParameterData(Rations::kBoostOnId, qi)) {
                int32 pi = 0;
                q->addPoint(0, on, pi);
            }
            float *inPtr = quietIn.data();
            float *outPtrs[2] = {quietOutL.data(), quietOutR.data()};
            Vst::AudioBusBuffers inBus, outBus;
            inBus.numChannels = 1;
            inBus.channelBuffers32 = &inPtr;
            outBus.numChannels = 2;
            outBus.channelBuffers32 = outPtrs;
            Vst::ProcessData pd;
            pd.processMode = Vst::kRealtime;
            pd.symbolicSampleSize = Vst::kSample32;
            pd.numSamples = opt.block;
            pd.numInputs = 1;
            pd.numOutputs = 1;
            pd.inputs = &inBus;
            pd.outputs = &outBus;
            pd.inputParameterChanges = &stomp;
            processor->process(pd);
            return processor->getLatencySamples();
        };
        const uint32 on = latencyAfterStomp(1.0);
        const uint32 off = latencyAfterStomp(0.0);
        if (on != latency || off != latency) {
            fprintf(stderr,
                    "FAIL: reported latency moved under a footswitch stomp - %u at rest, %u with "
                    "the Boost engaged, %u with it bypassed. A host would recompute its delay "
                    "compensation mid-song\n",
                    latency, on, off);
            return 1;
        }
        printf("pedal latency  %u samples, unchanged by a stomp in either direction\n", latency);
    }

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

    double bankProgress = 0.0;
    double activeIndex = 0.0;

    // One pass over the input at a given channel trim, writing into `output`. A lambda rather
    // than straight-line code because the trim check below has to render the SAME material twice
    // at two trims and compare, and a second copy of the render loop would be a second thing to
    // keep correct.
    bool renderFailed = false;
    // Normalized is the plug-in's default and is what every pass uses unless the output-mode check
    // below says otherwise, so the trim measurement above and the render the report describes are
    // both taken in the state the plug-in ships in.
    double outputModeNorm = Rations::normFromOutputMode(Rations::kOutputNormalized);
    // The tone stack and its switch. Both are pushed at their defaults by every pass, so nothing
    // above changes: Bass/Middle/Treble default to the middle of their range and EQ to on, which
    // is exactly what the processor's constructor already holds. The EQ check below is the only
    // caller that moves them.
    double toneNorm = 0.5;
    double eqOnNorm = 1.0;
    auto renderPass = [&](double levelNorm) {
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
            if (Vst::IParamValueQueue *q =
                    paramChanges.addParameterData(Rations::kCleanLevelId, queueIndex)) {
                int32 pointIndex = 0;
                q->addPoint(0, levelNorm, pointIndex);
            }
            // The output section, pushed every block like the rest. It reaches the plug-in through
            // the parameter queue rather than through a message, because that is the route a DAW
            // uses and the route the editor's own radio buttons take.
            if (Vst::IParamValueQueue *q =
                    paramChanges.addParameterData(Rations::kOutputModeId, queueIndex)) {
                int32 pointIndex = 0;
                q->addPoint(0, outputModeNorm, pointIndex);
            }
            for (Vst::ParamID id : {Rations::kBassId, Rations::kMiddleId, Rations::kTrebleId}) {
                if (Vst::IParamValueQueue *q = paramChanges.addParameterData(id, queueIndex)) {
                    int32 pointIndex = 0;
                    q->addPoint(0, toneNorm, pointIndex);
                }
            }
            if (Vst::IParamValueQueue *q =
                    paramChanges.addParameterData(Rations::kToneStackOnId, queueIndex)) {
                int32 pointIndex = 0;
                q->addPoint(0, eqOnNorm, pointIndex);
            }
            outParamChanges.clearQueue();
            data.inputParameterChanges = &paramChanges;
            data.outputParameterChanges = &outParamChanges;

            data.numSamples = static_cast<int32>(n);
            if (processor->process(data) != kResultOk) {
                fprintf(stderr, "rations_offline: process() failed at sample %zu\n", pos);
                renderFailed = true;
                return;
            }
            memcpy(output.data() + pos, outBlockL.data(), n * sizeof(float));

            // What the plug-in says about its own bank. This is the only route the information
            // takes — the processor never calls the controller directly — so reading it here is
            // reading exactly what the editor would show.
            bankProgress = readOutputParam(outParamChanges, Rations::kBankProgressId, bankProgress);
            activeIndex = readOutputParam(outParamChanges, Rations::kActiveIndexId, activeIndex);
        }
    };

    const clock_t t0 = clock();
    renderPass(0.5); // 0 dB: the trim's default, so this pass is the plug-in as it ships
    if (renderFailed)
        return 1;
    const double wallMs = 1000.0 * static_cast<double>(clock() - t0) / CLOCKS_PER_SEC;

    // --- the channel trim ---------------------------------------------------------------------
    // A trim of X dB must move the channel by X dB, end to end: parameter -> denormalize ->
    // dB-to-linear -> the ramp inside the rack -> the output. Every one of those is a place a
    // wrong constant hides while still producing a control that visibly does something, which is
    // why this is measured rather than left to the fact that the slider moves.
    //
    // It runs HERE, while the component is still active, and that is not a detail: process() after
    // setActive(false) is outside the contract and the plug-in does not produce comparable audio
    // through it - measured 0.78 dB down, uniformly, which reads exactly like a broken trim and is
    // not one. Pass one's output is kept and put back, because the report below is about that
    // render rather than about whichever trim was measured last.
    int trimFailures = 0;
    {
        const std::vector<float> reference = output;
        auto rmsOfOutput = [&]() {
            double sq = 0.0;
            for (size_t i = 0; i < total; ++i)
                if (std::isfinite(output[i]))
                    sq += static_cast<double>(output[i]) * output[i];
            return std::sqrt(sq / static_cast<double>(total));
        };

        // The 0 dB reference is rendered AGAIN rather than taken from the pass above, and that is
        // the whole of what makes this measurable. The first pass renders its opening pick attack
        // through a model with no convolution history, every later pass renders it through one
        // holding the tail of the pass before, and this material puts almost all of its energy in
        // that attack - second one of the take is fifty times the RMS of second two. So pass one
        // is not comparable with pass two at all, and comparing them reads as a 0.8 dB trim error
        // that is not there. Every pass from the second on IS comparable with every other,
        // because the trim is applied AFTER the model: it cannot change what the model is fed, so
        // it cannot change the history the next pass inherits.
        renderPass(0.5);
        if (renderFailed)
            return 1;
        const double refRms = rmsOfOutput();

        // Both ends of the range rather than some middling value: a range constant that
        // disagreed between the controller and the processor would be at its most visible there.
        // The tolerance is 0.02 dB - the ramp travels in 15 ms and this is an RMS over seconds,
        // so the ramp's own contribution is far below it.
        const double kTrials[] = {Rations::ranges::kLevelMin, Rations::ranges::kLevelMax};
        for (double db : kTrials) {
            const double norm = (db - Rations::ranges::kLevelMin) /
                                (Rations::ranges::kLevelMax - Rations::ranges::kLevelMin);
            // Twice, and the second one is the measurement: the trim RAMPS to its new value
            // over 15 ms, and 15 ms of a take whose energy is almost all in one pick attack is
            // worth about 0.1 dB of the answer. The second pass begins already at the value, so
            // what it measures is the trim rather than the trim plus its own arrival.
            renderPass(norm);
            renderPass(norm);
            if (renderFailed)
                return 1;
            const double measured = 20.0 * std::log10(rmsOfOutput() / refRms);
            printf("channel trim   %+.1f dB asked, %+.3f dB measured\n", db, measured);
            if (std::fabs(measured - db) > 0.02) {
                fprintf(stderr,
                        "FAIL: a %+.1f dB channel trim moved the output %+.3f dB - the trim's "
                        "range or its dB conversion disagrees somewhere between the parameter "
                        "and the rack\n",
                        db, measured);
                ++trimFailures;
            }
        }
        output = reference;
    }

    // --- the output section --------------------------------------------------------------------
    // Normalized brings each capture's stated loudness to engine::kNormalizedTargetDb, applied per
    // branch inside the crossfade. So with the dial parked, switching from Raw to Normalized must
    // move the output by exactly target - loudness for the capture that is sounding. That is one
    // number, it is written in the .nam, and every stage between the parameter and the audio has to
    // agree about it: the mode's value space, its denormalization, the dB-to-linear conversion, and
    // which side of the mix the compensation is applied on.
    int modeFailures = 0;
    {
        const std::vector<float> reference = output;
        auto rmsOfOutput = [&]() {
            double sq = 0.0;
            for (size_t i = 0; i < total; ++i)
                if (std::isfinite(output[i]))
                    sq += static_cast<double>(output[i]) * output[i];
            return std::sqrt(sq / static_cast<double>(total));
        };
        // Twice per mode, and the second is the measurement, for the same reason the trim check
        // renders twice: the first pass after a change carries the arrival as well as the value.
        auto renderMode = [&](Rations::OutputMode mode) {
            outputModeNorm = Rations::normFromOutputMode(mode);
            renderPass(0.5);
            renderPass(0.5);
            return rmsOfOutput();
        };

        const double raw = renderMode(Rations::kOutputRaw);
        const double normalized = renderMode(Rations::kOutputNormalized);
        const double moved = 20.0 * std::log10(normalized / raw);

        const std::vector<double> loudness =
            statedLoudness(opt.captures + "/" + Rations::kChannelDefaultName[0]);
        double best = 0.0, bestErr = 1e9;
        for (double db : loudness) {
            const double expect = Rations::engine::kNormalizedTargetDb - db;
            if (std::fabs(moved - expect) < std::fabs(bestErr)) {
                bestErr = moved - expect;
                best = db;
            }
        }
        printf("output mode    Raw -> Normalized moved %+.3f dB", moved);
        if (loudness.empty()) {
            printf("  (no capture states a loudness; nothing to check it against)\n");
        } else {
            printf("; nearest capture states %+.3f dB, so %+.3f dB expected\n", best,
                   Rations::engine::kNormalizedTargetDb - best);
            if (std::fabs(bestErr) > 0.02) {
                fprintf(stderr,
                        "FAIL: Normalized moved the output %+.3f dB and no capture in the bank "
                        "asks for that - the closest is off by %+.3f dB. The normalization target, "
                        "the mode's value space or the dB conversion disagrees somewhere between "
                        "the parameter and the engine\n",
                        moved, bestErr);
                ++modeFailures;
            }
        }

        // Calibrated needs the captures to state an output level in dBu, and plenty do not - the
        // trainer writes the field only when the capture was made with a calibrated interface. When
        // it is absent the engine falls back to unity per entry, which is Raw, and there is nothing
        // here to measure. Reported rather than silently skipped: "this bank cannot exercise this
        // mode" and "this mode is broken" look identical in a test that says nothing.
        const double calibrated = renderMode(Rations::kOutputCalibrated);
        const double calMoved = 20.0 * std::log10(calibrated / raw);
        if (std::fabs(calMoved) < 1e-9)
            printf("               Calibrated is inert - no capture states an output level\n");
        else
            printf("               Raw -> Calibrated moved %+.3f dB\n", calMoved);

        outputModeNorm = Rations::normFromOutputMode(Rations::kOutputNormalized);
        output = reference;
    }

    // --- the EQ switch -------------------------------------------------------------------------
    // Bass, Middle and Treble go IN or OUT of circuit together. "Out" is not "flat": the NAM tone
    // stack has no flat setting, so a switch that merely parked the three at their middle would
    // still be colouring the signal, and the difference is exactly the thing a player reaches for
    // this switch to hear. So the claim is stronger than "the output changes":
    //
    //   * with the switch OUT, the three dials are inert BIT FOR BIT. Not "close to inert": the
    //     stage is skipped entirely, so the same input has to give back the same samples, and any
    //     difference at all means the tone stack is still in the path somewhere.
    //   * with the switch IN, the same two dial settings differ. Otherwise the check above would
    //     pass just as well on a build that had removed the tone stack altogether.
    //   * and the two states differ from each other, which is what says the stage was REMOVED and
    //     not merely frozen at whatever it last held.
    //
    // Each state is rendered twice and the second is the measurement, for the same reason the trim
    // and output-mode checks render twice: the first pass after a change carries the arrival as
    // well as the value, and the models need a receptive field of the new material before their
    // output is a function of it alone.
    int eqFailures = 0;
    {
        const std::vector<float> reference = output;
        auto renderTone = [&](double tone, bool eqOn) {
            toneNorm = tone;
            eqOnNorm = eqOn ? 1.0 : 0.0;
            renderPass(0.5);
            renderPass(0.5);
            return output;
        };
        auto maxAbsDiff = [&](const std::vector<float> &a, const std::vector<float> &b) {
            double worst = 0.0;
            for (size_t i = 0; i < total; ++i)
                worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - b[i]));
            return worst;
        };

        const std::vector<float> inMin = renderTone(0.0, true);
        const std::vector<float> inMax = renderTone(1.0, true);
        const std::vector<float> outMin = renderTone(0.0, false);
        const std::vector<float> outMax = renderTone(1.0, false);

        const double movedIn = maxAbsDiff(inMin, inMax);
        const double movedOut = maxAbsDiff(outMin, outMax);
        const double removed = maxAbsDiff(inMin, outMin);
        printf("eq switch      dials move the output by %.6f in circuit, %.6f out; the stage "
               "itself is worth %.6f\n",
               movedIn, movedOut, removed);

        if (movedOut != 0.0) {
            fprintf(stderr,
                    "FAIL: with EQ out, moving Bass/Middle/Treble still changed the output by "
                    "%.9f - the tone stack is not being skipped\n",
                    movedOut);
            ++eqFailures;
        }
        if (movedIn <= 1e-6) {
            fprintf(stderr,
                    "FAIL: with EQ in, moving Bass/Middle/Treble changed the output by only "
                    "%.9f - the tone stack is not in circuit\n",
                    movedIn);
            ++eqFailures;
        }
        if (removed <= 1e-6) {
            fprintf(stderr,
                    "FAIL: switching EQ out changed the output by only %.9f - the stage is being "
                    "frozen rather than removed\n",
                    removed);
            ++eqFailures;
        }

        toneNorm = 0.5;
        eqOnNorm = 1.0;
        output = reference;
    }

    // --- Slim, end to end ---------------------------------------------------------------------
    //
    // The one thing nothing else in this tree measures: that the Slim setting actually reaches the
    // models. It cannot be checked by reading the code, because the value travels as an IMessage,
    // is stored, and is then read by a worker rebuilding a bank on another thread - three seams,
    // any of which can be right in isolation and wrong together.
    //
    // Two assertions when the captures support it: the small variant sounds DIFFERENT from the
    // whole one (so the setting arrives), and coming back to 1.0 reproduces the reference (so it
    // arrives both ways and a rebuild is repeatable rather than merely different). When they do
    // not support it the result is REPORTED rather than passed silently - a capture with one
    // variant is unaffected by construction, so rendering the same audio at both ends of the knob
    // would be a fact about the file and not about the plug-in.
    int slimFailures = 0;
    {
        const std::vector<float> reference = output;
        const bool slimmable = RationsTools::rootStatesSlimmable(opt.captures);
        // A rebuilt bank is republished before any entry is ready, so the channel is at its
        // ramped-silence gate until the workers catch up. Waiting the same settle the load waits
        // is what makes the render that follows a measurement rather than a race.
        auto renderSlim = [&](double slim) {
            RationsTools::sendSlim(hostContext, component, slim);
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.settleMs));
            renderPass(0.5);
            renderPass(0.5);
            return output;
        };
        auto maxAbsDiff = [&](const std::vector<float> &a, const std::vector<float> &b) {
            double worst = 0.0;
            for (size_t i = 0; i < total; ++i)
                worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - b[i]));
            return worst;
        };

        // Three renders and not two, so the baseline is one of THIS block's own: `output` on
        // entry is the main pass, which is not necessarily the same dial position or gain, and
        // comparing against it would be measuring the difference between two experiments. The
        // third render is also the control the second assertion needs - a rebuild at the size it
        // already had must reproduce itself, or "different at 0.0" would prove only that a rebuild
        // changes something.
        // Ask for the capabilities the way the editor does. Without this the controller has never
        // heard how many captures landed or what they are, so everything below would agree with
        // the captures by knowing nothing about them.
        RationsTools::requestCaps(hostContext, component);

        // Whether the CONTROL is offered at all, which is a separate question from whether it
        // works and is the one a player meets first. The editor draws the Slim icon only when a
        // loaded channel's captures are slimmable containers - the older single-variant ones can
        // do nothing with it - and the same fact retitles the parameter for a host's own list.
        // That is the observable this can reach without an editor, and it exercises the whole
        // chain: the bank's parsed sources, the capability message, and the controller.
        {
            Vst::ParameterInfo info = {};
            const int32 count = controller->getParameterCount();
            bool found = false;
            char title[128] = "";
            for (int32 i = 0; i < count && !found; ++i) {
                if (controller->getParameterInfo(i, info) != kResultOk ||
                    info.id != Rations::kSlimId)
                    continue;
                UString(info.title, USTRINGSIZE(info.title)).toAscii(title, sizeof(title));
                found = true;
            }
            const bool offered = found && strcmp(title, "Slim") == 0;
            printf("slim offered   %s, and the captures %s slimmable containers\n",
                   found ? title : "(no Slim parameter)", slimmable ? "ARE" : "are NOT");
            if (!found) {
                fprintf(stderr, "FAIL: the plug-in declares no Slim parameter\n");
                ++slimFailures;
            } else if (offered != slimmable) {
                fprintf(stderr,
                        "FAIL: Slim reads \"%s\" but the loaded captures %s slimmable - the icon "
                        "would be %s\n",
                        title, slimmable ? "ARE" : "are NOT",
                        offered ? "offered for captures that cannot use it"
                                : "hidden from captures that can");
                ++slimFailures;
            }
        }

        const std::vector<float> wholeFirst = renderSlim(1.0);
        const std::vector<float> small = renderSlim(0.0);
        const std::vector<float> wholeAgain = renderSlim(1.0);
        const double changed = maxAbsDiff(wholeFirst, small);
        const double returned = maxAbsDiff(wholeFirst, wholeAgain);

        printf("slim           0.0 moves the output by %.6f; back at 1.0 it returns to within "
               "%.9f%s\n",
               changed, returned, slimmable ? "" : " (captures state ONE variant - inert)");

        if (slimmable && changed <= 1e-6) {
            fprintf(stderr,
                    "FAIL: the captures are SlimmableContainers but Slim 0.0 changed the output "
                    "by only %.9f - the setting is not reaching the models\n",
                    changed);
            ++slimFailures;
        }
        if (returned > 1e-6) {
            fprintf(stderr,
                    "FAIL: returning Slim to 1.0 left the output %.9f from where it started - a "
                    "rebuild at the same size must reproduce the same model\n",
                    returned);
            ++slimFailures;
        }
        output = reference;
    }

    processor->setProcessing(false);
    if (!opt.saveState.empty()) {
        MemoryStream saved;
        if (component->getState(&saved) != kResultOk) {
            fprintf(stderr, "FAIL: the plug-in would not save its state\n");
            return 1;
        }
        FILE *f = std::fopen(opt.saveState.c_str(), "wb");
        if (!f) {
            fprintf(stderr, "FAIL: cannot open --save-state file %s\n", opt.saveState.c_str());
            return 1;
        }
        const size_t bytes = static_cast<size_t>(saved.getSize());
        const size_t wrote = std::fwrite(saved.getData(), 1, bytes, f);
        std::fclose(f);
        if (wrote != bytes) {
            fprintf(stderr, "FAIL: short write to %s\n", opt.saveState.c_str());
            return 1;
        }
        printf("save state     %zu bytes, version %d -> %s\n", bytes,
               bytes >= 4 ? *reinterpret_cast<const int32 *>(saved.getData()) : -1,
               opt.saveState.c_str());
    }

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
    printf("banks          %.0f%% built, active capture index %.3f\n", bankProgress * 100.0,
           activeIndex);
    printf("input  rms     %.6f\n", inRms);
    printf("output rms     %.6f   peak %.6f\n", outRms, peak);
    printf("max |out-in|   %.6f\n", maxDiff);
    printf("max step       %.6f\n", maxStep);
    printf("non-finite     %zu\n", nonFinite);

    if (!opt.dump.empty()) {
        FILE *f = std::fopen(opt.dump.c_str(), "wb");
        if (!f) {
            fprintf(stderr, "FAIL: cannot open --dump file %s\n", opt.dump.c_str());
            return 1;
        }
        const size_t wrote = std::fwrite(output.data(), sizeof(float), total, f);
        std::fclose(f);
        if (wrote != total) {
            fprintf(stderr, "FAIL: short write to %s (%zu of %zu)\n", opt.dump.c_str(), wrote,
                    total);
            return 1;
        }
        printf("dump           %zu samples -> %s\n", total, opt.dump.c_str());
    }
    printf("unwritten      %zu\n", stale);
    printf("wall           %.1f ms  (RTF %.4f)\n", wallMs, wallMs / (opt.seconds * 1000.0));

    int failures = trimFailures + modeFailures + eqFailures + slimFailures;
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
    // A zero here is now a wrong --captures rather than a broken build: the loads above were
    // accepted, so either the directories hold no readable .nam files or --settle-ms was too short
    // for the first model to finish building.
    if (bankProgress <= 0.0) {
        fprintf(stderr,
                "FAIL: nothing built from %s — check that it holds one subdirectory per channel "
                "of readable .nam captures, and that --settle-ms is long enough\n",
                opt.captures.c_str());
        ++failures;
    }
    if (maxDiff <= 1e-6) {
        fprintf(stderr, "FAIL: output is identical to the input — the chain is not in circuit\n");
        ++failures;
    }

    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
