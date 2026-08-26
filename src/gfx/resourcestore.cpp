// Embedded-resource lookup. See resourcestore.h.

#include "resourcestore.h"

#include <cstring>

namespace Rations
{

namespace
{

// Write-once from main() before any drawing; read-only afterwards. See the threading note in the
// header — this is deliberately unsynchronised.
const EmbeddedResource *gTable = nullptr;
size_t gCount = 0;

} // namespace

//------------------------------------------------------------------------
void installEmbeddedResources(const EmbeddedResource *table, size_t count)
{
    gTable = table;
    gCount = table ? count : 0;
}

//------------------------------------------------------------------------
size_t embeddedResourceCount()
{
    return gCount;
}

//------------------------------------------------------------------------
const EmbeddedResource *findEmbeddedResource(const std::string &path)
{
    // Linear: the table is the contents of one resources directory, and every lookup is a cache
    // miss that is about to decode a PNG.
    for (size_t i = 0; i < gCount; ++i)
        if (gTable[i].path && path == gTable[i].path)
            return &gTable[i];
    return nullptr;
}

} // namespace Rations
