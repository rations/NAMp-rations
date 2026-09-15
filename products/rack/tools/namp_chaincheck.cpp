// namp_chaincheck — the phase gate for the effects chain, with no audio device and no UI.
//
// Two modes, and they answer different questions:
//
//   --routing        builds chains out of SYNTHETIC backends whose output is exact arithmetic on
//                    their input, then checks the samples that come out. No plug-in is installed,
//                    nothing is dlopened, and the answers are numbers rather than "it sounded
//                    fine". This is what proves the ping-pong slot assignment does not alias, that
//                    the pre-section really is mono and the post-section really is stereo, that
//                    JACK's input buffer is never written, that a bypassed node costs nothing and
//                    changes nothing, that the wet/dry blend is amplitude-correct, and that a
//                    plug-in emitting NaN cannot reach a speaker.
//
//   --rt <key> [n]   runs n instances of a REAL VST3 plug-in either side of a stub anchor and
//                    counts allocations on the audio path with the DSP core's own malloc
//                    interception. The chain machinery must contribute none; a plug-in that
//                    allocates is reported separately, because we can name that but not prevent it.
//
// The anchor in both modes is a stub standing in for the amp: mono in, the same signal on both
// output
// channels, which is what src/namprocessor.cpp does after its chunk loop.

#include "host/chainbuilder.h"
#include "host/chainengine.h"
#include "host/hostapp.h"
#include "host/pluginpaths.h"
#include "host/scancache.h" // escapeField, for the hand-written path files below
#include "host/rackpreset.h"

#include "pluginterfaces/vst/ivstmessage.h"

// THE ALLOCATION HARNESS AND THREADSANITIZER CANNOT SHARE A BINARY, and the failure is not one
// anyone would diagnose from the symptom: both interpose the process allocator, and the pair
// segfaults during startup with nothing printed at all, before main() runs. Measured — not
// inferred from the idea that they might conflict.
//
// So under -DNAMPRACK_ENABLE_TSAN the build leaves allocation_tracking.cpp out and defines the
// globals its header declares right here, which lets everything else in this file compile and run
// unchanged. What is lost is the allocation COUNT; what is being measured in that build is the
// race, and the two questions were always separate tools in this tree.
//
// THE WINDOWS BUILD TAKES THE SAME BRANCH, for a different reason with the same consequence: the
// harness interposes malloc through dlsym(RTLD_NEXT) and its header includes <dlfcn.h>, neither of
// which exists on MinGW. The allocation gate is a development-machine gate and always was — the
// same place RULES puts ThreadSanitizer and live JACK. What the Windows build of this tool is for
// is the routing check and the PRESET ROUND-TRIP, which is a phase-7 gate in its own right: a rack
// saved on one platform has to come back on the other with every hosted parameter intact.
#ifndef NAMPRACK_ALLOC_HARNESS
#if defined(_WIN32)
#define NAMPRACK_ALLOC_HARNESS 0
#else
#define NAMPRACK_ALLOC_HARNESS 1
#endif
#endif

#if NAMPRACK_ALLOC_HARNESS
#include "test/allocation_tracking.h"
#else
namespace allocation_tracking
{
volatile int g_allocation_count = 0;
volatile int g_deallocation_count = 0;
volatile bool g_tracking_enabled = false;
void *(*original_malloc)(size_t) = nullptr;
void (*original_free)(void *) = nullptr;
void *(*original_realloc)(void *, size_t) = nullptr;
} // namespace allocation_tracking
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace NAMp::host;

namespace
{

constexpr int32_t kBlock = 256;
constexpr double kSampleRate = 48000.0;

// A well-formed VST3 key that resolves to nothing: adopt() never loads it, but PluginRef::valid()
// and anything that later parses a saved chain both have to be happy with it. Split across two
// string literals on purpose — "\x1f0" would otherwise be read as one hex escape.
constexpr const char *kSyntheticKey = "/synthetic\x1f"
                                      "00000000000000000000000000000000";

int gFailures = 0;

void check(bool condition, const char *what)
{
    if (condition) {
        std::printf("  ok    %s\n", what);
        return;
    }
    std::printf("  FAIL  %s\n", what);
    ++gFailures;
}

void checkClose(double got, double want, double tolerance, const char *what)
{
    if (std::fabs(got - want) <= tolerance) {
        std::printf("  ok    %-52s %.6f\n", what, got);
        return;
    }
    std::printf("  FAIL  %-52s %.6f (wanted %.6f)\n", what, got, want);
    ++gFailures;
}

//------------------------------------------------------------------------
// A backend whose arithmetic is known exactly, so the chain's routing can be checked by reading
// samples rather than by listening. It deliberately does NOT write its output in place: it reads
// every input first and then writes, so an aliasing bug in the slot assignment shows up as a wrong
// number instead of accidentally working.
class ArithmeticBackend final : public PluginBackend
{
public:
    enum class Mode {
        AddOffset,  // out = in + offset
        EmitNaN,    // out = NaN, whatever the input was
        EmitLoud,   // out = 100.0
        Silent,     // writes nothing at all: a plug-in that leaves its output bus alone
        Delay,      // out = in delayed by `latency` samples: a plug-in that is honestly late
        RecordMidi, // passes audio through and keeps every message it was handed, with its offset
    };

    // `reportLatency == false` makes the node delay its output by `latency` samples while telling
    // the host it has none. That is not a plug-in bug being modelled for its own sake — it is the
    // CONTROL for the dry-path compensation: with it, the same chain that lines up when the latency
    // is declared must visibly fail to line up when it is not, which is what makes the passing case
    // evidence rather than a coincidence.
    ArithmeticBackend(Mode mode, float offset, uint32_t latency, bool reportLatency = true)
        : mMode(mode), mOffset(offset), mLatency(latency), mReportLatency(reportLatency)
    {
    }

    PluginFormat format() const override
    {
        return PluginFormat::Vst3;
    }
    const char *key() const override
    {
        return "synthetic";
    }
    const char *displayName() const override
    {
        return "Arithmetic";
    }
    const char *category() const override
    {
        return "Test";
    }

    bool prepare(const ProcessConfig &config) override
    {
        mChannels = config.channels;
        // Allocated here and nowhere else, exactly as a real backend must.
        mRing.assign(static_cast<size_t>(mLatency) * static_cast<size_t>(mChannels), 0.0f);
        mRingPos = 0;
        return true;
    }
    void activate() override
    {
        mActive = true;
    }
    void deactivate() override
    {
        mActive = false;
    }
    void reset() override
    {
    }

    uint32_t latencySamples() const override
    {
        return mReportLatency ? mLatency : 0;
    }
    bool prefersInPlace() const override
    {
        return false;
    }
    int32_t audioInCount() const override
    {
        return mChannels;
    }
    int32_t audioOutCount() const override
    {
        return mChannels;
    }

    void process(const AudioBlock &block) noexcept override
    {
        ++mCalls;
        if (mMode == Mode::Silent)
            return;

        if (mMode == Mode::RecordMidi) {
            for (int32_t i = 0; i < block.midiCount; ++i)
                mMidiSeen.push_back(block.midi[i]);
            mChunkCounts.push_back(block.midiCount);
            for (int32_t c = 0; c < block.channels; ++c)
                std::memcpy(block.out[c], block.in[c],
                            sizeof(float) * static_cast<size_t>(block.frames));
            return;
        }

        if (mMode == Mode::Delay) {
            const int32_t len = static_cast<int32_t>(mLatency);
            if (len <= 0) {
                for (int32_t c = 0; c < block.channels; ++c)
                    std::memcpy(block.out[c], block.in[c],
                                sizeof(float) * static_cast<size_t>(block.frames));
                return;
            }
            int32_t endPos = mRingPos;
            for (int32_t c = 0; c < block.channels; ++c) {
                float *ring = mRing.data() + static_cast<size_t>(c) * static_cast<size_t>(len);
                const float *in = block.in[c];
                float *out = block.out[c];
                int32_t pos = mRingPos;
                for (int32_t s = 0; s < block.frames; ++s) {
                    out[s] = ring[pos];
                    ring[pos] = in[s];
                    if (++pos == len)
                        pos = 0;
                }
                endPos = pos;
            }
            mRingPos = endPos;
            return;
        }

        for (int32_t c = 0; c < block.channels; ++c) {
            const float *in = block.in[c];
            float *out = block.out[c];
            for (int32_t s = 0; s < block.frames; ++s) {
                switch (mMode) {
                    case Mode::AddOffset:
                        out[s] = in[s] + mOffset;
                        break;
                    case Mode::EmitNaN:
                        out[s] = std::nanf("");
                        break;
                    case Mode::RecordMidi: // handled whole-block above; never reached per sample
                        break;
                    case Mode::EmitLoud:
                        out[s] = 100.0f;
                        break;
                    case Mode::Silent:
                    case Mode::Delay:
                        break;
                }
            }
        }
    }

