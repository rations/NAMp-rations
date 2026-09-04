// namp-rations-standalone - play the amp head without a DAW.
//
// A deliberately small host: one top-level X window, the plug-in's own editor embedded inside it,
// a run loop the plug-in can register with, and a JACK client feeding the processor. It exists so
// Rations is usable on its own, and it is NOT a general-purpose host - there is no rack here, no
// plug-in scanning and nothing hosted but this one plug-in. The pedalboard is built into the
// plug-in, which is exactly why none of that is needed.
//
// The plug-in is loaded as a bundle through the SDK's module loader rather than linked in. That
// matters: the editor locates its art and fonts with dladdr() relative to its own .so
// (src/platform/respath.cpp), so it must genuinely be a loaded module for the resource paths to
// resolve the same way they do inside a DAW.
//
// Threading: everything except the JACK callback runs on this thread. The editor, the controller
// and the run loop are all single-threaded here, which is the same contract a DAW provides.

#include "jackclient.h"
#include "midiroute.h"
#include "runloop.h"

#include "rationsids.h"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace Steinberg;

namespace
{

// The size the editor comes up at is the editor's own (its head page at scale 1.0); these are only
// the last resort for a view that refuses to report one.
constexpr int kFallbackW = 1133;
constexpr int kFallbackH = 403;

Rations::RunLoop *gRunLoop = nullptr;

void onSignal(int)
{
    if (gRunLoop)
        gRunLoop->stop();
}

//------------------------------------------------------------------------
// The host end of the VST3 edit loop. The editor calls performEdit() when the user moves a
// control; we forward the value to the processor through the JACK client's lock-free ring.
class ComponentHandler : public Vst::IComponentHandler
{
public:
    explicit ComponentHandler(Rations::JackClient &jack) : mJack(jack)
    {
    }

    tresult PLUGIN_API beginEdit(Vst::ParamID) SMTG_OVERRIDE
    {
        return kResultOk;
    }

    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) SMTG_OVERRIDE
    {
        mJack.pushParameter(id, value);
        return kResultOk;
    }

    tresult PLUGIN_API endEdit(Vst::ParamID) SMTG_OVERRIDE
    {
        return kResultOk;
    }

    tresult PLUGIN_API restartComponent(int32) SMTG_OVERRIDE
    {
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IComponentHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    Rations::JackClient &mJack;
};

//------------------------------------------------------------------------
// Re-runs setupProcessing when JACK changes its buffer size under the running client. The chunk
// loop in JackClient keeps audio correct without this - no block ever reaches the processor larger
// than the size it was set up for - but the processor would otherwise stay configured for the size
// it saw at startup, sizing its internal buffers and its reported latency for a block the host is
// no longer sending.
//
// This runs on the run loop, not in JACK's buffer-size callback: setActive and setupProcessing are
// VST3 main-thread calls and the plug-in's message thread may be part-way through a load.
// JackClient::suspendProcessing() is what keeps the audio callback out of the processor while it
// is reconfigured.
class BufferSizeWatcher : public Linux::ITimerHandler
{
public:
    BufferSizeWatcher(Rations::JackClient &jack, Vst::IComponent *component,
                      Vst::IAudioProcessor *processor, const Vst::ProcessSetup &setup)
        : mJack(jack), mComponent(component), mProcessor(processor), mSetup(setup)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        const int size = mJack.takeBufferSizeChange();
        if (size <= 0)
            return;

        if (!mJack.suspendProcessing()) {
            fprintf(
                stderr,
                "namp-rations-standalone: the audio thread did not respond, so the processor was "
                "left set up for %d frames\n",
                mJack.blockSize());
            return;
        }

        mProcessor->setProcessing(false);
        mComponent->setActive(false);

        Vst::ProcessSetup setup = mSetup;
        setup.maxSamplesPerBlock = size;
        const bool ok = mProcessor->setupProcessing(setup) == kResultOk;
        if (ok)
            mSetup = setup;
        else
            fprintf(stderr, "namp-rations-standalone: the plug-in refused %d frames; keeping %d\n",
                    size, mSetup.maxSamplesPerBlock);

        mComponent->setActive(true);
        mProcessor->setProcessing(true);
        // Only adopt the new chunk size if the plug-in accepted it. If it did not, the old size is
        // still what it is prepared for, and the chunk loop must keep honouring that.
        mJack.resumeProcessing(ok ? size : mSetup.maxSamplesPerBlock);

