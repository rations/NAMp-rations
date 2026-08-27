// rations_midicheck — the offline proof for MIDI channel switching and the learn table.
//
// The phase's real gate is a player's own four-button footswitch changing channels with the
// editor closed, and that is not a thing this machine can do. What it CAN do is everything
// between the pedal and the audio, against the BUILT BUNDLE and through the same routes a host
// uses — which is where all the behaviour that is easy to get wrong actually lives:
//
//   * a Control Change arrives as a PARAMETER CHANGE on one of the 128 mapped parameters, not as
//     MIDI, so it is sent here through the same IParameterChanges queue a DAW fills;
//   * a Program Change arrives as a parameter change on the program list's own parameter;
//   * only a Note On arrives as an actual event, and only a Note On carries a MIDI channel.
//
// The three routes are asserted separately because they ARE separate, and a test that only
// exercised notes would pass over the two that a footswitch is most likely to send.
//
// What is checked, in order, with the reason each one is here:
//
//   1. Learning captures the first message the plug-in sees, and the press that teaches a row
//      does NOT also perform it. A row taught by stamping on a pedal that also switched the amp
//      would change the channel under a player who was setting up, not playing.
//   2. A learned binding then switches the channel, and the plug-in REPORTS that it did, through
//      the output parameter queue. A parameter the plug-in changes by itself and does not report
//      leaves the host's automation lane disagreeing with the audio.
//   3. A CC fires on the press and not on the release, and not once per block while a foot rests
//      on a latching pedal.
//   4. A note binding is pinned to its MIDI channel and a note on another channel does not match
//      it — the one discrimination VST3 leaves possible, so the one that has to work.
//   5. Teaching a message that another row already answers to takes it away from that row. One
//      button, one meaning.
//   6. The table survives a save and reload, an old (version 1) state blob loads with an empty
//      table rather than failing, and a reloaded project is never left ARMED — a session that
//      reopened still listening would learn whatever the player pressed next.
//   7. It reaches the AUDIO: after the banks are built, a learned pedal press moves the channel
//      that is actually sounding, not just the parameter.
//
// Usage:
//   rations_midicheck <Rations.vst3> [--rate 48000] [--block 128] [--settle-ms 6000]

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/eventlist.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include "public.sdk/source/common/memorystream.h"

#include "midilearn.h"
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
using namespace Rations;

namespace
{

struct Options {
    std::string bundle;
    double rate = 48000.0;
    int block = 128;
    int settleMs = 6000;
};

int gFailures = 0;

void ok(const char *what)
{
    printf("  ok    %s\n", what);
}

void fail(const char *what, const char *detail = nullptr)
{
    if (detail)
        printf("  FAIL  %s (%s)\n", what, detail);
    else
        printf("  FAIL  %s\n", what);
    ++gFailures;
}

void check(bool condition, const char *what, const char *detail = nullptr)
{
    if (condition)
        ok(what);
    else
        fail(what, detail);
}

bool parseArgs(int argc, char **argv, Options &opt)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](double &dst) {
            if (i + 1 >= argc)
                return false;
            dst = atof(argv[++i]);
            return true;
        };
        if (a == "--rate") {
            if (!next(opt.rate))
                return false;
        } else if (a == "--block") {
            double v = 0;
            if (!next(v))
                return false;
            opt.block = static_cast<int>(v);
        } else if (a == "--settle-ms") {
            double v = 0;
            if (!next(v))
                return false;
            opt.settleMs = static_cast<int>(v);
        } else if (!a.empty() && a[0] != '-' && opt.bundle.empty()) {
            opt.bundle = a;
        } else {
            fprintf(stderr, "rations_midicheck: unexpected argument '%s'\n", a.c_str());
            return false;
        }
    }
    if (opt.bundle.empty()) {
        fprintf(stderr, "usage: rations_midicheck <Rations.vst3> [--rate R] [--block N] "
                        "[--settle-ms MS]\n");
        return false;
    }
    if (opt.block < 16 || opt.block > 8192 || opt.rate < 8000.0)
        return false;
    return true;
}

