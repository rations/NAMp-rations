// rations_jackcheck — the live gate for the channel switch, and the measurement that settles
// engine::kSwitchModelBudget.
//
// That budget is not a number anyone picks. It buys switch latency with audio-thread work, one
// against the other, at a fixed product of one receptive field of model time: the incoming
// channel gets whatever of the budget the sounding channel is not already using, and closes a
// receptive-field gap at (that - 1) samples per sample. So the only question is how many models
// this machine can run inside one JACK period without missing the deadline, and that has to be
// measured under the conditions a plug-in is actually used in — which on this machine means the CPU
// governor left at powersave, where a 2.67 ms burst of work every 2.67 ms never raises the clock
// off its 800 MHz floor while a sustained offline render ramps it to 4800. An offline RTF is
// therefore about 5.7x too optimistic and is the wrong number to budget against; this tool produces
// the right one.
//
// What it runs is the BUILT BUNDLE, loaded the way a DAW loads it, driven from a real JACK process
// callback, with kChannelId stomped on a timer through a parameter-change queue — the same route a
// footswitch or an automation lane takes. Not the rack in isolation: the deadline belongs to the
// whole chain, gate and tone stack and resampler included.
//
// It is DELIBERATELY NOT CONNECTED to playback unless asked. A WaveNet does the same arithmetic
// whatever the input is, so the cost is identical with nothing patched in, and nothing reaches a
// real interface's monitors while the test runs.
//
// Usage:
//   rations_jackcheck <Rations.vst3> [--seconds S] [--stomp-ms N] [--restomp-ms N]
//                     [--settle-ms N] [--connect] [--sweep-gain]

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include "engineconfig.h"
#include "rationsids.h"

#include <jack/jack.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

using namespace Steinberg;

namespace
{

struct Options {
    std::string bundle;
    double seconds = 20.0;
    // How often a channel is stomped. Deliberately longer than a whole switch by default, so the
    // steady state is measured too rather than only the burst.
    int stompMs = 250;
    // A second stomp this long after the first, to land inside the switch window. 0 = off.
    int restompMs = 8;
    int settleMs = 6000;
    bool connect = false;
    // Keep the sounding channel's gain dial moving, which holds TWO of its captures bound instead
    // of one. That cost predates the channel switch — it is what the gain crossfade has always
    // cost while a knob is turning — so measuring it is how you tell "this buffer size cannot
    // afford a switch" from "this buffer size cannot afford this plug-in".
    bool sweepGain = false;
};

// Everything the RT callback touches, all of it built before the callback can run. Nothing here
// is allocated, resized or freed once JACK is active.
struct Rig {
    Vst::IAudioProcessor *processor = nullptr;
    Vst::ProcessData data = {};
    Vst::AudioBusBuffers inBus = {};
    Vst::AudioBusBuffers outBus = {};
    float *inPtrs[1] = {nullptr};
    float *outPtrs[2] = {nullptr, nullptr};
    std::vector<float> outR;
    Vst::ParameterChanges paramChanges;
    Vst::ParameterChanges outParamChanges;

    jack_port_t *portIn = nullptr;
    jack_port_t *portOut = nullptr;

    // Stomp schedule, in frames, advanced on the RT thread only.
    long long framePos = 0;
    long long nextStomp = 0;
    long long nextRestomp = -1;
    int stompFrames = 0;
    int restompFrames = 0;
    bool sweepGain = false;
    int channel = 0;     // the channel most recently asked for
    int lastChannel = 0; // where a re-stomp goes back to
    long long stomps = 0;

    // Switch latency, measured where it actually means something. kChannelId is the REQUEST and
    // kActiveChannelId is the answer - the channel that is genuinely sounding - so the latency is
    // the distance between the block that asked and the block where the answer arrived. Measured
    // here rather than in the offline proof because the offline harness runs the audio thread as
    // fast as it can, which leaves the prime worker permanently behind a "real time" twenty times
    // faster than real time and makes every number it produces about the switch meaningless.
    long long pendingSince = -1; // frame position of the request still unanswered, or -1
    int pendingChannel = -1;
    long long switchCount = 0;
    long long switchFramesTotal = 0;
    std::atomic<long long> switchFramesWorst{0};
    std::atomic<long long> switchFramesMean{0};
    std::atomic<long long> switchesMeasured{0};