    uint32_t paramCount() const override
    {
        return 0;
    }
    bool paramInfo(uint32_t, ParamInfo &) const override
    {
        return false;
    }
    double paramGet(uint32_t) const override
    {
        return 0.0;
    }
    void paramSetFromUi(uint32_t, double) override
    {
    }
    bool paramDisplay(uint32_t, double, char *, int32_t) const override
    {
        return false;
    }
    bool paramPollFromRt(uint32_t &, double &) override
    {
        return false;
    }
    // Nothing is queued here: paramSetFromUi writes the value straight into the array above,
    // because a stub with no audio thread has nothing to marshal across.
    void paramFlushToPlugin() override
    {
    }

    bool stateSave(std::vector<uint8_t> &) const override
    {
        return false;
    }
    bool stateLoad(const uint8_t *, size_t) override
    {
        return false;
    }

    EditorKind editorKind() const override
    {
        return EditorKind::NoEditor;
    }
    bool editorOpen(const EditorOpenRequest &, EditorSurface &) override
    {
        return false;
    }
    void editorIdle() override
    {
    }
    bool editorTakeResizeRequest(int32_t &, int32_t &) override
    {
        return false;
    }
    bool editorCheckSize(int32_t &, int32_t &) const override
    {
        return false;
    }
    void editorSetSize(int32_t, int32_t) override
    {
    }
    void editorShow() override
    {
    }
    void editorHide() override
    {
    }
    void editorClose() override
    {
    }

    bool hasFileLoader() const override
    {
        return false;
    }
    bool fileGet(int32_t, char *, int32_t) const override
    {
        return false;
    }
    bool fileSet(int32_t, const char *) override
    {
        return false;
    }

    int64_t diagTakeMaxMicros() override
    {
        return 0;
    }
    uint64_t diagRtAllocCount() const override
    {
        return 0;
    }

    int calls() const
    {
        return mCalls;
    }
    bool active() const
    {
        return mActive;
    }
    int32_t channels() const
    {
        return mChannels;
    }

    // RecordMidi's evidence, public because reading it IS the check. Vectors on purpose: this is a
    // check tool's own bookkeeping, not the audio path being measured, and push_back here would
    // fail the RT gate if RecordMidi were ever used in --rt. It is not — the allocation harness
    // runs against AddOffset chains.
    std::vector<RtMidiEvent> mMidiSeen;
    std::vector<int32_t> mChunkCounts;

private:
    Mode mMode;
    float mOffset = 0.0f;
    uint32_t mLatency = 0;
    bool mReportLatency = true;
    int32_t mChannels = 1;
    int mCalls = 0;
    bool mActive = false;
    std::vector<float> mRing;
    int32_t mRingPos = 0;
};

//------------------------------------------------------------------------
// Drives one JACK-sized block through the engine, standing in for JackClient::process(). The anchor
// is the amp's shape and nothing more: mono in, the same samples on both outputs.
void runBlock(ChainEngine &engine, const float *in, float *outL, float *outR, int32_t frames)
{
    engine.beginBlock();

    ChainIo io;
    engine.beginChunk(in, outL, outR, frames, io);
    for (int32_t s = 0; s < frames; ++s) {
        const float v = io.anchorIn[s];
        io.anchorOut[0][s] = v;
        io.anchorOut[1][s] = v;
    }
    engine.endChunk();
}

//------------------------------------------------------------------------
// The same block, driven as `chunks` equal chunks, with the cycle's MIDI handed to beginBlock the
// way the JACK client hands it over. Separate from runBlock because the thing being checked is what
// each CHUNK is given, which a single-chunk block cannot show.
void runBlockChunked(ChainEngine &engine, const float *in, float *outL, float *outR, int32_t frames,
                     int32_t chunks, const RtMidiEvent *midi, int32_t midiCount)
{
    engine.beginBlock(midi, midiCount);

    const int32_t n = frames / chunks;
    for (int32_t c = 0; c < chunks; ++c) {
        const int32_t off = c * n;
        ChainIo io;
        engine.beginChunk(in + off, outL + off, outR + off, n, io);
        for (int32_t s = 0; s < n; ++s) {
            const float v = io.anchorIn[s];
            io.anchorOut[0][s] = v;
            io.anchorOut[1][s] = v;
        }
        engine.endChunk();
    }
}

//------------------------------------------------------------------------
// One fixture: an engine, a builder wired to it, and the three JACK-side buffers.
struct Fixture {
    ChainEngine engine;
    ChainBuilder builder;
    std::vector<float> in;
    std::vector<float> outL;
    std::vector<float> outR;

    Fixture() : in(kBlock, 0.0f), outL(kBlock, 0.0f), outR(kBlock, 0.0f)
    {
        engine.prepare(kBlock);
        builder.setEngine(&engine);
        builder.configure(kSampleRate, kBlock);
    }

    int addArithmetic(ChainSection section, ArithmeticBackend::Mode mode, float offset,
                      uint32_t latency = 0, bool reportLatency = true)
    {
        std::string error;
        auto backend = std::make_unique<ArithmeticBackend>(mode, offset, latency, reportLatency);
        PluginRef ref;
        ref.format = PluginFormat::Vst3;
        ref.key = kSyntheticKey;
        const int index = builder.adopt(section, std::move(backend), ref, error);
        if (index < 0)
            std::printf("  FAIL  adopt: %s\n", error.c_str());
        return index;
    }

    void fill(float value)
    {
        for (auto &v : in)
            v = value;
    }

    void run()
    {
        runBlock(engine, in.data(), outL.data(), outR.data(), kBlock);
    }

    ~Fixture()
    {
        // Order matters and mirrors the standalone's shutdown: the audio thread stops first, then
        // whatever it was holding becomes the builder's to free.
        engine.abandon();
        builder.collectAll();
    }
};

//------------------------------------------------------------------------
// Whether this binary can count allocations at all. False in the ThreadSanitizer build, where the
// interposition was deliberately left out — see the note at the top of this file. Reported once
// rather than silently producing zeros, because "no allocations" and "no counter" print the same.
bool allocationHarnessBuilt()
{
    return NAMPRACK_ALLOC_HARNESS != 0;
}

//------------------------------------------------------------------------
// A counter that cannot fire is not evidence of anything, and operator-new interception is exactly
// the kind of thing a link order or an inlining decision can silently defeat.
bool harnessHasTeeth()
{
    allocation_tracking::g_allocation_count = 0;
    allocation_tracking::g_deallocation_count = 0;
    allocation_tracking::g_tracking_enabled = true;
    volatile auto *bait = new double[64];
    delete[] bait;
    allocation_tracking::g_tracking_enabled = false;

    if (allocation_tracking::g_allocation_count > 0)
        return true;
    std::fprintf(stderr, "chaincheck: the allocation harness saw nothing, so every count below "
                         "would be meaningless\n");
    return false;
}

//------------------------------------------------------------------------
// Runs `blocks` blocks with the allocation counter armed. The fixture must already be warm: a
// lazily-sized buffer growing on its first call is a warm-up, not a real-time violation.
void countBlocks(Fixture &fixture, int blocks, int &allocations, int &frees)
{
    allocation_tracking::g_allocation_count = 0;
    allocation_tracking::g_deallocation_count = 0;
    allocation_tracking::g_tracking_enabled = true;
    for (int i = 0; i < blocks; ++i)
        fixture.run();
    allocation_tracking::g_tracking_enabled = false;
    allocations = allocation_tracking::g_allocation_count;
    frees = allocation_tracking::g_deallocation_count;
}

//------------------------------------------------------------------------
// Sixteen synthetic nodes, one of them off full wet, warmed and then counted. This is the control
// for the real-plug-in run: identical chain code, no third-party code anywhere in the call graph.
void runSyntheticControl(int &allocations, int &frees)
{
    Fixture f;
    for (int i = 0; i < 8; ++i)
        f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.001f);
    for (int i = 0; i < 8; ++i)
        f.addArithmetic(ChainSection::Post, ArithmeticBackend::Mode::AddOffset, 0.001f);
    f.builder.setMix(ChainSection::Post, 3, 0.5f);
    f.builder.publish();
    f.fill(0.01f);
    for (int i = 0; i < 32; ++i)
        f.run();
    countBlocks(f, 500, allocations, frees);
}

