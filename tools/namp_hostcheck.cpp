// namp_hostcheck — the phase gate for the plug-in host, with no UI and no audio device.
//
// Three jobs:
//
//   --list          rebuild the catalogue and print it, reporting how many bundles were actually
//                   probed as opposed to answered from the cache. A warm run probing zero is the
//                   whole point of the cache.
//   --load <key>    load one plug-in through the backend, configure it, run blocks of silence
//                   through it and tear it down. This is the smoke test that says the four
//                   decisions in backend_vst3.h actually hold against real plug-ins.
//   --params <key>  as --load, then print the parameter list the generic panel would draw.
//   --state <key>   the control for the preset round-trip: set every parameter, save the plug-in's
//                   own state, push everything back to a default, restore, and see what came back.
//                   ONE plug-in, in ONE process, with no file and no restart — so a failure here is
//                   the plug-in losing its own state, and cannot be the preset code's doing.
//
// It links the host library and nothing else — no X11, no Cairo, no JACK — which is also a standing
// check that NampHost really is free of those.

#include "host/backend_vst3.h"
#include "host/catalog.h"
#include "host/pluginpaths.h"
#include "host/chainbuilder.h"
#include "host/diagnostics.h"
#include "host/hostapp.h"
#include "host/scanchild.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace NAMp::host;

namespace
{

constexpr int kBlockSize = 256;
constexpr int kBlocks = 1000;
constexpr double kSampleRate = 48000.0;

//------------------------------------------------------------------------
int doList()
{
    Catalog catalog;
    catalog.rescan();

    for (const auto &e : catalog.entries()) {
        std::printf("%-5s  %-40s  %s\n", formatTag(e.ref.format), e.name.c_str(),
                    e.ref.key.c_str());
    }
    std::printf("\n%zu plug-in(s); %d bundle(s) probed; cache %s\n", catalog.entries().size(),
                catalog.probedCount(),
                catalog.cachePath().empty() ? "(none)" : catalog.cachePath().c_str());
    return 0;
}

//------------------------------------------------------------------------
// Does adding and removing a search folder take effect WITHOUT a restart?
//
// It is a fair question for both halves and they fail differently. A VST3 folder is walked afresh
// on every scan, so removal is automatic. An LV2 folder is not: lilv has no way to take a bundle
// back out from under a plug-in that might be playing, so removal is a filter rather than an
// unload — see Lv2World::loadRoots — and a filter is exactly the kind of thing that silently
// stops matching. Hence one process, three scans, and a count each time.
int doPaths(const char *dir)
{
    PluginPaths paths;
    Catalog catalog;
    catalog.setSearchPaths(&paths);

    catalog.rescan();
    const int before = static_cast<int>(catalog.entries().size());
    std::printf("without %s: %d plug-in(s)\n", dir, before);

    std::string error;
    if (!paths.add(dir, error)) {
        std::fprintf(stderr, "hostcheck: cannot add %s: %s\n", dir, error.c_str());
        return 1;
    }
    catalog.rescan();
    const int added = static_cast<int>(catalog.entries().size());
    std::printf("with it:     %d plug-in(s)  (+%d, %d probed)\n", added, added - before,
                catalog.probedCount());

    paths.remove(dir);
    catalog.rescan();
    const int after = static_cast<int>(catalog.entries().size());
    std::printf("removed:     %d plug-in(s)\n", after);

    const bool ok = added > before && after == before;
    std::printf("%s\n", ok ? "PASSED - the folder took effect and stopped taking effect, both in "
                             "this one process"
                           : "FAILED - adding or removing the folder did not change the "
                             "catalogue");
    return ok ? 0 : 1;
}

//------------------------------------------------------------------------
// Runs `blocks` of silence through the backend. Returns false if anything refused to configure.
bool runBlocks(PluginBackend &backend, int channels, bool verbose)
{
    ProcessConfig config;
    config.sampleRate = kSampleRate;
    config.maxBlock = kBlockSize;
    config.channels = channels;

    if (!backend.prepare(config)) {
        std::fprintf(stderr, "hostcheck: prepare failed\n");
        return false;
    }
    backend.activate();

    // Distinct input and output buses, which is what the chain compiler always gives a VST3 node:
    // ping-pong slots mean no copies and no aliasing.
    std::vector<float> inBuf(static_cast<size_t>(channels) * kBlockSize, 0.0f);
    std::vector<float> outBuf(static_cast<size_t>(channels) * kBlockSize, 0.0f);
    std::vector<float *> in(static_cast<size_t>(channels));
    std::vector<float *> out(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) {
        in[static_cast<size_t>(c)] = inBuf.data() + c * kBlockSize;
        out[static_cast<size_t>(c)] = outBuf.data() + c * kBlockSize;
    }

    AudioBlock block;
    block.in = in.data();
    block.out = out.data();
    block.channels = channels;
    block.frames = kBlockSize;

    for (int i = 0; i < kBlocks; ++i)
        backend.process(block);

    if (verbose) {
        // Printed per pass, because the interesting question is whether the plug-in ACCEPTED the
        // channel count the chain asked for. A pre-amp node asked for mono and answering 2 is the
        // documented copy fallback, not a failure.
        std::printf("  asked %d, got    : %d in / %d out%s\n", channels, backend.audioInCount(),
                    backend.audioOutCount(),
                    (backend.audioInCount() == channels && backend.audioOutCount() == channels)
                        ? ""
                        : "   [copy fallback]");
        std::printf("  latency         : %u samples\n", backend.latencySamples());
        std::printf("  parameters      : %u\n", backend.paramCount());
        std::printf("  file loader     : %s\n", backend.hasFileLoader() ? "yes" : "no");
        if (diagArmed()) {
            std::printf("  worst-case      : %lld us (budget %.1f us)\n",
                        static_cast<long long>(backend.diagTakeMaxMicros()),
                        kBlockSize / kSampleRate * 1e6);
            std::printf("  RT allocations  : %llu\n",
                        static_cast<unsigned long long>(backend.diagRtAllocCount()));
        }
    }

    backend.deactivate();
    return true;
}

//------------------------------------------------------------------------
int doLoad(PluginFormat format, const std::string &key, bool listParams)
{
    PluginRef ref;
    ref.format = format;
    ref.key = key;

    // Through the same factory the chain builder uses, rather than a backend constructor directly.
    // A tool that reaches past the dispatch is a tool that can pass while the shipping path is
    // broken.
    std::string error;
    PluginBackend *backend = createBackend(ref, error);
    if (!backend) {
        std::fprintf(stderr, "hostcheck: %s\n", error.c_str());
        return 1;
    }

    static const char *const kEditorKindName[] = {"none (generic panel)", "VST3 IPlugView",
                                                  "LV2 ui:X11UI (embedded)",
                                                  "LV2 ui:showInterface (own window)"};
    std::printf("%s\n", backend->displayName());
    std::printf("  editor          : %s\n",
                kEditorKindName[static_cast<int>(backend->editorKind())]);

    // Mono first, which is the pre-amp section of a real chain, then stereo. A plug-in that
    // refuses mono takes the documented copy fallback rather than failing.
    bool ok = runBlocks(*backend, 1, true);
    if (ok)
        ok = runBlocks(*backend, 2, true);

    if (ok && listParams) {
        std::printf("\n");
        for (uint32_t i = 0; i < backend->paramCount(); ++i) {
            ParamInfo info;
            if (!backend->paramInfo(i, info))
                continue;
            char shown[128] = {};
            backend->paramDisplay(i, backend->paramGet(i), shown, sizeof(shown));
            std::printf("  %3u  %-32s %-10s %s%s\n", i, info.name, shown,
                        info.stepCount ? "[stepped] " : "", info.isReadOnly ? "[read-only]" : "");
        }
    }

    // Exercising a real teardown is half the point of this tool: an editor-less unload that leaks
    // or crashes is a bug we want to see here rather than in a host.
    delete backend;
    return ok ? 0 : 1;
}

} // namespace

