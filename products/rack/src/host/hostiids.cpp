// Interface IDs for the host side, defined exactly once per final binary.
//
// A VST3 interface's `iid` is a definition, not a declaration, so something has to emit it. The SDK
// solves this for its own interfaces with usediids.cpp, which every host binary compiles in; this
// file is the same idea for the one interface this host declares knowledge of but does not own.
//
// It is deliberately NOT part of the host library. A plug-in that IMPLEMENTS this interface emits
// the same symbol from its own edit controller, so a build that ever linked the host library into
// such a plug-in would get a duplicate definition the moment the two met. Keeping it a loose
// source file that each host EXECUTABLE lists costs one line per executable and keeps that build
// possible, and it mirrors how the SDK ships usediids.cpp.

#include "inampfileloader.h"

DEF_CLASS_IID(NAMp::INampFileLoader)