//------------------------------------------------------------------------
int doRouting()
{
    std::printf("empty rack (the standalone's cost today must not change)\n");
    {
        Fixture f;
        f.fill(0.25f);
        f.run();
        check(f.outL[0] == 0.25f && f.outR[0] == 0.25f, "anchor sees JACK's own buffers");
        check(f.engine.latencySamples() == 0, "no chain, no latency");
    }

    std::printf("\nserial routing, two mono pedals in front and two stereo pedals after\n");
    {
        Fixture f;
        f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.01f);
        f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.02f);
        f.addArithmetic(ChainSection::Post, ArithmeticBackend::Mode::AddOffset, 0.10f);
        f.addArithmetic(ChainSection::Post, ArithmeticBackend::Mode::AddOffset, 0.20f);
        f.builder.publish();

        f.fill(0.0f);
        f.run();
        // Every node must have contributed exactly once, in order. A slot that aliased would drop
        // or double one of these terms.
        checkClose(f.outL[0], 0.33, 1e-6, "left  = 0.01+0.02+0.10+0.20");
        checkClose(f.outR[0], 0.33, 1e-6, "right = 0.01+0.02+0.10+0.20");
        checkClose(f.outL[kBlock - 1], 0.33, 1e-6, "last sample of the block too");

        check(f.builder.backend(ChainSection::Pre, 0)->audioInCount() == 1,
              "pre-section nodes are configured mono");
        check(f.builder.backend(ChainSection::Post, 0)->audioInCount() == 2,
              "post-section nodes are configured stereo");
    }

    std::printf("\nMIDI reaches every node, sliced to the chunk it belongs in\n");
    {
        // Four chunks of kBlock/4. Four messages, one landing in each chunk, plus one past the end
        // of the block that must never be delivered at all.
        Fixture f;
        f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::RecordMidi, 0.0f);
        f.addArithmetic(ChainSection::Post, ArithmeticBackend::Mode::RecordMidi, 0.0f);
        f.builder.publish();

        const int32_t chunks = 4;
        const int32_t n = kBlock / chunks;
        const RtMidiEvent midi[5] = {
            {0, 0xb0, 80, 127},            // chunk 0, first sample
            {n + 5, 0xb0, 81, 127},        // chunk 1
            {2 * n + 17, 0x90, 60, 100},   // chunk 2
            {3 * n + (n - 1), 0xc0, 3, 0}, // chunk 3, last sample
            {kBlock + 40, 0xb0, 82, 127},  // past the end of the cycle: never delivered
        };
        f.fill(0.0f);
        runBlockChunked(f.engine, f.in.data(), f.outL.data(), f.outR.data(), kBlock, chunks, midi,
                        5);

        // Both sections, because they are run by different calls — the pre section inside
        // beginChunk and the post section inside endChunk — and a slice that is right for one is
        // not thereby right for the other.
        for (const ChainSection section : {ChainSection::Pre, ChainSection::Post}) {
            const auto *node =
                static_cast<const ArithmeticBackend *>(f.builder.backend(section, 0));
            const char *where = (section == ChainSection::Pre) ? "pre " : "post";
            char label[128];

            std::snprintf(label, sizeof(label), "%s: four of the five messages arrived", where);
            check(node->mMidiSeen.size() == 4, label);
            if (node->mMidiSeen.size() != 4)
                continue;

            std::snprintf(label, sizeof(label), "%s: the one past the block's end did not", where);
            bool leaked = false;
            for (const RtMidiEvent &e : node->mMidiSeen)
                leaked = leaked || e.data1 == 82;
            check(!leaked, label);

            std::snprintf(label, sizeof(label), "%s: one message per chunk, in order", where);
            check(node->mChunkCounts.size() == static_cast<size_t>(chunks) &&
                      node->mChunkCounts[0] == 1 && node->mChunkCounts[1] == 1 &&
                      node->mChunkCounts[2] == 1 && node->mChunkCounts[3] == 1,
                  label);

            // The whole point of the rebase: a node is given the chunk it is running, so an offset
            // measured from the block would be past the end of three chunks out of four.
            std::snprintf(label, sizeof(label), "%s: offsets rebased to the chunk", where);
            check(node->mMidiSeen[0].frame == 0 && node->mMidiSeen[1].frame == 5 &&
                      node->mMidiSeen[2].frame == 17 && node->mMidiSeen[3].frame == n - 1,
                  label);

            std::snprintf(label, sizeof(label), "%s: status and data bytes intact", where);
            check(node->mMidiSeen[0].status == 0xb0 && node->mMidiSeen[0].data1 == 80 &&
                      node->mMidiSeen[0].data2 == 127 && node->mMidiSeen[2].status == 0x90 &&
                      node->mMidiSeen[3].status == 0xc0,
                  label);
        }
    }

    std::printf("\na cycle with no MIDI hands nodes nothing at all\n");
    {
        Fixture f;
        f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::RecordMidi, 0.0f);
        f.builder.publish();
        f.fill(0.0f);
        f.run();
        const auto *node =
            static_cast<const ArithmeticBackend *>(f.builder.backend(ChainSection::Pre, 0));
        check(node->mMidiSeen.empty(), "nothing delivered");
        check(node->mChunkCounts.size() == 1 && node->mChunkCounts[0] == 0,
              "and the count really was zero, not merely unread");
    }

    std::printf("\nJACK's input buffer is read-only\n");
    {
        Fixture f;
        f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.5f);
        f.builder.publish();
        f.fill(0.125f);
        f.run();
        bool untouched = true;
        for (const float v : f.in)
            untouched = untouched && (v == 0.125f);
        check(untouched, "no node wrote through kSlotIn");
        checkClose(f.outL[0], 0.625, 1e-6, "and the pedal still ran");
    }

    std::printf("\nlong chain: eight pedals each side, alternating ping-pong slots\n");
    {
        Fixture f;
        for (int i = 0; i < 8; ++i)
            f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.01f);
        for (int i = 0; i < 8; ++i)
            f.addArithmetic(ChainSection::Post, ArithmeticBackend::Mode::AddOffset, 0.02f);
        f.builder.publish();
        f.fill(0.0f);
        f.run();
        checkClose(f.outL[0], 0.24, 1e-5, "8 x 0.01 mono then 8 x 0.02 stereo");
        checkClose(f.outR[0], 0.24, 1e-5, "both channels agree");
    }

    std::printf("\nbypass shortens the chain rather than branching in it\n");
    {
        Fixture f;
        f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.01f);
        const int muted =
            f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.02f);
        f.builder.setEnabled(ChainSection::Pre, muted, false);
        f.builder.publish();
        f.fill(0.0f);
        f.run();
        checkClose(f.outL[0], 0.01, 1e-6, "the disabled node contributed nothing");
        check(static_cast<ArithmeticBackend *>(f.builder.backend(ChainSection::Pre, muted))
                      ->calls() == 0,
              "and was not called at all");
    }

    std::printf("\nwet/dry mix is amplitude-complementary\n");
    {
        Fixture f;
        const int index =
            f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 1.0f);
        f.builder.setMix(ChainSection::Pre, index, 0.25f);
        f.builder.publish();
        f.fill(0.0f);
        f.run();
        // 0.25 * (0 + 1) + 0.75 * 0
        checkClose(f.outL[0], 0.25, 1e-6, "out = wet*mix + dry*(1-mix)");
    }

    std::printf("\nthe dry path is delayed by the node's own latency, so a blend does not comb\n");
    {
        // A ramp, so every sample is distinguishable from every other one and a misalignment by
        // even a single sample shows up as a wrong number rather than as a plausible one.
        constexpr uint32_t kNodeLatency = 64;
        auto ramp = [](int32_t s) { return 0.001f * static_cast<float>(s + 1); };

        // Fully wet: the reference the blend has to reproduce.
        std::vector<float> wet(kBlock, 0.0f);
        {
            Fixture f;
            f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::Delay, 0.0f, kNodeLatency);
            f.builder.publish();
            for (int32_t s = 0; s < kBlock; ++s)
                f.in[static_cast<size_t>(s)] = ramp(s);
            f.run();
            wet = f.outL;
        }
        check(wet[kNodeLatency] == ramp(0), "the reference node really is 64 samples late");

        // Half wet, latency declared. Both paths are late by the same 64 samples, so the sum is the
        // wet signal exactly — which is what "a mix control, not an effect" means.
        {
            Fixture f;
            const int index = f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::Delay,
                                              0.0f, kNodeLatency);
            f.builder.setMix(ChainSection::Pre, index, 0.5f);
            f.builder.publish();
            for (int32_t s = 0; s < kBlock; ++s)
                f.in[static_cast<size_t>(s)] = ramp(s);
            f.run();

            bool identical = true;
            for (int32_t s = 0; s < kBlock; ++s)
                identical =
                    identical && f.outL[static_cast<size_t>(s)] == wet[static_cast<size_t>(s)];
            check(identical, "50% wet against a declared-latency node equals the fully wet signal");
        }

        // The control: the same delay, undeclared. Nothing can compensate for a latency a plug-in
        // does not report, so this MUST differ — and by exactly the undelayed dry half.
        {
            Fixture f;
            const int index = f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::Delay,
                                              0.0f, kNodeLatency, /* reportLatency */ false);
            f.builder.setMix(ChainSection::Pre, index, 0.5f);
            f.builder.publish();
            for (int32_t s = 0; s < kBlock; ++s)
                f.in[static_cast<size_t>(s)] = ramp(s);
            f.run();

            bool differs = false;
            for (int32_t s = 0; s < kBlock; ++s)
                differs = differs || f.outL[static_cast<size_t>(s)] != wet[static_cast<size_t>(s)];
            check(differs, "...and an UNdeclared one does not, which is what makes that a result");
            checkClose(f.outL[kBlock - 1],
                       0.5 * ramp(kBlock - 1 - kNodeLatency) + 0.5 * ramp(kBlock - 1), 1e-6,
                       "the uncompensated blend is half late and half early");
        }
    }

    std::printf("\nthe safety pass, which is the only thing between a bad plug-in and a speaker\n");
    {
        Fixture f;
        f.addArithmetic(ChainSection::Post, ArithmeticBackend::Mode::EmitNaN, 0.0f);
        f.builder.publish();
        f.fill(0.1f);
        f.run();
        check(std::isfinite(f.outL[0]) && f.outL[0] == 0.0f, "NaN becomes silence");
        check(std::isfinite(f.outR[0]) && f.outR[0] == 0.0f, "on both channels");
    }
    {
        Fixture f;
        f.addArithmetic(ChainSection::Post, ArithmeticBackend::Mode::EmitLoud, 0.0f);
        f.builder.publish();
        f.fill(0.1f);
        f.run();
        checkClose(f.outL[0], 4.0, 1e-6, "a runaway level is clamped to +/- 4.0");
    }

    std::printf("\na plug-in that declines to write its output bus\n");
    {
        Fixture f;
        // Pre-fill JACK's outputs with something recognisable: JACK does not promise them clean, so
        // this is exactly the state a real callback can start in.
        Fixture &fx = f;
        for (int32_t i = 0; i < kBlock; ++i) {
            fx.outL[static_cast<size_t>(i)] = 9.0f;
            fx.outR[static_cast<size_t>(i)] = 9.0f;
        }
        f.addArithmetic(ChainSection::Post, ArithmeticBackend::Mode::Silent, 0.0f);
        f.builder.publish();
        f.fill(0.1f);
        f.run();
        check(f.outL[0] == 0.0f && f.outR[0] == 0.0f, "stale output is cleared, not passed on");
    }

    std::printf("\nlatency is summed and reported, not silently zero\n");
    {
        Fixture f;
        f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.0f, 64);
        f.addArithmetic(ChainSection::Post, ArithmeticBackend::Mode::AddOffset, 0.0f, 192);
        f.builder.publish();
        check(f.builder.latencySamples() == 256, "builder sums 64 + 192");
        f.fill(0.0f);
        f.run();
        check(f.engine.latencySamples() == 256, "and the engine publishes it once adopted");
    }

    std::printf(
        "\nedits while audio is running: retirement, not destruction on the audio thread\n");
    {
        Fixture f;
        f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.01f);
        f.builder.publish();
        f.fill(0.0f);
        f.run();

        for (int round = 0; round < 32; ++round) {
            f.builder.remove(ChainSection::Pre, 0);
            f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.02f);
            f.builder.publish();
            f.run(); // the audio thread adopts and hands the old snapshot back
            f.builder.collect();
        }
        checkClose(f.outL[0], 0.02, 1e-6, "32 hot swaps, still the right chain");
    }

    std::printf("\nthe chain is full at %d nodes and says so\n", kMaxChainNodes);
    {
        Fixture f;
        for (int i = 0; i < kMaxChainNodes; ++i)
            f.addArithmetic(ChainSection::Pre, ArithmeticBackend::Mode::AddOffset, 0.0f);
        std::string error;
        auto extra =
            std::make_unique<ArithmeticBackend>(ArithmeticBackend::Mode::AddOffset, 0.0f, 0);
        PluginRef ref;
        ref.format = PluginFormat::Vst3;
        ref.key = kSyntheticKey;
        check(f.builder.adopt(ChainSection::Pre, std::move(extra), ref, error) < 0,
              "node 33 is refused rather than overrunning the snapshot");
    }

    std::printf("\nthe .namprack format, including what a hostile or hand-edited one does\n");
    {
        // base64 first, because everything below rests on it. A blob with every byte value in it,
        // at all four lengths mod 3, is what catches a padding bug — which decodes to something
        // plausible rather than failing, and is therefore the kind that ships.
        bool roundTripped = true;
        for (size_t len = 253; len <= 256; ++len) {
            std::vector<uint8_t> blob(len);
            for (size_t i = 0; i < len; ++i)
                blob[i] = static_cast<uint8_t>(i * 7 + 3);
            std::vector<uint8_t> back;
            roundTripped = roundTripped &&
                           base64Decode(base64Encode(blob.data(), blob.size()), back, 4096) &&
                           back == blob;
        }
        check(roundTripped, "base64 round-trips every byte value at every padding length");

        std::vector<uint8_t> junk;
        check(!base64Decode("AAAA=AAA", junk, 4096), "...rejects padding in the middle");
        check(!base64Decode("AAA", junk, 4096), "...rejects a length that is not a multiple of 4");
        check(!base64Decode("AA!A", junk, 4096), "...rejects a character outside the alphabet");
        check(!base64Decode("AAAAAAAA", junk, 2), "...refuses to decode past the caller's cap");

        // A whole preset, written and read back.
        RackPreset preset;
        {
            PresetNode node;
            node.section = ChainSection::Post;
            node.formatTag = "LV2";
            node.formatKnown = true;
            node.ref.format = PluginFormat::Lv2;
            node.ref.key = "http://example.org/plugin#one";
            node.name = "A pedal with a \\ backslash and a  double space";
            node.enabled = false;
            node.mix = 0.375f;
            node.params = {0.0, 0.5, 1.0};
            node.state = {1, 2, 3, 250, 251, 252};
            preset.nodes.push_back(node);
        }
        const std::string text = writeRackPreset(preset);

        RackPreset parsed;
        std::string warnings;
        check(parseRackPreset(text.data(), text.size(), parsed, warnings) &&
                  parsed.nodes.size() == 1,
              "a preset written by this build parses back into one node");
        if (parsed.nodes.size() == 1) {
            const PresetNode &n = parsed.nodes[0];
            check(n.section == ChainSection::Post && n.ref.key == preset.nodes[0].ref.key &&
                      n.name == preset.nodes[0].name && !n.enabled && n.mix == 0.375f &&
                      n.params == preset.nodes[0].params && n.state == preset.nodes[0].state,
                  "...with every field, including a name that needed escaping");
        }

        // Untrusted input: one case per way a preset off disk can be malformed.
        RackPreset bad;
        check(!parseRackPreset("hello\n", 6, bad, warnings), "a file with no header is refused");
        check(!parseRackPreset("#NAMPRACK 99\n", 13, bad, warnings),
              "a version this build cannot read is refused rather than guessed at");

        const std::string truncated = text.substr(0, text.size() / 2);
        check(parseRackPreset(truncated.data(), truncated.size(), bad, warnings) &&
                  bad.nodes.empty() && !warnings.empty(),
              "a truncated file yields no nodes and says why");

        const char *unknownFormat = "#NAMPRACK 1\nnode PRE\nformat CLAP\nkey x\nname Future\nend\n";
        check(parseRackPreset(unknownFormat, std::strlen(unknownFormat), bad, warnings) &&
                  bad.nodes.size() == 1 && !bad.nodes[0].formatKnown &&
                  bad.nodes[0].formatTag == "CLAP",
              "a format from a later build is kept verbatim, not dropped");

        const char *corrupt = "#NAMPRACK 1\nnode PRE\nformat LV2\nkey urn:x\nmix banana\n"
                              "param nine 0.5\nstate !!!!\nunknown-field 1\nend\n";
        check(parseRackPreset(corrupt, std::strlen(corrupt), bad, warnings) &&
                  bad.nodes.size() == 1 && bad.nodes[0].mix == 1.0f && bad.nodes[0].state.empty(),
              "malformed fields are dropped and the node survives with its defaults");
        check(!warnings.empty(), "...and every one of them is reported rather than swallowed");

        // A node that names a plug-in nothing can load must become a placeholder, because dropping
        // it destroys a user's chain because of a plug-in they happened not to install.
        {
            Fixture f;
            RackPreset missing;
            PresetNode node;
            node.section = ChainSection::Pre;
            node.formatTag = "CLAP";
            node.name = "Something from the future";
            node.ref.key = "urn:not-installed";
            missing.nodes.push_back(node);

            ApplyReport report;
            applyRack(f.builder, missing, std::string(), report);
            check(report.placeholders == 1 && f.builder.count(ChainSection::Pre) == 1,
                  "an uninstalled plug-in becomes a placeholder, not a deletion");

            ChainNodeInfo info;
            check(f.builder.nodeInfo(ChainSection::Pre, 0, info) && info.placeholder &&
                      info.ref.key == "urn:not-installed",
                  "...keeping its key, so it is written back out intact");

            RackPreset again;
            captureRack(f.builder, std::string(), again);
            check(again.nodes.size() == 1 && again.nodes[0].ref.key == "urn:not-installed",
                  "...and it survives a save/load cycle it could not participate in");
        }

        // Name validation, because a preset name is concatenated into a path.
        check(rackNameIsSafe("My Board 2"), "an ordinary preset name is accepted");
        check(!rackNameIsSafe("../../etc/passwd"), "a traversal is refused, not sanitised");
        check(!rackNameIsSafe("a/b") && !rackNameIsSafe(".hidden") && !rackNameIsSafe(""),
              "...as are a separator, a leading dot and an empty name");
    }

    std::printf("\nthe plug-in search-path list, which is user-typed and therefore untrusted\n");
    {
        // Everything a scanner is handed comes through here, so a bad row must be refused BEFORE
        // any third-party binary is opened from it — not sanitised into something plausible.
        PluginPaths paths;
        std::string error;

        // THE CASES ARE SPELLED IN THE PLATFORM'S OWN PATHS, because what is under test is the RULE
        // — absolute, no "..", no tab — and this platform's answer to "absolute" is part of the
        // rule rather than a detail of the data. A Windows run against POSIX paths would refuse
        // every row and pass every assertion for the wrong reason.
#if defined(_WIN32)
        const char *kDirA = "C:\\Windows";
        const char *kDirB = "C:\\Windows\\System32";
        const char *kDirBSlash = "C:\\Windows\\System32\\";
        const char *kRelative = "Windows";
        const char *kTraversal = "C:\\Windows\\..\\Users";
        const char *kWithTab = "C:\\Windows\\with\ttab";
        const char *kNoSuchDir = "C:\\no\\such\\directory\\at\\all";
#else
        const char *kDirA = "/tmp";
        const char *kDirB = "/usr";
        const char *kDirBSlash = "/usr/";
        const char *kRelative = "tmp";
        const char *kTraversal = "/tmp/../etc";
        const char *kWithTab = "/tmp/with\ttab";
        const char *kNoSuchDir = "/no/such/directory/at/all";
#endif

        check(paths.add(kDirA, error), "an absolute directory is accepted");
        check(!paths.add(kDirA, error) && error == "already in the list",
              "...once, and the second attempt says why");
        check(!paths.add(kRelative, error), "a relative path is refused");
        check(!paths.add(kTraversal, error), "...as is one containing ..");
        check(!paths.add(kWithTab, error), "...and one carrying a tab, which is a field "
                                           "separator in the files this is written to");
        check(!paths.add(kNoSuchDir, error) && error == "not a directory",
              "a path that is not a directory is refused with the reason");
        check(paths.roots().size() == 1, "and none of the refusals left anything behind");

        // Trailing separators, because /usr/lib/vst3 and /usr/lib/vst3/ would otherwise be two rows
        // walking one tree — and only one of them would match the row the user clicked to remove.
        std::string canonicalError;
        check(paths.add(kDirBSlash, canonicalError) && paths.roots().back() == kDirB,
              "a trailing separator is normalised away");
        check(paths.remove(kDirB), "which is what makes the remove match");
        check(!paths.remove(kDirB), "...and a second remove reports that there was nothing to do");

        // The file round-trip, including a path with the characters that would break the format if
        // they were not escaped. The scratch file goes wherever this platform keeps derived state,
        // which is the same variable the real list is written under.
#if defined(_WIN32)
        const char *stateRoot = std::getenv("LOCALAPPDATA");
        const std::string file =
            std::string(stateRoot ? stateRoot : "C:\\Windows\\Temp") + "\\pluginpaths-check";
#else
        const std::string file = std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") +
                                 "/.cache/NAMp-Rack/pluginpaths-check";
#endif
        PluginPaths saved;
        check(saved.add(kDirA, error), "a list to save");
        check(saved.save(file), "the list writes");
        PluginPaths loaded;
        check(loaded.load(file) && loaded.roots() == saved.roots(), "...and reads back identical");

        // A file this build cannot understand is discarded whole rather than half-read, exactly as
        // the scan cache is. The cost is that the user re-adds their folders once; the alternative
        // is guessing at the meaning of a format that has not been written yet.
        {
            std::ofstream out(file, std::ios::trunc);
            out << "#NAMPPATHS 99\n" << escapeField(kDirA) << '\n';
        }
        PluginPaths future;
        check(!future.load(file) && future.roots().empty(),
              "a version this build does not know is refused rather than partly honoured");

        {
            // Written through escapeField() rather than by hand, because a Windows path IS a string
            // full of the character the format escapes with: an unescaped "C:\\Windows" would come
            // back as "C:Windows" and the row would be dropped for the wrong reason.
            std::ofstream out(file, std::ios::trunc);
            out << "#NAMPPATHS 1\nnot-absolute\n"
                << escapeField(std::string(kTraversal)) << '\n'
                << escapeField(kDirA) << '\n';
        }
        PluginPaths mixed;
        check(mixed.load(file) && mixed.roots().size() == 1 && mixed.roots()[0] == kDirA,
              "unsafe rows are dropped and the rest of the file is still honoured");

        std::error_code ec;
        std::filesystem::remove(file, ec);
    }

    std::printf("\nthe real-time allocation counter, which has to be able to fire\n");
    {
        // The same argument harnessHasTeeth() makes about the malloc interception. This counter
        // reads zero for every plug-in installed here, which is the result we want and is also
        // exactly what a counter wired to nothing would report — so it is made to fire on purpose
        // once, and then shown not to fire outside the scope.
        //
        // IHostApplication::createInstance() is the call being watched: it news a HostMessage or a
        // HostAttributeList, and it is the classic route by which a well-behaved-looking plug-in
        // allocates on the audio thread.
        HostApp *app = hostApp();
        check(app != nullptr, "the host application is published");
        if (app) {
            // createInstance takes TUID by value, which is char[16] and therefore a mutable
            // pointer; the interface's own iid is a const FUID, so it is copied out rather than
            // cast around.
            Steinberg::TUID messageIid = {};
            Steinberg::Vst::IMessage::iid.toTUID(messageIid);

            Steinberg::Vst::IMessage *message = nullptr;
            const uint64_t before = app->rtAllocCount();

            app->createInstance(messageIid, messageIid, reinterpret_cast<void **>(&message));
            check(app->rtAllocCount() == before,
                  "an allocation made off the audio thread is not counted");
            if (message)
                message->release();
            message = nullptr;

            {
                const RtScope scope;
                check(rtScopeActive(), "RtScope raises the flag the counter reads");
                app->createInstance(messageIid, messageIid, reinterpret_cast<void **>(&message));
            }
            check(app->rtAllocCount() == before + 1, "...and the same call inside one IS counted");
            check(!rtScopeActive(), "and the flag is down again afterwards");
            if (message)
                message->release();
        }
    }

    // The chain machinery on its own, with no third-party code anywhere in the call graph. This is
    // the check that attributes an allocation: if this is clean and --rt is not, the allocation
    // belongs to the plug-in, which we can name but not fix.
    std::printf("\nno allocation on the audio path, chain machinery only\n");
    if (!allocationHarnessBuilt()) {
        std::printf("  skipped - this binary was built without the allocation harness: it cannot "
                    "coexist with ThreadSanitizer, and MinGW has neither dlsym(RTLD_NEXT) nor "
                    "<dlfcn.h> for it to interpose through. Every other check above still ran.\n");
    } else {
        if (!harnessHasTeeth())
            return 1;

        int allocations = 0;
        int frees = 0;
        runSyntheticControl(allocations, frees);

        check(allocations == 0 && frees == 0, "500 blocks through 16 nodes, no malloc and no free");
        if (allocations || frees)
            std::printf("        %d allocations, %d frees\n", allocations, frees);
    }

    return gFailures == 0 ? 0 : 1;
}