// One IConnectionPoint message at the component, the way the editor sends them. `row` is written
// only when the message carries one.
bool sendMessage(Vst::HostApplication &host, Vst::IComponent *component, const char *messageID,
                 const int *row)
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
    msg->setMessageID(messageID);
    if (row)
        msg->getAttributes()->setInt(kMidiRowAttr, *row);
    return cp->notify(msg) == kResultOk;
}

// What one block of processing is told and what it answers back. Everything the pedal does
// travels one of these two ways, so this is the whole interface under test.
struct Block {
    // In.
    std::vector<std::pair<Vst::ParamID, double>> params;
    std::vector<Vst::Event> events;
    // Out.
    bool sawChannelEcho = false;
    double channelEcho = 0.0;
    double activeChannel = -1.0;
};

struct Harness {
    Vst::IAudioProcessor *processor = nullptr;
    int block = 128;

    void run(Block &b, int repeats = 1)
    {
        std::vector<float> in(static_cast<size_t>(block), 0.05f);
        std::vector<float> outL(static_cast<size_t>(block)), outR(static_cast<size_t>(block));
        float *inPtrs[1] = {in.data()};
        float *outPtrs[2] = {outL.data(), outR.data()};

        Vst::AudioBusBuffers inBus = {};
        inBus.numChannels = 1;
        inBus.channelBuffers32 = inPtrs;
        Vst::AudioBusBuffers outBus = {};
        outBus.numChannels = 2;
        outBus.channelBuffers32 = outPtrs;

        Vst::ParameterChanges paramChanges;
        paramChanges.setMaxParameters(16);
        Vst::ParameterChanges outParamChanges;
        outParamChanges.setMaxParameters(16);
        Vst::EventList eventList;
        eventList.setMaxSize(16);

        Vst::ProcessData data = {};
        data.processMode = Vst::kOffline;
        data.symbolicSampleSize = Vst::kSample32;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &inBus;
        data.outputs = &outBus;
        data.numSamples = static_cast<int32>(block);
        data.inputParameterChanges = &paramChanges;
        data.outputParameterChanges = &outParamChanges;
        data.inputEvents = &eventList;

        b.sawChannelEcho = false;
        b.channelEcho = 0.0;
        b.activeChannel = -1.0;

        for (int r = 0; r < repeats; ++r) {
            paramChanges.clearQueue();
            int32 qi = 0, pi = 0;
            for (const auto &p : b.params)
                if (auto *q = paramChanges.addParameterData(p.first, qi))
                    q->addPoint(0, p.second, pi);

            eventList.clear();
            for (Vst::Event e : b.events)
                eventList.addEvent(e);

            outParamChanges.clearQueue();
            processor->process(data);

            // The plug-in reporting a parameter it changed by itself, and the hidden read-only
            // parameter that says which channel is actually SOUNDING - the request and the answer,
            // which are not the same thing while a switch is being held.
            for (int32 i = 0; i < outParamChanges.getParameterCount(); ++i) {
                Vst::IParamValueQueue *q = outParamChanges.getParameterData(i);
                if (!q || q->getPointCount() <= 0)
                    continue;
                int32 off = 0;
                Vst::ParamValue v = 0.0;
                if (q->getPoint(q->getPointCount() - 1, off, v) != kResultTrue)
                    continue;
                if (q->getParameterId() == kChannelId) {
                    b.sawChannelEcho = true;
                    b.channelEcho = v;
                } else if (q->getParameterId() == kActiveChannelId) {
                    b.activeChannel = v;
                }
            }
        }
    }
};

// A CC as it actually arrives: a parameter change on the mapped parameter, value normalized over
// 0 .. 127.
std::pair<Vst::ParamID, double> cc(int number, int value)
{
    return {static_cast<Vst::ParamID>(kMidiCcBaseId + number), value / 127.0};
}

