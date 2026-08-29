// Loading capture banks into the built bundle, for the offline tools that drive it.
//
// This header exists because the plug-in stopped shipping captures. It used to resolve four banks
// out of its own Contents/Resources/captures, so a tool that loaded the bundle got them for free
// and had nothing to hand over; now every bank is a folder the user picks, and a headless host has
// to pick them too. Four tools need that and it is one wire format, so it is written once.
//
// The route is deliberately the SAME one the editor uses — an IConnectionPoint message straight at
// the component, path as a UTF-8 binary attribute — rather than a back door added for testing. A
// proof that drives the plug-in differently from the way a user does is proving something else.

#pragma once

#include "public.sdk/source/vst/hosting/hostclasses.h"

#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include "engineconfig.h"
#include "rationsids.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace RationsTools
{

// Where the four banks live, as a directory holding one subdirectory per channel named the way
// kChannelDefaultName names them. Falls back to $RATIONS_TEST_CAPTURES so the gate scripts can set
// it once instead of every tool's command line carrying it.
inline std::string captureRoot(const std::string &fromArgs)
{
    if (!fromArgs.empty())
        return fromArgs;
    if (const char *env = std::getenv("RATIONS_TEST_CAPTURES"))
        return env;
    return std::string();
}

// What to print when there is no root. A missing argument, not a broken build: the tools used to
// say "this bundle has no Contents/Resources/captures", and that sentence stopped being true.
inline void printCaptureUsage(const char *tool)
{
    std::fprintf(stderr,
                 "%s: no capture banks given.\n"
                 "  Pass --captures <dir>, or set $RATIONS_TEST_CAPTURES. The directory holds one\n"
                 "  subdirectory per channel: %s, %s, %s, %s.\n"
                 "  The plug-in ships no captures — every bank is a folder the user loads — so a\n"
                 "  proof that needs one has to be told where it is.\n",
                 tool, Rations::kChannelDefaultName[0], Rations::kChannelDefaultName[1],
                 Rations::kChannelDefaultName[2], Rations::kChannelDefaultName[3]);
}

// Load one channel from a directory of captures, exactly as the settings page does.
inline bool sendCaptureLoad(Steinberg::Vst::HostApplication &host,
                            Steinberg::Vst::IComponent *component, int channel,
                            const std::string &path, bool isDirectory)
{
    using namespace Steinberg;
    if (channel < 0 || channel >= Rations::kChannelCount)
        return false;
    FUnknownPtr<Vst::IConnectionPoint> cp(component);
    if (!cp)
        return false;
    // createInstance takes TUID by value, which is a non-const char[16], so the interface id has to
    // be copied out of the FUID rather than passed straight through.
    TUID iid;
    std::memcpy(iid, Vst::IMessage::iid, sizeof(TUID));
    Vst::IMessage *raw = nullptr;
    if (host.createInstance(iid, iid, reinterpret_cast<void **>(&raw)) != kResultOk || !raw)
        return false;
    IPtr<Vst::IMessage> msg = owned(raw);
    msg->setMessageID(Rations::kMsgLoadCapture[channel]);
    msg->getAttributes()->setBinary(Rations::kMsgPathAttr, path.c_str(),
                                    static_cast<uint32>(path.size()));
    msg->getAttributes()->setInt(Rations::kMsgIsDirAttr, isDirectory ? 1 : 0);
    return cp->notify(msg) == kResultOk;
}

// All four channels from one root, each from its own subdirectory. A channel whose directory is
// missing is not an error here: ModelBank warns and that channel outputs ramped silence, which is
// the behaviour a tool asserting on a partial bank set wants to see rather than be spared.
inline bool loadCaptureRoot(Steinberg::Vst::HostApplication &host,
                            Steinberg::Vst::IComponent *component, const std::string &root)
{
    if (root.empty())
        return false;
    bool ok = true;
    for (int c = 0; c < Rations::kChannelCount; ++c) {
        const std::string dir = root + "/" + Rations::kChannelDefaultName[c];
        ok = sendCaptureLoad(host, component, c, dir, /*isDirectory=*/true) && ok;
    }
    return ok;
}

} // namespace RationsTools