//------------------------------------------------------------------------
int doRt(PluginFormat format, const std::string &key, int perSide)
{
    if (allocationHarnessBuilt() && !harnessHasTeeth())
        return 1;

    // The control runs BEFORE anything third-party is loaded. The allocation counter is a process
    // global, so a plug-in that keeps a background thread would otherwise leak counts into it and
    // make the control look dirty when the chain is clean.
    int controlAllocs = 0;
    int controlFrees = 0;
    runSyntheticControl(controlAllocs, controlFrees);

    Fixture f;

    PluginRef ref;
    ref.format = format;
    ref.key = key;

    for (int i = 0; i < perSide; ++i) {
        std::string error;
        if (f.builder.add(ChainSection::Pre, ref, error) < 0) {
            std::fprintf(stderr, "chaincheck: pre node %d: %s\n", i, error.c_str());
            return 1;
        }
        if (f.builder.add(ChainSection::Post, ref, error) < 0) {
            std::fprintf(stderr, "chaincheck: post node %d: %s\n", i, error.c_str());
            return 1;
        }
    }
    f.builder.publish();

    for (int32_t i = 0; i < kBlock; ++i)
        f.in[static_cast<size_t>(i)] = 0.25f * std::sin(2.0f * static_cast<float>(M_PI) * 110.0f *
                                                        static_cast<float>(i) / 48000.0f);

    // Warm every lazily-sized buffer before counting. A plug-in that grows a vector on its first
    // call is warming up, not misbehaving on the audio path, and counting that would measure the
    // wrong thing.
    for (int i = 0; i < 32; ++i)
        f.run();

    int allocations = 0;
    int frees = 0;
    countBlocks(f, 500, allocations, frees);

    std::printf("plug-in       %s\n", f.builder.backend(ChainSection::Pre, 0)->displayName());
    std::printf("chain         %d each side of the anchor, 500 blocks of %d frames\n", perSide,
                kBlock);
    if (!allocationHarnessBuilt()) {
        // The chain still RAN — 500 blocks through real plug-ins on real threads, which is the
        // whole point of this build. Only the counter is missing, and saying "0 allocations" here
        // would be reporting the absence of a measurement as a measurement.
        std::printf(
            "allocations   not measured (built without the harness, for ThreadSanitizer)\n");
        std::printf("\nPASSED - the chain ran; see the sanitizer's own output for races\n");
        return 0;
    }
    std::printf("control       %d allocations, %d frees   (chain machinery only)\n", controlAllocs,
                controlFrees);
    std::printf("measured      %d allocations, %d frees\n", allocations, frees);

    if (controlAllocs != 0 || controlFrees != 0) {
        std::printf("\nFAILED - the chain machinery itself allocated. This is ours to fix.\n");
        return 1;
    }
    if (allocations != 0 || frees != 0) {
        // Attribution is the point. The control ran the identical chain code with synthetic
        // backends and stayed at zero, so what is left came in with the plug-in. The no-allocation
        // contract binds our code; a third-party plug-in that allocates while the audio is running
        // is a fact about that plug-in, and naming it is all a host can do.
        std::printf("\nPASSED for the chain - but %d allocations happened while %s was in it. That "
                    "is either its audio thread or a background thread of its own; a process-wide "
                    "counter cannot tell them apart, and neither is something a host can prevent. "
                    "Expect xruns from it under load.\n",
                    allocations, f.builder.backend(ChainSection::Pre, 0)->displayName());
        return 0;
    }
    std::printf("\nPASSED - no allocations on the audio path\n");
    return 0;
}