std::pair<Vst::ParamID, double> pc(int program)
{
    return {static_cast<Vst::ParamID>(kMidiProgramChangeId),
            program / static_cast<double>(kMidiProgramCount - 1)};
}

Vst::Event noteOn(int channel, int pitch, float velocity = 0.8f)
{
    Vst::Event e = {};
    e.type = Vst::Event::kNoteOnEvent;
    e.busIndex = 0;
    e.sampleOffset = 0;
    e.noteOn.channel = static_cast<int16>(channel);
    e.noteOn.pitch = static_cast<int16>(pitch);
    e.noteOn.velocity = velocity;
    e.noteOn.noteId = -1;
    return e;
}

// The value kChannelId takes for channel `c` - the same arithmetic the plug-in's own table uses,
// written out here so the two are compared rather than shared.
double channelValue(int c)
{
    return static_cast<double>(c) / static_cast<double>(kChannelCount - 1);
}

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
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
        fprintf(stderr, "rations_midicheck: cannot load %s\n  %s\n", opt.bundle.c_str(),
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
        fprintf(stderr, "rations_midicheck: no audio effect class in %s\n", opt.bundle.c_str());
        return 1;
    }

    Vst::IComponent *component = provider->getComponent();
    FUnknownPtr<Vst::IAudioProcessor> processor(component);
    if (!component || !processor) {
        fprintf(stderr, "rations_midicheck: no audio processor\n");
        return 1;
    }

    // The event input bus is the whole reason any of this arrives. Its absence would not be a
    // wrong answer later on, it would be silence with no error anywhere, so it is checked first
    // and by asking the plug-in rather than by reading the source.
    printf("buses\n");
    check(component->getBusCount(Vst::kEvent, Vst::kInput) == 1, "one event input bus is declared");

    Vst::SpeakerArrangement in = Vst::SpeakerArr::kMono, out = Vst::SpeakerArr::kStereo;
    processor->setBusArrangements(&in, 1, &out, 1);
    component->activateBus(Vst::kAudio, Vst::kInput, 0, true);
    component->activateBus(Vst::kAudio, Vst::kOutput, 0, true);
    component->activateBus(Vst::kEvent, Vst::kInput, 0, true);

    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kOffline;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(opt.block);
    setup.sampleRate = opt.rate;
    if (processor->setupProcessing(setup) != kResultOk) {
        fprintf(stderr, "rations_midicheck: setupProcessing refused\n");
        return 1;
    }
    component->setActive(true);
    processor->setProcessing(true);

    Harness rig;
    rig.processor = processor;
    rig.block = opt.block;

    // Rows are Clean, Crunch, OD1, OD2 - the order kMidiLearnRows is written in.
    const int kClean = 0, kCrunch = 1, kOd1 = 2, kOd2 = 3;

    // --- 1. learning captures, and does not perform ------------------------------------------
    printf("learn\n");
    check(sendMessage(hostContext, component, kMsgMidiLearn, &kOd1),
          "a row can be armed for learning");
    {
        Block b;
        b.params.push_back(cc(80, 127));
        rig.run(b);
        check(!b.sawChannelEcho, "the press that teaches a row does not also perform it");
    }
    {
        // Now the same pedal, released and pressed again. The release must not fire, and the
        // second press must.
        Block release;
        release.params.push_back(cc(80, 0));
        rig.run(release);
        check(!release.sawChannelEcho, "a CC release does not switch the channel");

        Block press;
        press.params.push_back(cc(80, 127));
        rig.run(press);
        check(press.sawChannelEcho && near(press.channelEcho, channelValue(kOd1)),
              "a learned CC switches the channel and the plug-in reports it");
    }
    {
        // Held down: the same value arriving block after block is one press, not many.
        Block held;
        held.params.push_back(cc(80, 127));
        rig.run(held, 8);
        check(!held.sawChannelEcho, "a held CC fires once, not once per block");
    }

    // --- 2. Program Change -------------------------------------------------------------------
    printf("program change\n");
    check(sendMessage(hostContext, component, kMsgMidiLearn, &kCrunch), "arm a row for a PC");
    {
        Block teach;
        teach.params.push_back(pc(7));
        rig.run(teach);
        check(!teach.sawChannelEcho, "the PC that teaches a row does not perform it");

        Block use;
        use.params.push_back(pc(7));
        rig.run(use);
        check(use.sawChannelEcho && near(use.channelEcho, channelValue(kCrunch)),
              "a learned Program Change switches the channel");

        Block other;
        other.params.push_back(pc(8));
        rig.run(other);
        check(!other.sawChannelEcho, "a different Program Change number does nothing");
    }

    // --- 3. Note On, and the channel it came in on -------------------------------------------
    printf("note on\n");
    check(sendMessage(hostContext, component, kMsgMidiLearn, &kOd2), "arm a row for a note");
    {
        Block teach;
        teach.events.push_back(noteOn(2, 60));
        rig.run(teach);
        check(!teach.sawChannelEcho, "the note that teaches a row does not perform it");

        Block same;
        same.events.push_back(noteOn(2, 60));
        rig.run(same);
        check(same.sawChannelEcho && near(same.channelEcho, channelValue(kOd2)),
              "a learned note on its own channel switches the channel");

        // The one discrimination VST3 leaves possible, because NoteOnEvent is the only one of the
        // three routes that still carries a channel.
        Block otherChannel;
        otherChannel.events.push_back(noteOn(5, 60));
        rig.run(otherChannel);
        check(!otherChannel.sawChannelEcho, "the same note on another MIDI channel does not match");

        // Velocity zero is a note off written as a note on. Acting on it would switch the channel
        // when the player's foot came UP.
        Block noteOff;
        noteOff.events.push_back(noteOn(2, 60, 0.0f));
        rig.run(noteOff);
        check(!noteOff.sawChannelEcho, "a zero-velocity note on is a note off and does nothing");
    }

    // --- 4. one button, one meaning ----------------------------------------------------------
    printf("uniqueness\n");
    check(sendMessage(hostContext, component, kMsgMidiLearn, &kClean),
          "arm a fourth row for a message another row already has");
    {
        Block teach;
        teach.params.push_back(cc(80, 127));
        rig.run(teach);

        Block release;
        release.params.push_back(cc(80, 0));
        rig.run(release);

        Block press;
        press.params.push_back(cc(80, 127));
        rig.run(press);
        check(press.sawChannelEcho && near(press.channelEcho, channelValue(kClean)),
              "the message now performs the row that learned it last");
    }
    // The check above is also the proof that OD1 LOST the binding, which is the half that
    // matters. Rows are matched in order, and the block records the last echo it saw, so if both
    // Clean (row 0) and OD1 (row 2) still answered to CC 80 the value read back would be OD1's.
    // It is Clean's, so exactly one row answers.

    // --- 5. state ----------------------------------------------------------------------------
    printf("state\n");
    {
        MemoryStream saved;
        check(component->getState(&saved) == kResultOk, "state saves");
        saved.seek(0, IBStream::kIBSeekSet, nullptr);

        // Clear every row, prove the pedal has stopped working, then restore and prove it works
        // again. Without the middle step "it came back" could pass on a table that never left.
        for (int row = 0; row < kMidiLearnRowCount; ++row)
            sendMessage(hostContext, component, kMsgMidiClear, &row);
        {
            Block release;
            release.params.push_back(cc(80, 0));
            rig.run(release);
            Block press;
            press.params.push_back(cc(80, 127));
            rig.run(press);
            check(!press.sawChannelEcho, "a cleared row stops answering");
        }

        check(component->setState(&saved) == kResultOk, "state reloads");
        {
            Block release;
            release.params.push_back(cc(80, 0));
            rig.run(release);
            Block press;
            press.params.push_back(cc(80, 127));
            rig.run(press);
            check(press.sawChannelEcho && near(press.channelEcho, channelValue(kClean)),
                  "the table survives save and reload");
        }

        // A reloaded project must never be left listening. Arm a row, reload, and check that the
        // next message is not swallowed as a lesson - it should either do nothing or perform an
        // existing binding, but it must not be captured.
        sendMessage(hostContext, component, kMsgMidiLearn, &kOd1);
        saved.seek(0, IBStream::kIBSeekSet, nullptr);
        check(component->setState(&saved) == kResultOk, "state reloads over an armed row");
        {
            Block release;
            release.params.push_back(cc(99, 0));
            rig.run(release);
            Block press;
            press.params.push_back(cc(99, 127));
            rig.run(press);
            check(!press.sawChannelEcho, "an unlearned CC still does nothing after the reload");

            // If the reload had left OD1 armed, CC 99 would now be bound to it. Press it again:
            // still nothing.
            Block release2;
            release2.params.push_back(cc(99, 0));
            rig.run(release2);
            Block press2;
            press2.params.push_back(cc(99, 127));
            rig.run(press2);
            check(!press2.sawChannelEcho, "reloading a project does not leave a row listening");
        }
    }

    // A state blob from before the MIDI table existed. Written by hand rather than kept as a
    // fixture: what matters is the shape a version 1 writer produced, and that is a version word,
    // the parameter block, and two IR paths.
    {
        MemoryStream v1;
        auto put32 = [&](int32 v) {
            int32 done = 0;
            v1.write(&v, sizeof(v), &done);
        };
        auto putDouble = [&](double v) {
            int32 done = 0;
            v1.write(&v, sizeof(v), &done);
        };
        put32(1);
        for (int i = 0; i < 8; ++i)
            putDouble(i == 7 ? 1.0 : 0.5); // the eight shared controls, gate on
        putDouble(0.0);                    // channel
        for (int c = 0; c < kChannelCount; ++c)
            putDouble(0.0); // the four gain dials
        putDouble(0.0);     // blend
        for (int slot = 0; slot < kIrSlotCount; ++slot)
            put32(0); // two empty IR paths, as writeStr8 writes them
        v1.seek(0, IBStream::kIBSeekSet, nullptr);
        check(component->setState(&v1) == kResultOk,
              "a state blob from before the MIDI table still loads");
        Block release;
        release.params.push_back(cc(80, 0));
        rig.run(release);
        Block press;
        press.params.push_back(cc(80, 127));
        rig.run(press);
        check(!press.sawChannelEcho, "... and loads with an empty table, not a stale one");
    }

    // --- 6. it reaches the audio -------------------------------------------------------------
    //
    // Everything above is about the parameter. This is about the amp: after the banks are built,
    // a pedal press has to move the channel that is actually SOUNDING, which is a different
    // parameter and is held back until the target channel's capture exists.
    printf("audio\n");
    {
        printf("        (waiting %d ms for the banks)\n", opt.settleMs);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(opt.settleMs);
        Block idle;
        while (std::chrono::steady_clock::now() < deadline)
            rig.run(idle, 32);

        int od2 = kOd2;
        sendMessage(hostContext, component, kMsgMidiLearn, &od2);
        Block teach;
        teach.params.push_back(cc(90, 127));
        rig.run(teach);
        Block release;
        release.params.push_back(cc(90, 0));
        rig.run(release);

        Block press;
        press.params.push_back(cc(90, 127));
        rig.run(press);
        check(press.sawChannelEcho && near(press.channelEcho, channelValue(kOd2)),
              "the pedal asks for OD2");

        // The switch is deliberately not instant - the incoming model is primed from the input
        // ring first - so the sounding channel is polled rather than read once.
        Block settle;
        bool arrived = false;
        for (int i = 0; i < 400 && !arrived; ++i) {
            rig.run(settle, 8);
            arrived = near(settle.activeChannel, channelValue(kOd2));
        }
        check(arrived, "and OD2 becomes the channel that is sounding");
    }

    processor->setProcessing(false);
    component->setActive(false);

    printf("\n%s\n", gFailures == 0 ? "rations_midicheck: PASSED" : "rations_midicheck: FAILED");
    return gFailures == 0 ? 0 : 1;
}