//------------------------------------------------------------------------
// The narrowest possible question: can this plug-in save and restore itself, at all?
//
// The preset round-trip is a long chain of things — a file format, base64, a restart, a rebuilt
// instance — and when it fails the first thing worth knowing is whether the plug-in can do the one
// thing all of that rests on. This does nothing but ask it, in memory.
int doStateSelfTest(PluginFormat format, const std::string &key)
{
    PluginRef ref;
    ref.format = format;
    ref.key = key;

    std::string error;
    std::unique_ptr<PluginBackend> backend(createBackend(ref, error));
    if (!backend) {
        std::fprintf(stderr, "hostcheck: %s\n", error.c_str());
        return 1;
    }

    ProcessConfig config;
    config.sampleRate = kSampleRate;
    config.maxBlock = kBlockSize;
    config.channels = 2;
    if (!backend->prepare(config)) {
        std::fprintf(stderr, "hostcheck: %s refused the process configuration\n",
                     backend->displayName());
        return 1;
    }
    backend->activate();

    // A MIDI controller destination is skipped everywhere below, and it is not a lenience.
    // VST3 routes an incoming CC by writing it onto a parameter, so a plug-in that accepts CCs
    // publishes a block of parameters that hold whatever the last controller said — never anything
    // a user set. Persisting one would be the bug; not persisting it is the contract. Counting them
    // here reported four different plug-ins as losing 127 parameters each, all of them behaving
    // correctly, which is exactly the noise that makes a real regression invisible.
    const uint32_t count = backend->paramCount();
    std::vector<double> wanted(count, 0.0);
    uint32_t skipped = 0;
    uint32_t seed = 0xABCDEFu;
    for (uint32_t p = 0; p < count; ++p) {
        ParamInfo info;
        if (!backend->paramInfo(p, info) || info.isReadOnly || info.isMidiMapped) {
            if (info.isMidiMapped)
                ++skipped;
            wanted[p] = backend->paramGet(p);
            continue;
        }
        seed = seed * 1664525u + 1013904223u;
        double value = static_cast<double>(seed >> 8) / 16777216.0;
        if (info.stepCount >= 1)
            value = std::round(value * info.stepCount) / info.stepCount;
        backend->paramSetFromUi(p, value);
        wanted[p] = value;
    }
    backend->paramFlushToPlugin();

    std::vector<uint8_t> blob;
    if (!backend->stateSave(blob)) {
        std::printf("%s: stateSave() refused\n", backend->displayName());
        return 1;
    }
    std::printf("%s: %u parameters, %zu bytes of state\n", backend->displayName(), count,
                blob.size());
    if (skipped > 0)
        std::printf("  (%u of them are MIDI controller destinations and are not state; see the "
                    "note above this check)\n",
                    skipped);

    // Move everything somewhere else, so a restore that does nothing at all cannot look like a
    // restore that worked.
    for (uint32_t p = 0; p < count; ++p) {
        ParamInfo info;
        if (backend->paramInfo(p, info) && !info.isReadOnly && !info.isMidiMapped)
            backend->paramSetFromUi(p, info.defaultNormalized);
    }
    backend->paramFlushToPlugin();

    backend->deactivate();
    const bool restored = backend->stateLoad(blob.data(), blob.size());
    backend->activate();
    if (!restored) {
        std::printf("%s: stateLoad() refused its own blob\n", backend->displayName());
        return 1;
    }

    int lost = 0;
    double worst = 0.0;
    for (uint32_t p = 0; p < count; ++p) {
        ParamInfo info;
        if (!backend->paramInfo(p, info) || info.isReadOnly || info.isMidiMapped)
            continue;
        const double got = backend->paramGet(p);
        const double delta = std::fabs(got - wanted[p]);
        worst = std::max(worst, delta);
        if (delta > 0.01) {
            if (lost < 8)
                std::printf("  lost   %-28s set %.6f, restored %.6f%s\n", info.name, wanted[p], got,
                            std::fabs(got - info.defaultNormalized) < 1e-9 ? "  (its default)"
                                                                           : "");
            ++lost;
        }
    }

    backend->deactivate();
    if (lost == 0) {
        std::printf("PASSED - the plug-in saves and restores its own parameters (worst grid "
                    "deviation %.6f)\n",
                    worst);
        return 0;
    }
    std::printf("FAILED - %d of %u settings did not survive the plug-in's OWN save/restore, with "
                "no file and no restart involved. Nothing a host does can fix that.\n",
                lost, count - skipped);
    return 1;
}