//========================================================================
// The preset round-trip gate.
//
// "Round-trip a mixed-format chain across a restart with identical audio output" is only a real
// claim if the restart is real, so it is TWO PROCESSES: --preset-save builds a chain, randomises
// it, writes the .namprack and renders the reference audio; --preset-verify is a fresh process that
// knows nothing but the file, loads it, renders again and compares. Doing both halves in one
// process would leave every plug-in already dlopened, every static initialiser already run and the
// scan cache already warm, which is most of what a restart actually changes.
//
// THE RENDER PROCEDURE IS IDENTICAL ON BOTH SIDES, and that is load-bearing rather than tidy. On
// the save side a node's parameters arrive through the ring the audio thread drains; on the verify
// side they arrive as restored state before the instance is ever activated. A plug-in that ramps a
// parameter change would differ in the first block for that reason alone and for no reason worth
// reporting. So both sides run the same silent warm-up, then reset(), then render — and any
// difference left after that is a genuine one.
//========================================================================
namespace preset
{

constexpr int kWarmBlocks = 64;
constexpr int kRenderBlocks = 128;

//------------------------------------------------------------------------
// A deterministic, broadband, non-repeating-within-the-render test signal. A sine would let a
// misaligned chain look right; noise from a fixed seed makes every sample its own witness.
void fillTestSignal(std::vector<float> &out, uint32_t &state)
{
    for (float &v : out) {
        state = state * 1664525u + 1013904223u;
        v = 0.25f * (static_cast<float>(state >> 8) * (1.0f / 8388608.0f) - 1.0f);
    }
}

//------------------------------------------------------------------------
void renderChain(Fixture &f, std::vector<float> &audio)
{
    // Silence first, so every queued parameter edit has landed and any smoothing has settled...
    f.fill(0.0f);
    for (int i = 0; i < kWarmBlocks; ++i)
        f.run();

    // ...then drop the tails that warm-up created, so the render starts from the same state a
    // freshly loaded chain would.
    for (const auto section : {ChainSection::Pre, ChainSection::Post}) {
        for (int i = 0; i < f.builder.count(section); ++i) {
            if (PluginBackend *backend = f.builder.backend(section, i))
                backend->reset();
        }
    }

    audio.clear();
    audio.reserve(static_cast<size_t>(kRenderBlocks) * static_cast<size_t>(kBlock) * 2);
    uint32_t seed = 0x5EED1234u;
    for (int i = 0; i < kRenderBlocks; ++i) {
        fillTestSignal(f.in, seed);
        f.run();
        audio.insert(audio.end(), f.outL.begin(), f.outL.end());
        audio.insert(audio.end(), f.outR.begin(), f.outR.end());
    }
}

//------------------------------------------------------------------------
bool writeAudio(const std::string &path, const std::vector<float> &audio)
{
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    const bool ok = std::fwrite(audio.data(), sizeof(float), audio.size(), file) == audio.size();
    std::fclose(file);
    return ok;
}

bool readAudio(const std::string &path, std::vector<float> &audio)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    audio.assign(static_cast<size_t>(kRenderBlocks) * static_cast<size_t>(kBlock) * 2, 0.0f);
    const bool ok = std::fread(audio.data(), sizeof(float), audio.size(), file) == audio.size();
    std::fclose(file);
    return ok;
}