        printf("namp-rations-standalone: JACK buffer size is now %d frames\n", mJack.blockSize());
        fflush(stdout);
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    Rations::JackClient &mJack;
    Vst::IComponent *mComponent = nullptr;
    Vst::IAudioProcessor *mProcessor = nullptr;
    Vst::ProcessSetup mSetup;
};

//------------------------------------------------------------------------
// Feeds everything the audio thread published back into the controller. Runs as a run-loop timer
// on the UI thread, so no IEditController call ever happens on the RT thread.
//
// THIS IS THE HOST'S HALF OF A LOOP, and it carries more than the meters. A VST3 plug-in reports
// a parameter IT changed by itself through outputParameterChanges, and the host is what turns
// that back into IEditController::setParamNormalized so the panel agrees with the audio. Rations
// changes parameters by itself whenever the MIDI learn table fires - a stomp is the plug-in
// moving its own channel switch or its own pedal switch - so a pump that forwarded only the
// hidden meter parameters left a footswitch that changed the SOUND while the panel sat still: the
// bat switch stayed where it was and the pedal's lamp stayed dark. Nothing was wrong with the
// plug-in; the host end of the loop was missing.
//
// Only slots whose sequence number has moved are forwarded, so the meters - which arrive every
// block - do not turn into a redraw storm.
class FeedbackPump : public Linux::ITimerHandler
{
public:
    FeedbackPump(Rations::JackClient &jack, Vst::IEditController *controller)
        : mJack(jack), mController(controller)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        if (!mController)
            return;
        for (int i = 0; i < Rations::JackClient::kFeedbackSlots; ++i) {
            Vst::ParamID id = Vst::kNoParamId;
            double value = 0.0;
            uint32_t seq = 0;
            if (!mJack.readFeedback(i, id, value, seq))
                break; // slots are filled in order; the first empty one is the end
            if (seq == mSeen[i])
                continue;
            mSeen[i] = seq;
            mController->setParamNormalized(id, value);
        }
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    Rations::JackClient &mJack;
    Vst::IEditController *mController;
    // The sequence number last forwarded for each slot. Zero is "never seen", and the audio thread
    // increments before it ever publishes, so slot values always start out looking new.
    uint32_t mSeen[Rations::JackClient::kFeedbackSlots] = {};
};

//------------------------------------------------------------------------
// Where to look for NAMp-rations.vst3 when no path is given on the command line. The standalone is
// a host: it loads the same bundle a DAW would, rather than linking the plug-in in, so that
// dladdr-based resource lookup behaves identically in both. That means it has to be able to FIND
// the bundle - covering the release tarball (bundle beside the binary), the build tree, and a
// system or per-user VST3 install.
std::string findBundle()
{
    std::vector<std::string> candidates;

    if (const char *env = getenv("RATIONS_VST3"))
        candidates.emplace_back(env);

    char exe[4096] = {0};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        std::string dir(exe, static_cast<size_t>(n));
        const size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos)
            dir.resize(slash);
        candidates.push_back(dir + "/NAMp-rations.vst3");              // release tarball
        candidates.push_back(dir + "/VST3/Release/NAMp-rations.vst3"); // build tree
    }

    if (const char *home = getenv("HOME"))
        candidates.push_back(std::string(home) + "/.vst3/NAMp-rations.vst3");
    candidates.push_back("/usr/local/lib/vst3/NAMp-rations.vst3");
    candidates.push_back("/usr/lib/vst3/NAMp-rations.vst3");

    std::error_code ec;
    for (const auto &path : candidates)
        if (std::filesystem::is_directory(path, ec))
            return path;

    return candidates.empty() ? std::string() : candidates.front();
}

//------------------------------------------------------------------------
// Settings file. A four-channel amp that forgets which captures it was playing every time it
// starts is not usable - there are four banks, two impulse responses, nine MIDI bindings and five
// pedals behind this - and a DAW would keep all of it in the project.
std::string statePath()
{
    std::string dir;
    if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        dir = xdg;
    else if (const char *home = getenv("HOME"); home && *home)
        dir = std::string(home) + "/.config";
    else
        return {};
    return dir + "/NAMp-rations/standalone.state";
}

// Restores what the last session was playing. A truncated or foreign file is not an error worth
// stopping for: the plug-in is required to reject a bad blob cleanly, and the standalone then
// simply starts with four empty channels - which is an ordinary state here, not a broken one.
void loadState(Vst::IComponent *component, Vst::IEditController *controller)
{
    const std::string path = statePath();
    if (path.empty())
        return;
    FILE *file = fopen(path.c_str(), "rb");
    if (!file)
        return;

    std::vector<char> bytes;
    char buffer[4096];
    size_t got = 0;
    // A settings file this large is not one we wrote; stop reading rather than grow without bound
    // on a file someone else put there.
    constexpr size_t kMaxState = 1u << 20;
    bool tooLarge = false;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + got);
        if (bytes.size() > kMaxState) {
            tooLarge = true;
            break;
        }
    }
    fclose(file);
    if (tooLarge) {
        fprintf(stderr, "namp-rations-standalone: %s is implausibly large; ignoring it\n",
                path.c_str());
        return;
    }
    if (bytes.empty())
        return;

    MemoryStream stream(bytes.data(), static_cast<TSize>(bytes.size()));
    if (component->setState(&stream) != kResultOk) {
        fprintf(stderr, "namp-rations-standalone: %s was rejected; starting empty\n", path.c_str());
        return;
    }
    // The controller reads the SAME blob from the start, which is what a host does. Its reader is
    // a second walk over the same bytes, so the two must be handed identical input or the panel
    // shows something the audio does not agree with.
    int64 ignored = 0;
    stream.seek(0, IBStream::kIBSeekSet, &ignored);
    controller->setComponentState(&stream);
}