    // Worst process() call seen, in nanoseconds, and the period it had to fit in.
    std::atomic<long long> worstNs{0};
    std::atomic<long long> lastProgressPermille{0};
    std::atomic<int> xruns{0};
};

Rig g_rig;

inline long long nowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

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

int processCallback(jack_nframes_t nframes, void *)
{
    Rig &r = g_rig;
    const long long t0 = nowNs();

    float *in = static_cast<float *>(jack_port_get_buffer(r.portIn, nframes));
    float *out = static_cast<float *>(jack_port_get_buffer(r.portOut, nframes));
    r.inPtrs[0] = in;
    r.outPtrs[0] = out;
    r.outPtrs[1] = r.outR.data();

    // The stomp. One parameter point at the top of the block, which is exactly where a host puts
    // an automation change or a MIDI-mapped controller.
    r.paramChanges.clearQueue();
    bool stomped = false;
    int want = r.channel;
    if (r.framePos >= r.nextStomp) {
        r.lastChannel = r.channel;
        want = (r.channel + 1) % Rations::kChannelCount;
        r.nextStomp = r.framePos + r.stompFrames;
        if (r.restompFrames > 0)
            r.nextRestomp = r.framePos + r.restompFrames;
        stomped = true;
    } else if (r.nextRestomp >= 0 && r.framePos >= r.nextRestomp) {
        // A second stomp inside the switch window, back to where it came from. This is the path
        // that must never leave a half-primed model bound.
        want = r.lastChannel;
        r.nextRestomp = -1;
        stomped = true;
    }
    if (stomped) {
        r.channel = want;
        int32 queueIndex = 0;
        if (Vst::IParamValueQueue *q =
                r.paramChanges.addParameterData(Rations::kChannelId, queueIndex)) {
            int32 pointIndex = 0;
            q->addPoint(0,
                        static_cast<double>(want) / static_cast<double>(Rations::kChannelCount - 1),
                        pointIndex);
        }
        ++r.stomps;
    }

    if (r.sweepGain) {
        // A dial that never stops moving, so the auto-detent never collapses the second branch.
        const double phase = static_cast<double>(r.framePos % 96000) / 96000.0;
        int32 queueIndex = 0;
        if (Vst::IParamValueQueue *q =
                r.paramChanges.addParameterData(Rations::kChannelGainId[r.channel], queueIndex)) {
            int32 pointIndex = 0;
            q->addPoint(0, phase, pointIndex);
        }
    }

    r.outParamChanges.clearQueue();
    r.data.numSamples = static_cast<int32>(nframes);
    r.processor->process(r.data);

    r.lastProgressPermille.store(
        static_cast<long long>(1000.0 *
                               readOutputParam(r.outParamChanges, Rations::kBankProgressId, 0.0)),
        std::memory_order_relaxed);

    // Where the request was made, and where the audio actually got to.
    if (stomped) {
        r.pendingSince = r.framePos;
        r.pendingChannel = want;
    }
    if (r.pendingSince >= 0) {
        const double activeNorm =
            readOutputParam(r.outParamChanges, Rations::kActiveChannelId, -1.0);
        if (activeNorm >= 0.0) {
            const int active = static_cast<int>(
                std::lround(activeNorm * static_cast<double>(Rations::kChannelCount - 1)));
            if (active == r.pendingChannel) {
                const long long took = r.framePos + nframes - r.pendingSince;
                ++r.switchCount;
                r.switchFramesTotal += took;
                if (took > r.switchFramesWorst.load(std::memory_order_relaxed))
                    r.switchFramesWorst.store(took, std::memory_order_relaxed);
                r.switchFramesMean.store(r.switchFramesTotal / r.switchCount,
                                         std::memory_order_relaxed);
                r.switchesMeasured.store(r.switchCount, std::memory_order_relaxed);
                r.pendingSince = -1;
            }
        }
    }

    r.framePos += nframes;

    const long long dt = nowNs() - t0;
    if (dt > r.worstNs.load(std::memory_order_relaxed))
        r.worstNs.store(dt, std::memory_order_relaxed);
    return 0;
}

int xrunCallback(void *)
{
    g_rig.xruns.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

bool parseArgs(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        const char *v = nullptr;
        if (a == "--seconds") {
            if (!(v = next()))
                return false;
            o.seconds = atof(v);
        } else if (a == "--stomp-ms") {
            if (!(v = next()))
                return false;
            o.stompMs = atoi(v);
        } else if (a == "--restomp-ms") {
            if (!(v = next()))
                return false;
            o.restompMs = atoi(v);
        } else if (a == "--settle-ms") {
            if (!(v = next()))
                return false;
            o.settleMs = atoi(v);
        } else if (a == "--connect") {
            o.connect = true;
        } else if (a == "--sweep-gain") {
            o.sweepGain = true;
        } else if (o.bundle.empty()) {
            o.bundle = a;
        } else {
            fprintf(stderr, "rations_jackcheck: unexpected argument '%s'\n", a.c_str());
            return false;
        }
    }
    if (o.bundle.empty()) {
        fprintf(stderr, "usage: rations_jackcheck <Rations.vst3> [--seconds S] [--stomp-ms N]\n"
                        "       [--restomp-ms N] [--settle-ms N] [--connect] [--sweep-gain]\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 2;

    // Published before the plug-in is instantiated: allocateMessage() asks the context for
    // IMessage instances, so without one every processor -> controller message is dropped.
    Vst::HostApplication hostContext;
    Vst::PluginContextFactory::instance().setPluginContext(&hostContext);

    std::string error;
    auto module = VST3::Hosting::Module::create(opt.bundle, error);
    if (!module) {
        fprintf(stderr, "rations_jackcheck: cannot load %s\n  %s\n", opt.bundle.c_str(),
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
        fprintf(stderr, "rations_jackcheck: no audio effect class in %s\n", opt.bundle.c_str());
        return 1;
    }

    Vst::IComponent *component = provider->getComponent();
    FUnknownPtr<Vst::IAudioProcessor> processor(component);
    if (!component || !processor) {
        fprintf(stderr, "rations_jackcheck: the plug-in did not provide component + processor\n");
        return 1;
    }

    jack_status_t status;
    jack_client_t *client = jack_client_open("rations_jackcheck", JackNoStartServer, &status);
    if (!client) {
        fprintf(stderr, "rations_jackcheck: no JACK server (status 0x%x)\n",
                static_cast<unsigned>(status));
        return 1;
    }
    const double rate = jack_get_sample_rate(client);
    const jack_nframes_t block = jack_get_buffer_size(client);
    const double periodMs = 1000.0 * block / rate;

    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(block);
    setup.sampleRate = rate;
    if (processor->setupProcessing(setup) != kResultOk) {
        fprintf(stderr, "rations_jackcheck: the plug-in rejected the process setup\n");
        jack_client_close(client);
        return 1;
    }

    Rig &r = g_rig;
    r.processor = processor;
    r.outR.assign(static_cast<size_t>(block), 0.0f);
    r.inBus.numChannels = 1;
    r.inBus.channelBuffers32 = r.inPtrs;
    r.outBus.numChannels = 2;
    r.outBus.channelBuffers32 = r.outPtrs;
    r.data.processMode = Vst::kRealtime;
    r.data.symbolicSampleSize = Vst::kSample32;
    r.data.numInputs = 1;
    r.data.numOutputs = 1;
    r.data.inputs = &r.inBus;
    r.data.outputs = &r.outBus;
    // Pre-sized here, on the message thread. addParameterData()/addPoint() on the RT thread then
    // reuse what is already reserved and never grow a vector — the same property the plug-in's
    // own meter feedback relies on.
    r.paramChanges.setMaxParameters(8);
    r.outParamChanges.setMaxParameters(8);
    r.data.inputParameterChanges = &r.paramChanges;
    r.data.outputParameterChanges = &r.outParamChanges;
    r.sweepGain = opt.sweepGain;
    r.stompFrames = static_cast<int>(opt.stompMs * 0.001 * rate);
    r.restompFrames = opt.restompMs > 0 ? static_cast<int>(opt.restompMs * 0.001 * rate) : 0;
    r.nextStomp = r.stompFrames;

    r.portIn = jack_port_register(client, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    r.portOut = jack_port_register(client, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!r.portIn || !r.portOut) {
        fprintf(stderr, "rations_jackcheck: could not register ports\n");
        jack_client_close(client);
        return 1;
    }
    jack_set_process_callback(client, processCallback, nullptr);
    jack_set_xrun_callback(client, xrunCallback, nullptr);

    printf("server         %.0f Hz, %u frames (%.2f ms period)\n", rate, block, periodMs);
    printf("model budget   %.2f concurrent models, whatever else the audio thread is doing\n",
           Rations::engine::kSwitchModelBudget);
    printf("stomps         every %d ms, with a second stomp %d ms later%s\n", opt.stompMs,
           opt.restompMs, opt.sweepGain ? ", gain dial sweeping" : "");

    // The four banks are built on the plug-in's own workers, so give them time before the audio
    // thread is asked to switch between them. Counting xruns while models are still being built
    // would measure the build, not the switch.
    component->setActive(true);
    processor->setProcessing(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(opt.settleMs));

    if (jack_activate(client) != 0) {
        fprintf(stderr, "rations_jackcheck: could not activate the JACK client\n");
        jack_client_close(client);
        return 1;
    }
    if (opt.connect) {
        const char **playback = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                               JackPortIsPhysical | JackPortIsInput);
        if (playback && playback[0])
            jack_connect(client, jack_port_name(r.portOut), playback[0]);
        if (playback)
            jack_free(playback);
    }

    // Let the first switch happen before the counters are believed: the very first stomp also
    // pays for whatever a channel's first-ever block costs.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    r.worstNs.store(0, std::memory_order_relaxed);
    const int xrunsBefore = r.xruns.load(std::memory_order_relaxed);

    const int seconds = static_cast<int>(opt.seconds);
    double loadSum = 0.0, loadMax = 0.0;
    int loadN = 0;
    for (int i = 0; i < seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const double load = jack_cpu_load(client);
        loadSum += load;
        loadMax = std::max(loadMax, load);
        ++loadN;
    }

    const long long worstNs = r.worstNs.load(std::memory_order_relaxed);
    const int xruns = r.xruns.load(std::memory_order_relaxed) - xrunsBefore;
    const long long stomps = r.stomps;
    const double progress = r.lastProgressPermille.load(std::memory_order_relaxed) / 10.0;

    jack_deactivate(client);
    processor->setProcessing(false);
    component->setActive(false);
    jack_client_close(client);

    printf("\nbanks          %.0f%% built\n", progress);
    printf("stomps         %lld in %d s\n", stomps, seconds);
    printf("dsp load       mean %.1f%%, peak %.1f%%\n", loadN ? loadSum / loadN : 0.0, loadMax);
    printf("worst block    %.2f ms of a %.2f ms period (%.0f%% of the deadline)\n", worstNs / 1e6,
           periodMs, 100.0 * (worstNs / 1e6) / periodMs);
    printf("xruns          %d\n", xruns);

    // The number this phase exists to produce. Measured from the block that carried the parameter
    // change to the block in which the plug-in reported that channel as the one SOUNDING, so it
    // includes the ownership handover, whatever priming was left, and the fade - everything a
    // player's foot has to wait through.
    const long long measured = g_rig.switchesMeasured.load(std::memory_order_relaxed);
    if (measured > 0) {
        const double toMs = 1000.0 / rate;
        printf("switch latency mean %.1f ms, worst %.1f ms over %lld completed switches\n",
               static_cast<double>(g_rig.switchFramesMean.load(std::memory_order_relaxed)) * toMs,
               static_cast<double>(g_rig.switchFramesWorst.load(std::memory_order_relaxed)) * toMs,
               measured);
    } else {
        printf("switch latency no switch completed - the target channel never became the one "
               "sounding\n");
    }

    if (progress < 99.9) {
        printf("\nFAILED - the banks never finished building; raise --settle-ms\n");
        return 1;
    }
    if (stomps <= 0) {
        printf("\nFAILED - no stomps were delivered, so nothing was switched\n");
        return 1;
    }
    if (xruns != 0) {
        printf("\nFAILED - %d xruns while switching. kSwitchModelBudget is too high for this "
               "buffer size: lower it in engineconfig.h and measure again.\n",
               xruns);
        return 1;
    }
    if (worstNs / 1e6 > periodMs) {
        printf("\nFAILED - a block took longer than its period even though JACK did not report an "
               "xrun; there is no headroom left\n");
        return 1;
    }
    printf("\nPASSED - zero xruns while switching channels\n");
    return 0;
}
