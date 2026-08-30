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
#include "base/source/fstreamer.h"

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
using namespace Rations;

namespace
{

struct Options {
    std::string bundle;
    double rate = 48000.0;
    int block = 128;
    int settleMs = 6000;
    // A directory holding one subdirectory per channel. Defaults to $RATIONS_TEST_CAPTURES.
    std::string captures;
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

// Returns what it was told, so a check that GATES something after it can be written as one
// statement rather than as a condition tested twice.
bool check(bool condition, const char *what, const char *detail = nullptr)
{
    if (condition)
        ok(what);
    else
        fail(what, detail);
    return condition;
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
        } else if (a == "--captures") {
            // Read straight off argv rather than through next(), which parses a double: this is
            // the one option here whose value is a path.
            if (i + 1 >= argc)
                return false;
            opt.captures = argv[++i];
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
        fprintf(stderr, "usage: rations_midicheck <Rations.vst3> [--captures <dir>] "
                        "[--rate R] [--block N] [--settle-ms MS]\n");
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
    // Every parameter the plug-in reported changing this block. The two fields above are the
    // channel's, kept because four sections were written against them; the pedal rows need the
    // general form, because what a footswitch row echoes is a value it COMPUTED and the whole
    // point is to check which one.
    std::vector<std::pair<Vst::ParamID, double>> echoes;
};

// What the plug-in said it did to one parameter this block, if it said anything.
bool echoOf(const Block &b, Vst::ParamID id, double &value)
{
    for (const auto &e : b.echoes)
        if (e.first == id) {
            value = e.second;
            return true;
        }
    return false;
}

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
        b.echoes.clear();

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
            b.echoes.clear();
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
                // kActiveChannelId is the plug-in REPORTING, not the plug-in acting, so it is
                // deliberately left out of the echo list: what that list is for is "which
                // parameter did a footswitch move", and a read-only meter is not an answer.
                if (q->getParameterId() != kActiveChannelId)
                    b.echoes.push_back({q->getParameterId(), v});
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

// The four channel trims, read out of a state blob.
//
// This used to take them off the TAIL, on the ground that they were the last thing a version 3 blob
// held and that reading forward would be a second copy of the state layout. Version 4 appends the
// output section and the four capture sources after them, so the tail is now three doubles and four
// variable-length path records, and there is no offset from the end that finds a trim. So it does
// read forward - and, since it has to, it reads forward through everything rather than seeking by
// arithmetic, which is the version of "a second copy of the layout" that fails loudly when the
// layout changes under it instead of returning four plausible wrong numbers.
bool readTrims(MemoryStream &s, double out[kChannelCount],
               std::vector<double> *pedals = nullptr)
{
    s.seek(0, IBStream::kIBSeekSet, nullptr);
    IBStreamer streamer(&s, kLittleEndian);

    int32 version = 0;
    if (!streamer.readInt32(version) || version < 1)
        return false;

    double skip = 0.0;
    // Eight controls, the channel, four gain dials, the blend.
    for (int i = 0; i < 8 + 1 + kChannelCount + 1; ++i)
        if (!streamer.readDouble(skip))
            return false;
    // The two IR paths.
    for (int slot = 0; slot < kIrSlotCount; ++slot) {
        char8 *p = streamer.readStr8();
        if (!p)
            return false;
        delete[] p;
    }
    // The MIDI table, from version 2 - four rows until version 6 gave the block its own count.
    // Spelled out here rather than shared with the plug-in, because walking a blob by a layout
    // this file keeps its own copy of is the whole point: a reader that agreed with the writer by
    // construction would agree with it about a mistake too.
    int32 midiRows = (version >= 2) ? kMidiLearnRowsV2 : 0;
    if (version >= 6 && !streamer.readInt32(midiRows))
        return false;
    if (midiRows < 0 || midiRows > kMidiRowStateMax)
        return false;
    for (int32 row = 0; row < midiRows; ++row) {
        int32 word = 0;
        if (!streamer.readInt32(word))
            return false;
    }
    // A version 1 or 2 blob stops here and has no trims to read. That is not a failure of this
    // helper - it is the case the caller is asserting about - so it answers with the defaults the
    // plug-in itself would apply.
    if (version < 3) {
        for (int c = 0; c < kChannelCount; ++c)
            out[c] = 0.5;
        if (pedals)
            pedals->clear();
        return true;
    }
    for (int c = 0; c < kChannelCount; ++c)
        if (!streamer.readDouble(out[c]))
            return false;
    if (!pedals)
        return true;

    // On to the pedalboard, which means reading past the output section and the four capture
    // sources first. This is where the MIDI block's length prefix is really tested: get that count
    // wrong and everything from here on is read at the wrong offset, so a wrong answer here is a
    // loud failure rather than four plausible numbers.
    pedals->clear();
    if (version < 4)
        return true;
    for (int i = 0; i < 3; ++i) // output mode, calibrate, calibration level
        if (!streamer.readDouble(skip))
            return false;
    for (int c = 0; c < kChannelCount; ++c) {
        int32 isDir = 0;
        if (!streamer.readInt32(isDir))
            return false;
        for (int f = 0; f < 2; ++f) { // the path, then the name override
            char8 *p = streamer.readStr8();
            if (!p)
                return false;
            delete[] p;
        }
    }
    if (version < 5)
        return true;
    int32 count = 0;
    if (!streamer.readInt32(count) || count < 0 || count > kPedalStateMax)
        return false;
    for (int32 i = 0; i < count; ++i) {
        double v = 0.0;
        if (!streamer.readDouble(v))
            return false;
        pedals->push_back(v);
    }
    return true;
}

// Rewrite a state blob's MIDI block: a different version word, a different number of rows, and a
// length prefix only from version 6. Everything else is copied BYTE FOR BYTE, which is what makes
// this a fixture for the reader rather than a second implementation of the writer - the two halves
// of the blob this test is not asking about cannot be got wrong by it.
//
// Two blobs are made this way, and they are the two ends of the compatibility claim: a version 5
// blob with four rows (what every project written before the pedalboard's footswitches looks like)
// and a version 6 blob with more rows than this build has (what a project written by a later build
// with a sixth pedal would look like).
bool rewriteMidiBlock(MemoryStream &src, int32 version, int32 rows, std::vector<char> &out)
{
    const char *bytes = src.getData();
    const int64 size = src.getSize();
    if (!bytes || size <= 0)
        return false;

    src.seek(0, IBStream::kIBSeekSet, nullptr);
    IBStreamer streamer(&src, kLittleEndian);
    int32 srcVersion = 0;
    if (!streamer.readInt32(srcVersion) || srcVersion < 6)
        return false; // this only downgrades from the current writer

    double skip = 0.0;
    for (int i = 0; i < 8 + 1 + kChannelCount + 1; ++i)
        if (!streamer.readDouble(skip))
            return false;
    for (int slot = 0; slot < kIrSlotCount; ++slot) {
        char8 *p = streamer.readStr8();
        if (!p)
            return false;
        delete[] p;
    }
    const int64 midiStart = streamer.tell();
    int32 srcRows = 0;
    if (!streamer.readInt32(srcRows) || srcRows < 0 || srcRows > kMidiRowStateMax)
        return false;
    std::vector<int32> words(static_cast<size_t>(srcRows), 0);
    for (int32 i = 0; i < srcRows; ++i)
        if (!streamer.readInt32(words[static_cast<size_t>(i)]))
            return false;
    const int64 midiEnd = streamer.tell();

    auto put = [&out](const void *p, size_t n) {
        const char *c = static_cast<const char *>(p);
        out.insert(out.end(), c, c + n);
    };
    out.clear();
    put(&version, sizeof(version));
    put(bytes + sizeof(int32), static_cast<size_t>(midiStart) - sizeof(int32));
    if (version >= 6)
        put(&rows, sizeof(rows));
    for (int32 i = 0; i < rows; ++i) {
        // Rows the source does not have are written UNLEARNED, which is what a build with more
        // rows than the source would itself have stored for them.
        const int32 w = (i < srcRows) ? words[static_cast<size_t>(i)] : 0;
        put(&w, sizeof(w));
    }
    put(bytes + midiEnd, static_cast<size_t>(size - midiEnd));
    return true;
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

// A footswitch press, as a controller actually sends one: the release first, so that the 127 is a
// RISING edge. Nothing else is acted on - a held pedal must fire once and not once per block - so a
// press sent without a release ahead of it would silently do nothing at all.
Block stomp(Harness &rig, int number)
{
    Block release;
    release.params.push_back(cc(number, 0));
    rig.run(release);
    Block press;
    press.params.push_back(cc(number, 127));
    rig.run(press);
    return press;
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
    // The EDITOR half. Its setComponentState walks the same blob the processor's setState does and
    // has to walk it in the same steps, or every field after the first disagreement is read at the
    // wrong offset. Nothing in this tree tested that until the MIDI table's length prefix made it
    // possible to get wrong - and the SDK validator does not: a controller reading the blob wrongly
    // still returns kResultOk, so all 47 of its tests pass while the editor shows the wrong panel.
    Vst::IEditController *controller = provider->getController();
    if (!controller) {
        fprintf(stderr, "rations_midicheck: no edit controller\n");
        return 1;
    }

    // A learned footswitch has to reach the channel that is actually SOUNDING, not merely the
    // parameter, and this tool asserts exactly that — so the four channels have to have something
    // to sound. With nothing loaded every channel is ramped silence and the assertion would pass
    // against four identical nothings.
    opt.captures = RationsTools::captureRoot(opt.captures);
    if (opt.captures.empty()) {
        RationsTools::printCaptureUsage("rations_midicheck");
        return 1;
    }
    if (!RationsTools::loadCaptureRoot(hostContext, component, opt.captures)) {
        fprintf(stderr, "rations_midicheck: the plug-in refused a capture directory under %s\n",
                opt.captures.c_str());
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

        // Quiet between the messages, because that is what a stomp looks like. The SAME program
        // number in the very next block is a host resending rather than a foot arriving twice, and
        // the plug-in is entitled to tell the difference - a player cannot press twice inside one
        // 2.67 ms period. This test used to send them back to back and it was the wrong sequence.
        Block quiet;
        rig.run(quiet, 8);

        Block use;
        use.params.push_back(pc(7));
        rig.run(use);
        check(use.sawChannelEcho && near(use.channelEcho, channelValue(kCrunch)),
              "a learned Program Change switches the channel");

        rig.run(quiet, 8);
        Block other;
        other.params.push_back(pc(8));
        rig.run(other);
        check(!other.sawChannelEcho, "a different Program Change number does nothing");

        // The same program pressed again, spaced as a foot spaces it, acts again - which is the
        // whole of what a programmable footswitch does and the thing a toggle row depends on. A
        // Program Change has no release, so there is no edge here to look for and never was.
        rig.run(quiet, 8);
        Block again;
        again.params.push_back(pc(7));
        rig.run(again);
        check(again.sawChannelEcho, "the same Program Change pressed again acts again");

        // ... and the same program arriving block after block acts once, not once per block.
        //
        // The LEARNED program, which the first version of this check got wrong: it used an
        // unlearned number, so "no echo" was true whether the guard existed or not and the check
        // asserted nothing at all. Caught by removing the guard and watching this pass.
        rig.run(quiet, 8);
        Block held;
        held.params.push_back(pc(7));
        rig.run(held, 20);
        // echoOf, never sawChannelEcho: the latter is set once before the repeats and stays set,
        // so it answers "did any of the twenty blocks act" where the question here is "did the
        // LAST one". The echo list is cleared per block and is the one that can tell.
        double lastBlockEcho = 0.0;
        check(!echoOf(held, kChannelId, lastBlockEcho),
              "a Program Change repeated block after block stops acting after the first");
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

        // The same blob is the check that a project written before the trims existed opens with
        // them at 0 dB rather than at whatever the plug-in happened to be holding - which is the
        // whole promise of a version marker: an old project sounds as it did.
        MemoryStream after;
        check(component->getState(&after) == kResultOk, "state saves after a version 1 load");
        double trims[kChannelCount] = {};
        check(readTrims(after, trims), "the saved blob carries four trims");
        bool allDefault = true;
        for (double t : trims)
            allDefault = allDefault && near(t, 0.5);
        check(allDefault, "a version 1 project opens with every channel trim at 0 dB");
    }

    // --- 5b. the channel trims survive a save and reload --------------------------------------
    // Written and read as the last four doubles of the blob, which is where version 3 appends
    // them. Distinct values per channel, and both ends of the range among them, so a reader that
    // silently dropped one or transposed two would not pass by accident.
    printf("channel trims\n");
    {
        static const double kWanted[kChannelCount] = {0.0, 0.25, 0.75, 1.0};
        Block set;
        for (int c = 0; c < kChannelCount; ++c)
            set.params.push_back({kChannelLevelId[c], kWanted[c]});
        rig.run(set);

        MemoryStream saved;
        check(component->getState(&saved) == kResultOk, "state saves with the trims set");
        double trims[kChannelCount] = {};
        check(readTrims(saved, trims), "the saved blob carries four trims");
        bool exact = true;
        for (int c = 0; c < kChannelCount; ++c)
            exact = exact && near(trims[c], kWanted[c]);
        check(exact, "each channel's trim is saved as its own value");

        // Move them all somewhere else, then reload and read them back out.
        Block clobber;
        for (int c = 0; c < kChannelCount; ++c)
            clobber.params.push_back({kChannelLevelId[c], 0.5});
        rig.run(clobber);
        saved.seek(0, IBStream::kIBSeekSet, nullptr);
        check(component->setState(&saved) == kResultOk, "state reloads with the trims");
        MemoryStream again;
        check(component->getState(&again) == kResultOk, "state saves again");
        double back[kChannelCount] = {};
        check(readTrims(again, back), "the reloaded blob carries four trims");
        bool restored = true;
        for (int c = 0; c < kChannelCount; ++c)
            restored = restored && near(back[c], kWanted[c]);
        check(restored, "the trims survive save and reload");
    }

    // --- 6. it reaches the audio -------------------------------------------------------------
    //
    // Everything above is about the parameter. This is about the amp: after the banks are built,
    // a pedal press has to move the channel that is actually SOUNDING, which is a different
    // parameter and is held back until the target channel's capture exists.
    printf("audio\n");
    {
        // Reload the banks. The state section above deliberately pushes version 1 and version 3
        // blobs at the plug-in, and neither of those names any captures - a build that wrote them
        // resolved its banks from inside its own bundle and never recorded where they came from.
        // So loading one CLEARS all four channels, which is the correct answer to "this project
        // specifies no captures" and leaves nothing here for a footswitch to switch to.
        if (!RationsTools::loadCaptureRoot(hostContext, component, opt.captures)) {
            fprintf(stderr, "rations_midicheck: could not reload captures from %s\n",
                    opt.captures.c_str());
            return 1;
        }
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


    // --- 7. the pedalboard's footswitches -----------------------------------------------------
    //
    // Five more rows in the same table, and nothing new in the mechanism - which is the claim D8
    // made when it wrote the table generic over ParamID, so this is where it gets checked. What IS
    // new is what a row DOES: a channel row sets, a pedal row toggles, and the difference is the
    // reason a footswitch with four buttons can drive five pedals at all.
    printf("pedal footswitches\n");
    {
        auto setPedals = [&rig](double value) {
            Block set;
            for (int p = 0; p < kPedalCount; ++p)
                set.params.push_back({kPedalOnId[p], value});
            rig.run(set);
        };

        int row = kMidiLearnChannelRows + kPedalBoost;
        sendMessage(hostContext, component, kMsgMidiLearn, &row);
        setPedals(0.0);
        Block teach;
        teach.params.push_back(cc(100, 127));
        rig.run(teach);
        double v = 0.0;
        check(!echoOf(teach, kPedalOnId[kPedalBoost], v),
              "the press that teaches a pedal row does not also stomp the pedal");

        // The claim the row rests on: one button, one pedal, on and off and on again. A row that
        // SET rather than toggled would echo 1.0 three times and pass every check but this one.
        const double kWanted[3] = {1.0, 0.0, 1.0};
        bool alternates = true;
        for (int i = 0; i < 3; ++i) {
            Block press = stomp(rig, 100);
            alternates =
                alternates && echoOf(press, kPedalOnId[kPedalBoost], v) && near(v, kWanted[i]);
        }
        check(alternates, "a pedal row toggles: three presses give on, off, on");

        // ... and it toggles from what the PARAMETER holds, not from a count of its own presses.
        // A host, a preset or an automation lane may have moved that footswitch since the last
        // stomp, and the next stomp has to answer to what the player can see. Boost is on at this
        // point, so a build tracking its own state would turn it off here and a correct one turns
        // it on.
        Block off;
        off.params.push_back({kPedalOnId[kPedalBoost], 0.0});
        rig.run(off);
        Block afterHost = stomp(rig, 100);
        check(echoOf(afterHost, kPedalOnId[kPedalBoost], v) && near(v, 1.0),
              "a stomp toggles from the parameter's value, not from a private one");

        // One row per pedal, each moving its own. The table's order is a static_assert in
        // midilearn.h; this is the end-to-end half of it, through the real parameter queue.
        setPedals(0.0);
        for (int p = 0; p < kPedalCount; ++p) {
            row = kMidiLearnChannelRows + p;
            sendMessage(hostContext, component, kMsgMidiLearn, &row);
            Block learn;
            learn.params.push_back(cc(100 + p, 127));
            rig.run(learn);
        }

        // The board as it stands before any of them is stomped, read out of the plug-in's own
        // state. See the check below for why the echoes are not enough on their own.
        MemoryStream beforeBlob;
        double beforeTrims[kChannelCount] = {};
        std::vector<double> before;
        check(component->getState(&beforeBlob) == kResultOk &&
                  readTrims(beforeBlob, beforeTrims, &before) &&
                  static_cast<int>(before.size()) == kPedalParamCount,
              "the board's whole parameter set can be read before the stomps");

        // "and nothing else" is spelled out as the other four pedals and the channel rather than
        // as an echo COUNT: the output queue also carries the level meter and the sounding-channel
        // report, which are the plug-in describing itself and not the plug-in acting.
        bool eachOwn = true;
        for (int p = 0; p < kPedalCount; ++p) {
            Block press = stomp(rig, 100 + p);
            eachOwn = eachOwn && echoOf(press, kPedalOnId[p], v) && near(v, 1.0) &&
                      !press.sawChannelEcho;
            for (int q = 0; q < kPedalCount; ++q)
                if (q != p)
                    eachOwn = eachOwn && !echoOf(press, kPedalOnId[q], v);
        }
        check(eachOwn, "each pedal row moves its own pedal and nothing else");

        // And what the plug-in STORED, which is not the same question and needed a fault to prove
        // it. The echo carries the ParamID out of the learn table while the store goes to an index
        // the processor computes, so a build that stored one parameter along - a footswitch stomp
        // that quietly moved that pedal's Drive knob instead - echoed the right thing and passed
        // every check above. The state blob is the second route and it is the one that sees it:
        // exactly the five footswitches may have moved, and nothing else on the board.
        MemoryStream afterBlob;
        double afterTrims[kChannelCount] = {};
        std::vector<double> after;
        check(component->getState(&afterBlob) == kResultOk &&
                  readTrims(afterBlob, afterTrims, &after) && after.size() == before.size(),
              "the board's whole parameter set can be read after them");
        bool onlySwitches = after.size() == before.size();
        for (size_t i = 0; i < after.size() && onlySwitches; ++i) {
            bool isSwitch = false;
            for (int p = 0; p < kPedalCount; ++p)
                isSwitch = isSwitch || static_cast<int>(i) == pedalParamIndex(kPedalOnId[p]);
            onlySwitches = isSwitch ? near(after[i], 1.0) : near(after[i], before[i]);
        }
        check(onlySwitches, "five stomps turn on five footswitches and touch nothing else");

        // The two halves of the table do not reach into each other. A pedal stomp must leave the
        // channel alone, and a channel stomp must leave the board alone - which is the failure a
        // table indexed one row out would produce, and it would produce it silently.
        int od1 = kOd1;
        sendMessage(hostContext, component, kMsgMidiLearn, &od1);
        Block learnChannel;
        learnChannel.params.push_back(cc(110, 127));
        rig.run(learnChannel);

        // A PROGRAMMABLE footswitch, which is what a player actually stomps on: each slot is
        // programmed to send one number, so the identical message arrives on every press and
        // nothing arrives on release. There is no rising edge after the first press, and the rule
        // that looked for one made such a pedal work once and then go dead - invisible on a channel
        // row, because selecting Clean twice is selecting Clean, and fatal on a pedal row.
        setPedals(0.0);
        {
            Block release;
            release.params.push_back(cc(100, 0));
            rig.run(release);
            const double kAfter[3] = {1.0, 0.0, 1.0};
            bool everyPress = true;
            for (int i = 0; i < 3; ++i) {
                // Blocks of nothing between the presses, because that is what a stomp looks like:
                // one message, then quiet, then another message some tenths of a second later.
                // Delivering three presses in three CONSECUTIVE blocks would be a host resending
                // rather than a player pressing, and the rule under test is allowed to tell the
                // difference - it is the whole reason the second half of it exists. Caught by
                // being written the wrong way round first.
                Block quiet;
                rig.run(quiet, 8);
                Block press;
                press.params.push_back(cc(100, 127));
                rig.run(press);
                everyPress =
                    everyPress && echoOf(press, kPedalOnId[kPedalBoost], v) && near(v, kAfter[i]);
            }
            check(everyPress, "a slot that sends the same value every press toggles on every press");
        }

        // And the other half of that rule, which is what the rising edge was really protecting. A
        // host writing the same value into a CC parameter block after block - a drawn automation
        // lane, or a resend - must act ONCE, not at three hundred and seventy-five presses a
        // second. Consecutive blocks is exactly what rig.run(b, n) delivers.
        setPedals(0.0);
        {
            Block held;
            held.params.push_back(cc(101, 127));
            rig.run(held, 20);
            check(!echoOf(held, kPedalOnId[kPedalChorus], v),
                  "the same value repeated block after block stops acting after the first");

            // The echo above says the LAST block did nothing; the state says how many of the
            // twenty did anything at all. One toggle leaves Chorus on, twenty leave it off.
            MemoryStream blob;
            double t[kChannelCount] = {};
            std::vector<double> p;
            const int chorusOn = pedalParamIndex(kPedalOnId[kPedalChorus]);
            check(component->getState(&blob) == kResultOk && readTrims(blob, t, &p) &&
                      chorusOn >= 0 && static_cast<int>(p.size()) > chorusOn &&
                      near(p[static_cast<size_t>(chorusOn)], 1.0),
                  "... having acted exactly once");
        }

        Block pedalPress = stomp(rig, 100 + kPedalDelay);
        check(!pedalPress.sawChannelEcho, "a pedal stomp does not move the channel");
        Block channelPress = stomp(rig, 110);
        bool touchedNoPedal = true;
        for (int p = 0; p < kPedalCount; ++p)
            touchedNoPedal = touchedNoPedal && !echoOf(channelPress, kPedalOnId[p], v);
        check(channelPress.sawChannelEcho && near(channelPress.channelEcho, channelValue(kOd1)) &&
                  touchedNoPedal,
              "a channel stomp moves the channel and no pedal");
    }

    // --- 7b. nine rows in the state blob, and four in an old one ------------------------------
    //
    // The MIDI table sits in the MIDDLE of the blob, so its length is not a detail: a reader that
    // takes the wrong number of words reads everything after it - the trims, the output section,
    // the capture paths, the pedalboard - at the wrong offset. That is why state version 6 gave
    // the block a count, and it is the only thing version 6 does.
    printf("state, nine rows\n");
    {
        MemoryStream saved;
        check(component->getState(&saved) == kResultOk, "state saves with pedal rows learned");
        double trims[kChannelCount] = {};
        std::vector<double> pedals;
        check(readTrims(saved, trims, &pedals) &&
                  static_cast<int>(pedals.size()) == kPedalParamCount,
              "the saved blob still reads through to the pedalboard");

        auto reloadAndCheck = [&](MemoryStream &blob, const char *what) {
            blob.seek(0, IBStream::kIBSeekSet, nullptr);
            if (!check(component->setState(&blob) == kResultOk, what))
                return;
            MemoryStream again;
            double t[kChannelCount] = {};
            std::vector<double> p;
            check(component->getState(&again) == kResultOk && readTrims(again, t, &p),
                  "... and saves again");
            bool same = p.size() == pedals.size();
            for (int c = 0; c < kChannelCount && same; ++c)
                same = near(t[c], trims[c]);
            for (size_t i = 0; i < p.size() && same; ++i)
                same = near(p[i], pedals[i]);
            check(same, "... with every trim and every pedal value where it was");
        };

        // The editor's own reader, against the processor's. Both sides walk this blob, and the
        // one thing the length prefix can break is the OFFSET everything after it is read at - so
        // what is compared is the fields on the far side of the MIDI block, through the only
        // window a host has on the controller: the parameters themselves.
        {
            saved.seek(0, IBStream::kIBSeekSet, nullptr);
            check(controller->setComponentState(&saved) == kResultOk,
                  "the editor accepts the same blob");
            bool mirrors = true;
            for (int c = 0; c < kChannelCount; ++c)
                mirrors = mirrors && near(controller->getParamNormalized(kChannelLevelId[c]),
                                          trims[c]);
            for (int i = 0; i < kPedalParamCount; ++i)
                mirrors = mirrors && near(controller->getParamNormalized(kPedalParams[i].id),
                                          pedals[static_cast<size_t>(i)]);
            check(mirrors, "and reads every trim and every pedal value at the same offset");
        }

        // Round trip at the current version: the pedal rows come back and still work.
        for (int r = 0; r < kMidiLearnRowCount; ++r)
            sendMessage(hostContext, component, kMsgMidiClear, &r);
        reloadAndCheck(saved, "a version 6 blob reloads");
        {
            Block press = stomp(rig, 100 + kPedalFlanger);
            double v = 0.0;
            check(echoOf(press, kPedalOnId[kPedalFlanger], v),
                  "a pedal row survives save and reload");
        }

        // A project written before the footswitch rows existed: version 5, four rows, no count.
        // Its four channel bindings must survive intact and its five pedal rows must come back
        // unlearned - and, the part that costs a misread, everything AFTER the table must land
        // where it belongs.
        std::vector<char> older;
        check(rewriteMidiBlock(saved, 5, kMidiLearnRowsV2, older),
              "a version 5 blob can be built from this one");
        {
            MemoryStream blob(older.data(), static_cast<TSize>(older.size()));
            reloadAndCheck(blob, "a version 5 blob loads");
        }
        {
            Block press = stomp(rig, 100 + kPedalFlanger);
            double v = 0.0;
            check(!echoOf(press, kPedalOnId[kPedalFlanger], v),
                  "a version 5 project opens with its pedal rows unlearned");
            Block channel = stomp(rig, 110);
            check(channel.sawChannelEcho && near(channel.channelEcho, channelValue(kOd1)),
                  "... and with its channel rows exactly as they were");
        }

        // And a project written by a LATER build with more rows than this one has. Its extra rows
        // are read and dropped, not seeked past, so the trims after them still line up.
        std::vector<char> newer;
        check(rewriteMidiBlock(saved, 6, kMidiLearnRowCount + 3, newer),
              "a blob with more rows than this build can be built");
        {
            MemoryStream blob(newer.data(), static_cast<TSize>(newer.size()));
            reloadAndCheck(blob, "a blob with more rows than this build loads");
        }
        {
            Block press = stomp(rig, 100 + kPedalFlanger);
            double v = 0.0;
            check(echoOf(press, kPedalOnId[kPedalFlanger], v),
                  "... and the rows this build does have still work");
        }
    }

    processor->setProcessing(false);
    component->setActive(false);

    printf("\n%s\n", gFailures == 0 ? "rations_midicheck: PASSED" : "rations_midicheck: FAILED");
    return gFailures == 0 ? 0 : 1;
}
