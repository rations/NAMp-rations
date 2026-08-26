// Built-in copies of the art and fonts, for a binary that has no resource directory beside it.
//
// The VST3 bundle carries its art in Contents/Resources and finds it with dladdr (respath.h), and
// that stays the primary source everywhere: a file on disk always wins, so a user can still
// replace a layer without a rebuild. This is the fallback underneath it.
//
// IN THIS PROJECT THE TABLE IS ALWAYS EMPTY, and that is correct rather than an oversight. The
// parent plug-in installed one from a generated translation unit so its single-file standalone
// could run with no bundle beside it; this project ships the VST3 bundle and nothing else, so
// there is always a Contents/Resources for dladdr to find and nothing to fall back to. The
// mechanism is kept because it is what makes embeddedResourceCount() == 0 mean "a missing
// resource directory is a real problem" rather than "this is the expected state", which is the
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

} // namespace Rations