void saveState(Vst::IComponent *component)
{
    const std::string path = statePath();
    if (path.empty())
        return;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec)
        return;

    MemoryStream stream;
    if (component->getState(&stream) != kResultOk)
        return;

    FILE *file = fopen(path.c_str(), "wb");
    if (!file) {
        fprintf(stderr, "namp-rations-standalone: cannot write %s\n", path.c_str());
        return;
    }
    const size_t size = static_cast<size_t>(stream.getSize());
    if (size > 0 && fwrite(stream.getData(), 1, size, file) != size)
        fprintf(stderr, "namp-rations-standalone: short write to %s\n", path.c_str());
    fclose(file);
}

//------------------------------------------------------------------------
void printUsage()
{
    printf("usage: namp-rations-standalone [options] [path to NAMp-rations.vst3]\n"
           "\n"
           "  NAMp Rations as a JACK application: the plug-in's own editor in a window of its\n"
           "  own, with the amp on JACK's ports. A JACK server must already be running.\n"
           "\n"
           "  --no-state    do not read or write %s\n"
           "  -h, --help    this message\n"
           "\n"
           "  Ports: NAMp-rations:in, NAMp-rations:out_l, NAMp-rations:out_r (connected to the\n"
           "  first physical ports found) and NAMp-rations:midi_in for a footswitch, which is\n"
           "  left unconnected because guessing which MIDI source is the pedal would be worse\n"
           "  than not trying.\n"
           "\n"
           "  The bundle is searched for in $RATIONS_VST3, then beside this binary, then the\n"
           "  build tree, ~/.vst3, /usr/local/lib/vst3 and /usr/lib/vst3.\n",
           statePath().empty() ? "the settings file" : statePath().c_str());
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    std::string modulePath;
    bool useState = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
        if (arg == "--no-state") {
            useState = false;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "namp-rations-standalone: unknown option %s\n", arg.c_str());
            printUsage();
            return 2;
        }
        modulePath = arg;
    }
    if (modulePath.empty())
        modulePath = findBundle();

    // The host context must be published BEFORE the plug-in is instantiated:
    // ComponentBase::allocateMessage() asks it for IMessage instances, so without one every
    // controller->processor message (the four capture banks, the two IRs, Slim) is silently
    // dropped and only parameter changes get through. That presents as a plug-in whose knobs work
    // and which never loads a capture.
    Vst::HostApplication hostContext;
    Vst::PluginContextFactory::instance().setPluginContext(&hostContext);

    std::string error;
    auto module = VST3::Hosting::Module::create(modulePath, error);
    if (!module) {
        fprintf(stderr, "namp-rations-standalone: cannot load %s\n  %s\n", modulePath.c_str(),
                error.c_str());
        printUsage();
        return 1;
    }

    // --- instantiate the plug-in -------------------------------------
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
        fprintf(stderr, "namp-rations-standalone: no audio effect class in %s\n",
                modulePath.c_str());
        return 1;
    }

    Vst::IComponent *component = provider->getComponent();
    Vst::IEditController *controller = provider->getController();
    if (!component || !controller) {
        fprintf(stderr, "namp-rations-standalone: the plug-in did not provide both parts\n");
        return 1;
    }

    FUnknownPtr<Vst::IAudioProcessor> processor(component);
    if (!processor) {
        fprintf(stderr, "namp-rations-standalone: the plug-in has no IAudioProcessor\n");
        return 1;
    }

    // --- audio -------------------------------------------------------
    // Where each kind of MIDI message has to be delivered, worked out here on the main thread
    // because both lookups are IEditController calls and the audio thread may never make one.
    //
    // Declared BEFORE the JACK client on purpose: the client reads it from the audio thread, so it
    // has to outlive the client, and destruction runs in reverse declaration order. That matters
    // on the early-return paths below, where nothing has called jack.close() and the destructor is
    // what stops the audio thread.
    Rations::MidiRoute route;
    route.resolve(controller);

    Rations::JackClient jack;

    // A first connection just to learn the server's rate and block size, so setupProcessing can be
    // told the truth before the component is activated.
    jack_status_t status = static_cast<jack_status_t>(0);
    jack_client_t *probe = jack_client_open("NAMp-rations-probe", JackNoStartServer, &status);
    double sampleRate = 48000.0;
    int blockSize = 1024;
    if (probe) {
        sampleRate = static_cast<double>(jack_get_sample_rate(probe));
        blockSize = static_cast<int>(jack_get_buffer_size(probe));
        jack_client_close(probe);
    } else {
        fprintf(stderr,
                "namp-rations-standalone: no JACK server; continuing with the editor only\n");
    }

    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate = sampleRate;
    if (processor->setupProcessing(setup) != kResultOk) {
        fprintf(stderr, "namp-rations-standalone: the plug-in rejected the process setup\n");
        return 1;
    }

    // Before setActive, which is where a host puts it: the four banks then start building from the
    // paths the blob names, and each channel sits at its ramped-silence gate until its first entry
    // lands.
    if (useState)
        loadState(component, controller);

    component->setActive(true);
    processor->setProcessing(true);

    ComponentHandler handler(jack);
    controller->setComponentHandler(&handler);

    if (probe && !jack.open("NAMp-rations", processor, component, &route))
        fprintf(stderr, "namp-rations-standalone: continuing without audio\n");

    // --- window and editor -------------------------------------------
    ::Display *display = XOpenDisplay(nullptr);
    if (!display) {
        fprintf(stderr, "namp-rations-standalone: cannot open the X display\n");
        return 1;
    }

    // The view is created before the window, so the window can be opened at the size the editor
    // actually wants rather than at a constant that would have to be kept in step with the panel.
    IPtr<IPlugView> view = owned(controller->createView(Vst::ViewType::kEditor));
    if (view && view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        fprintf(stderr, "namp-rations-standalone: the plug-in has no X11 editor\n");
        view = nullptr;
    }

    int winW = kFallbackW;
    int winH = kFallbackH;
    if (view) {
        ViewRect wanted = {};
        if (view->getSize(&wanted) == kResultTrue && wanted.getWidth() > 0 &&
            wanted.getHeight() > 0) {
            winW = wanted.getWidth();
            winH = wanted.getHeight();
        }
    }

    const int screen = DefaultScreen(display);
    ::Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen), 0, 0, static_cast<unsigned>(winW),
        static_cast<unsigned>(winH), 0, BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, "NAMp Rations");
    // So the desktop entry's StartupWMClass matches and the window gets the right icon.
    XClassHint classHint = {};
    char resName[] = "namp-rations-standalone";
    char resClass[] = "NAMp Rations";
    classHint.res_name = resName;
    classHint.res_class = resClass;
    XSetClassHint(display, window, &classHint);
    XSelectInput(display, window, StructureNotifyMask | SubstructureNotifyMask);

    Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDelete, 1);
    XMapWindow(display, window);
    XFlush(display);

    Rations::RunLoop runLoop(display);
    gRunLoop = &runLoop;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (view) {
        view->setFrame(&runLoop);
        if (view->attached(reinterpret_cast<void *>(static_cast<uintptr_t>(window)),
                           kPlatformTypeX11EmbedWindowID) != kResultTrue) {
            fprintf(stderr, "namp-rations-standalone: the editor refused to attach\n");
            view = nullptr;
        }
    }
    // Only now: the run loop cannot resize a window the view has not attached to, and every page
    // change arrives as exactly that request.
    if (view)
        runLoop.setEmbedding(window, view);

    FeedbackPump feedback(jack, controller);
    runLoop.registerTimer(&feedback, 33);

    BufferSizeWatcher blockWatcher(jack, component, processor, setup);
    if (jack.isOpen())
        runLoop.registerTimer(&blockWatcher, 33);

    runLoop.setXEventCallback([&](const XEvent &event) {
        if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == wmDelete)
            runLoop.stop();
        else if (event.type == ConfigureNotify && event.xconfigure.window == window)
            runLoop.windowConfigured(event.xconfigure.width, event.xconfigure.height);
    });

    runLoop.run();

    // --- teardown ----------------------------------------------------
    if (jack.isOpen())
        runLoop.unregisterTimer(&blockWatcher);
    runLoop.unregisterTimer(&feedback);
    if (view) {
        view->removed();
        view = nullptr;
    }
    jack.close();
    processor->setProcessing(false);
    component->setActive(false);
    // With the audio thread gone and the component inactive, so nothing is moving underneath the
    // blob being written.
    if (useState)
        saveState(component);
    controller->setComponentHandler(nullptr);

    XDestroyWindow(display, window);
    XCloseDisplay(display);
    gRunLoop = nullptr;
    // Retract the host context before it leaves scope, so nothing can reach a dangling pointer
    // during static destruction.
    Vst::PluginContextFactory::instance().setPluginContext(nullptr);
    return 0;
}
