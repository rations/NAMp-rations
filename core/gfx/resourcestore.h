// Built-in copies of the art and fonts, for a binary that has no resource directory beside it.
//
// The VST3 bundle carries its art in Contents/Resources and finds it with dladdr (respath.h), and
// that stays the primary source everywhere: a file on disk always wins, so a user can still
// replace a layer without a rebuild. This is the fallback underneath it: what a binary draws with
// when it LINKS the amp in rather than loading a bundle, and so has no Contents/Resources for
// dladdr to find.
//
// THE TWO PRODUCTS USE THIS DIFFERENTLY, and neither one's story is the general rule.
//
//   products/rack   namp-rack links the amp in and ships as a single executable, so it links the
//                   generated table too. It is built by cmake/embedresources.cmake from the same
//                   files the bundle copies. The PLUG-IN must not link it: it already carries
//                   those files in Contents/Resources, where a user can replace them, and linking
//                   both would put two copies of every layer in one bundle.
//
//   products/rations  nothing links a table, so there is never anything to fall back to. Its
//                   standalone LOADS the built bundle rather than linking the amp, so it resolves
//                   art through the same Contents/Resources the plug-in does and needs no
//                   built-ins. installBuiltinResources() below is declared and never defined
//                   there, which is exactly right: nothing calls it, so nothing needs it, and a
//                   target that asked for it without linking a definition would fail at link time
//                   rather than draw flat rectangles.
//
// Either way an empty table is a normal state for a binary that resolves art from a bundle and an
// impossible one for a binary that does not, which is what makes embeddedResourceCount() == 0 mean
// "a missing resource directory is a real problem" rather than "this is the expected state" — the
// distinction respath.cpp's warning turns on.
//
// THREADING AND OWNERSHIP. installEmbeddedResources() writes two file-scope pointers and is
// expected to be called once, from main(), before any window or editor exists; every later access
// is a read. It is not synchronised and must not be called once drawing has started. The table and
// the bytes it points at are not owned here and must have static storage duration — which is what
// the generated file provides, and what lets FreeType keep a face pointing straight into it.

#pragma once

#include <cstddef>
#include <string>

namespace Rations
{

//------------------------------------------------------------------------
// One embedded file: its path relative to the resource directory ("img/base.png", exactly the
// string the caches build), and its bytes.
struct EmbeddedResource {
    const char *path;
    const unsigned char *data;
    size_t size;
};

//------------------------------------------------------------------------
// Publish a table. Not owned; must outlive every user of it (static storage).
void installEmbeddedResources(const EmbeddedResource *table, size_t count);

//------------------------------------------------------------------------
// Look one up by its relative path. Null when nothing is installed under it, which is the normal
// state inside the plug-in bundle.
const EmbeddedResource *findEmbeddedResource(const std::string &path);

//------------------------------------------------------------------------
// How many resources are installed. Zero means this binary has no built-in set, which is what
// makes a missing resource directory an actual problem rather than the expected state.
size_t embeddedResourceCount();

//------------------------------------------------------------------------
// Defined by the generated file, declared here so a binary can ask for the built-in set without
// knowing anything about how it was generated. Link the generated library to get it; the link
// fails cleanly if a target asks for the built-ins without linking them.
void installBuiltinResources();

} // namespace Rations
