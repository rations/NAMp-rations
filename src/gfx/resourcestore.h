// Built-in copies of the art and fonts, for a binary that has no resource directory beside it.
//
// The VST3 bundle carries its art in Contents/Resources and finds it with dladdr (respath.h), and
// that stays the primary source everywhere: a file on disk always wins, so a user can still
// replace a layer without a rebuild. This is the fallback underneath it, and it is what lets the
// standalone ship as a single executable — it links the amp in rather than loading a bundle, so
// there is no Contents/Resources for dladdr to find.
//
// The table itself is generated at build time by cmake/embedresources.cmake from the same files
// the bundle copies. ONLY THE STANDALONE LINKS IT. The plug-in must not: it already carries the
// same files in Contents/Resources, where a user can replace them, and linking both would put two
// copies of every layer in one bundle.
//
// So an empty table is a normal state in the bundle and an impossible one in the standalone, which
// is what makes embeddedResourceCount() == 0 mean "a missing resource directory is a real problem"
// rather than "this is the expected state" — the distinction respath.cpp's warning turns on.
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