//------------------------------------------------------------------------
// Every parameter of every node, so the verify side can say whether the SAVE captured the settings
// as well as whether the two renders agree. Two separate claims; a single audio comparison would
// pass if the save had captured nothing and both sides had loaded the same nothing.
bool writeParams(const std::string &path, const ChainBuilder &builder)
{
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    for (const auto section : {ChainSection::Pre, ChainSection::Post}) {
        for (int i = 0; i < builder.count(section); ++i) {
            const PluginBackend *backend = builder.backend(section, i);
            if (!backend)
                continue;
            for (uint32_t p = 0; p < backend->paramCount(); ++p)
                std::fprintf(file, "%.17g\n", backend->paramGet(p));
        }
    }
    std::fclose(file);
    return true;
}

//------------------------------------------------------------------------
int doSave(const std::string &dir, const std::vector<std::pair<ChainSection, std::string>> &plugins)
{
    Fixture f;

    for (const auto &entry : plugins) {
        PluginRef ref;
        // A key containing a unit separator is a VST3 bundle-plus-uid pair; anything else that
        // looks like a URI is LV2. That is the same rule the catalogue writes with.
        ref.format = entry.second.find(kKeySeparator) != std::string::npos ? PluginFormat::Vst3
                                                                           : PluginFormat::Lv2;
        ref.key = entry.second;

        std::string error;
        if (f.builder.add(entry.first, ref, error) < 0) {
            std::fprintf(stderr, "chaincheck: %s: %s\n", entry.second.c_str(), error.c_str());
            return 1;
        }
    }
    f.builder.publish();

    // Randomise, so the preset carries something a default-constructed chain could not produce by
    // accident. Fixed seed, because a gate that fails one run in twenty is not a gate.
    uint32_t seed = 0xC0FFEEu;
    int params = 0;
    for (const auto section : {ChainSection::Pre, ChainSection::Post}) {
        for (int i = 0; i < f.builder.count(section); ++i) {
            PluginBackend *backend = f.builder.backend(section, i);
            if (!backend)
                continue;
            for (uint32_t p = 0; p < backend->paramCount(); ++p) {
                ParamInfo info;
                if (!backend->paramInfo(p, info) || info.isReadOnly || info.isBypass)
                    continue;
                seed = seed * 1664525u + 1013904223u;
                double value = static_cast<double>(seed >> 8) / 16777216.0;
                // A stepped parameter is snapped to its own grid before it is set, so that it can
                // be required to come back EXACTLY. Handing a two-position switch 0.184 and then
                // being surprised that it reads 0 afterwards is measuring the plug-in's rounding,
                // not the preset's fidelity.
                if (info.stepCount >= 1) {
                    const double steps = static_cast<double>(info.stepCount);
                    value = std::round(value * steps) / steps;
                }
                backend->paramSetFromUi(p, value);
                ++params;
            }
        }
    }
    // A node at less than full wet, so the mix and the dry-delay path are part of what round-trips.
    if (f.builder.count(ChainSection::Post) > 0)
        f.builder.setMix(ChainSection::Post, 0, 0.375f);
    f.builder.publish();

    const std::string presetPath = dir + "/chain.namprack";
    std::string error;
    if (!saveRack(f.builder, presetPath, error)) {
        std::fprintf(stderr, "chaincheck: %s\n", error.c_str());
        return 1;
    }
    writeParams(dir + "/params.txt", f.builder);

    std::vector<float> audio;
    renderChain(f, audio);
    if (!writeAudio(dir + "/audio.f32", audio)) {
        std::fprintf(stderr, "chaincheck: cannot write %s/audio.f32\n", dir.c_str());
        return 1;
    }

    std::printf("saved     %s\n", presetPath.c_str());
    std::printf("chain     %d before the amp, %d after; %d parameters randomised\n",
                f.builder.count(ChainSection::Pre), f.builder.count(ChainSection::Post), params);
    std::printf("rendered  %zu samples of reference audio\n", audio.size());
    return 0;
}