//------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    // Before anything else: this binary is its own scan helper, so --list probes plug-ins out of
    // process exactly the way the standalone does. See src/host/scanchild.h.
    int scanExit = 0;
    if (runScanChildIfRequested(argc, argv, scanExit))
        return scanExit;

    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s --list | --load <key> | --params <key> | --state <key>\n"
                     "       %s --load-lv2 <uri> | --params-lv2 <uri> | --state-lv2 <uri>\n"
                     "       %s --paths <dir>\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }

    if (!installHostApp()) {
        std::fprintf(stderr, "hostcheck: a plug-in context is already installed\n");
        return 1;
    }

    int rc = 2;
    if (std::strcmp(argv[1], "--list") == 0) {
        rc = doList();
    } else if (argc >= 3 && std::strcmp(argv[1], "--load") == 0) {
        rc = doLoad(PluginFormat::Vst3, argv[2], false);
    } else if (argc >= 3 && std::strcmp(argv[1], "--params") == 0) {
        rc = doLoad(PluginFormat::Vst3, argv[2], true);
    } else if (argc >= 3 && std::strcmp(argv[1], "--load-lv2") == 0) {
        rc = doLoad(PluginFormat::Lv2, argv[2], false);
    } else if (argc >= 3 && std::strcmp(argv[1], "--params-lv2") == 0) {
        rc = doLoad(PluginFormat::Lv2, argv[2], true);
    } else if (argc >= 3 && std::strcmp(argv[1], "--state") == 0) {
        rc = doStateSelfTest(PluginFormat::Vst3, argv[2]);
    } else if (argc >= 3 && std::strcmp(argv[1], "--state-lv2") == 0) {
        rc = doStateSelfTest(PluginFormat::Lv2, argv[2]);
    } else if (argc >= 3 && std::strcmp(argv[1], "--paths") == 0) {
        rc = doPaths(argv[2]);
    } else {
        std::fprintf(stderr,
                     "usage: %s --list | --load <key> | --params <key> | --state <key>\n"
                     "       %s --load-lv2 <uri> | --params-lv2 <uri> | --state-lv2 <uri>\n"
                     "       %s --paths <dir>\n",
                     argv[0], argv[0], argv[0]);
    }

    uninstallHostApp();
    return rc;
}