//------------------------------------------------------------------------
int doVerify(const std::string &dir)
{
    std::vector<float> reference;
    if (!readAudio(dir + "/audio.f32", reference)) {
        std::fprintf(stderr, "chaincheck: cannot read %s/audio.f32\n", dir.c_str());
        return 1;
    }

    Fixture f;
    ApplyReport report;
    std::string error;
    // Timed, because loading a preset happens on the run loop and the whole of it is a UI stall.
    // This is the number the chainbuilder.h flag about a load worker has to be argued from.
    const auto loadStarted = std::chrono::steady_clock::now();
    const bool loadOk = loadRack(f.builder, dir + "/chain.namprack", report, error);
    const double loadMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStarted)
            .count();
    if (!loadOk) {
        std::fprintf(stderr, "chaincheck: %s\n", error.c_str());
        return 1;
    }

    std::printf("loaded    %d plug-in(s), %d placeholder(s), %d skipped, in %.0f ms (%.0f ms per "
                "plug-in)\n",
                report.loaded, report.placeholders, report.skipped, loadMs,
                report.loaded > 0 ? loadMs / report.loaded : 0.0);
    check(report.loaded > 0, "the preset named plug-ins this build could load");
    check(report.placeholders == 0 && report.skipped == 0,
          "...and every one of them came back as a real instance");

    // Claim one: the save captured the live settings.
    //
    // CONTINUOUS PARAMETERS ARE COMPARED WITH A TOLERANCE, and the reason is a property of VST3
    // rather than a convenience. A plug-in is entitled to quantise: ask for 0.580933 and it may
    // hold 0.58125, or 0.17. The processor then writes ITS value into the state, while the
    // controller — which is what paramGet reads — still reports what it was asked for. So after a
    // save/load cycle the two legitimately differ by the plug-in's own grid step. Measured here:
    // 3.2e-4 for BigBubbleMuff, 4.2e-3 for a plug-in on a 0.01 grid.
    //
    // 0.01 admits every grid seen and is still nowhere near loose enough to hide the failure this
    // check exists to catch, which is a parameter falling back to its DEFAULT — a random value
    // landing within 0.01 of a default happens two times in a hundred, and never for all of them
    // at once. Stepped parameters were snapped above and must match exactly.
    constexpr double kQuantisationTolerance = 0.01;
    {
        std::FILE *file = std::fopen((dir + "/params.txt").c_str(), "rb");
        double worst = 0.0;
        int compared = 0;
        int mismatches = 0;
        if (file) {
            for (const auto section : {ChainSection::Pre, ChainSection::Post}) {
                for (int i = 0; i < f.builder.count(section); ++i) {
                    const PluginBackend *backend = f.builder.backend(section, i);
                    if (!backend)
                        continue;
                    for (uint32_t p = 0; p < backend->paramCount(); ++p) {
                        double want = 0.0;
                        if (std::fscanf(file, "%lf", &want) != 1)
                            break;
                        ParamInfo info;
                        const bool stepped = backend->paramInfo(p, info) && info.stepCount >= 1;
                        const double tolerance = stepped ? 1e-9 : kQuantisationTolerance;
                        const double delta = std::fabs(backend->paramGet(p) - want);
                        // Named, not just counted. "worst deviation 0.96" says a preset is broken;
                        // "GxBooster parameter 3" says which one, which is the difference between
                        // a failing gate and a diagnosable one.
                        if (delta > tolerance && mismatches < 8) {
                            std::printf("          %s parameter %u (%s): saved %.6f, loaded %.6f\n",
                                        backend->displayName(), p, info.name, want,
                                        backend->paramGet(p));
                            ++mismatches;
                        }
                        worst = std::max(worst, delta);
                        ++compared;
                    }
                }
            }
            std::fclose(file);
        }
        std::printf("params    %d compared, worst deviation %.9f, %d past tolerance\n", compared,
                    worst, mismatches);
        check(compared > 0 && mismatches == 0,
              "every parameter came back as it was saved, up to the plug-in's own grid");
    }

    // Claim two: and the audio is the same, sample for sample.
    std::vector<float> audio;
    renderChain(f, audio);
    check(audio.size() == reference.size(), "the render is the same length");

    size_t differing = 0;
    double worst = 0.0;
    const size_t count = std::min(audio.size(), reference.size());
    for (size_t i = 0; i < count; ++i) {
        if (audio[i] != reference[i]) {
            ++differing;
            worst = std::max(worst, std::fabs(static_cast<double>(audio[i]) -
                                              static_cast<double>(reference[i])));
        }
    }
    std::printf("audio     %zu samples, %zu differing, worst |delta| %.9g\n", count, differing,
                worst);
    check(differing == 0, "the reloaded chain renders bit-for-bit identical audio");

    return gFailures == 0 ? 0 : 1;
}

} // namespace preset

//========================================================================
// Risk 19: the end-of-chain clamp, measured rather than argued about.
//
// "Replace isfinite() with a branch-free exponent test" is plausible and was unmeasured, and an
// unmeasured number is not one this project ships against. Both implementations live in
// chainengine.cpp so they are compiled under the same flags as each other and as the shipping
// code — this file is built at -O0 on purpose (see CMakeLists.txt), so a copy benchmarked here
// would answer a question nobody asked.
//
// Two signal profiles, because they exercise different branches: ordinary audio, where every sample
// is finite and in range, and a poisoned buffer, where a plug-in has gone unstable. The first is
// the case that runs a billion times; the second is the case the pass exists for.
//========================================================================
int doClampBench()
{
    constexpr int kFrames = 256;
    constexpr int kIterations = 200000;

    std::vector<float> left(kFrames), right(kFrames);
    std::vector<float> seedL(kFrames), seedR(kFrames);

    auto fillOrdinary = [&] {
        uint32_t state = 0x1234567u;
        for (int i = 0; i < kFrames; ++i) {
            state = state * 1664525u + 1013904223u;
            seedL[static_cast<size_t>(i)] =
                0.5f * (static_cast<float>(state >> 8) * (1.0f / 8388608.0f) - 1.0f);
            state = state * 1664525u + 1013904223u;
            seedR[static_cast<size_t>(i)] =
                0.5f * (static_cast<float>(state >> 8) * (1.0f / 8388608.0f) - 1.0f);
        }
    };
    auto fillPoisoned = [&] {
        fillOrdinary();
        for (int i = 0; i < kFrames; i += 4) {
            seedL[static_cast<size_t>(i)] = std::nanf("");
            seedR[static_cast<size_t>(i)] = 40.0f;
        }
    };

    auto time = [&](void (*fn)(float *, float *, int32_t)) {
        // Re-seeded every iteration so the input is identical for both candidates and so a clamped
        // buffer cannot become a cheaper input to the next pass.
        const auto started = std::chrono::steady_clock::now();
        for (int it = 0; it < kIterations; ++it) {
            std::memcpy(left.data(), seedL.data(), sizeof(float) * kFrames);
            std::memcpy(right.data(), seedR.data(), sizeof(float) * kFrames);
            fn(left.data(), right.data(), kFrames);
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        return std::chrono::duration<double, std::nano>(elapsed).count() /
               (static_cast<double>(kIterations) * kFrames * 2.0);
    };

    // The memcpy is inside the timed loop for both, so subtract what it costs on its own rather
    // than reporting it as part of the clamp.
    auto nothing = [](float *, float *, int32_t) {};

    std::printf("%d frames x %d iterations, nanoseconds per sample\n\n", kFrames, kIterations);
    std::printf("%-22s %12s %12s\n", "", "ordinary", "poisoned");

    double results[2][2] = {};
    const char *names[2] = {"isfinite (shipping)", "exponent test"};
    void (*fns[2])(float *, float *, int32_t) = {clampChainOutput, clampChainOutputExponent};

    for (int profile = 0; profile < 2; ++profile) {
        if (profile == 0)
            fillOrdinary();
        else
            fillPoisoned();
        const double overhead = time(nothing);
        for (int candidate = 0; candidate < 2; ++candidate)
            results[candidate][profile] = time(fns[candidate]) - overhead;
    }

    for (int candidate = 0; candidate < 2; ++candidate)
        std::printf("%-22s %12.4f %12.4f\n", names[candidate], results[candidate][0],
                    results[candidate][1]);

    // A margin, not a win by a hair. Anything inside it is measurement noise dressed up as a
    // decision, and swapping a plainly readable isfinite() loop for bit-twiddling on that basis is
    // the trade the rules exist to prevent.
    constexpr double kMargin = 1.15;
    const bool fasterOrdinary = results[0][0] > results[1][0] * kMargin;
    const bool fasterPoisoned = results[0][1] > results[1][1] * kMargin;

    std::printf("\n");
    if (fasterOrdinary && fasterPoisoned)
        std::printf("VERDICT - the exponent test is more than %.0f%% faster on both profiles; "
                    "adopt it.\n",
                    (kMargin - 1.0) * 100.0);
    else
        std::printf("VERDICT - no margin worth the loss of clarity. The shipping isfinite() pass "
                    "stays, and risk 19 is answered with a number rather than an opinion.\n");
    return 0;
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s --routing\n"
                     "       %s --rt <key> | --rt-lv2 <uri> [pedals per side]\n"
                     "       %s --preset-save <dir> pre:<key> post:<key> ...\n"
                     "       %s --preset-verify <dir>\n",
                     argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    if (!installHostApp()) {
        std::fprintf(stderr, "chaincheck: a plug-in context is already installed\n");
        return 1;
    }

    int rc = 2;
    if (std::strcmp(argv[1], "--routing") == 0) {
        rc = doRouting();
        std::printf("\n%s\n", gFailures == 0 ? "PASSED - routing is what the chain compiler claims"
                                             : "FAILED");
    } else if (argc >= 3 && std::strcmp(argv[1], "--rt") == 0) {
        const int perSide = (argc >= 4) ? std::atoi(argv[3]) : 2;
        rc = doRt(PluginFormat::Vst3, argv[2], perSide < 1 ? 1 : perSide);
    } else if (argc >= 3 && std::strcmp(argv[1], "--rt-lv2") == 0) {
        // Same measurement, an LV2 URI instead of a VST3 key. Worth its own verb rather than a
        // format guess: a key that happens to parse as a URI is not evidence of anything.
        const int perSide = (argc >= 4) ? std::atoi(argv[3]) : 2;
        rc = doRt(PluginFormat::Lv2, argv[2], perSide < 1 ? 1 : perSide);
    } else if (argc >= 4 && std::strcmp(argv[1], "--preset-save") == 0) {
        std::vector<std::pair<ChainSection, std::string>> plugins;
        for (int i = 3; i < argc; ++i) {
            const std::string spec = argv[i];
            if (spec.compare(0, 4, "pre:") == 0)
                plugins.emplace_back(ChainSection::Pre, spec.substr(4));
            else if (spec.compare(0, 5, "post:") == 0)
                plugins.emplace_back(ChainSection::Post, spec.substr(5));
            else
                std::fprintf(stderr, "chaincheck: '%s' is not pre:<key> or post:<key>\n",
                             spec.c_str());
        }
        rc = preset::doSave(argv[2], plugins);
    } else if (std::strcmp(argv[1], "--clamp-bench") == 0) {
        rc = doClampBench();
    } else if (argc >= 3 && std::strcmp(argv[1], "--preset-verify") == 0) {
        rc = preset::doVerify(argv[2]);
        std::printf("\n%s\n", gFailures == 0
                                  ? "PASSED - the chain survived a restart with the same audio"
                                  : "FAILED");
    } else {
        std::fprintf(stderr,
                     "usage: %s --routing | --rt <key> | --rt-lv2 <uri> [pedals per side]\n"
                     "       %s --preset-save <dir> pre:<key> post:<key> ... | --preset-verify "
                     "<dir>\n",
                     argv[0], argv[0]);
    }

    uninstallHostApp();
    return rc;
}
