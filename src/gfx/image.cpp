// ImageCache implementation. See image.h.

#include "image.h"

#include "resourcestore.h"
#include "platform/respath.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace Rations
{

namespace
{

// Cairo reads a PNG through a callback, which is all an in-memory image needs.
struct MemoryPng {
    const unsigned char *data;
    size_t size;
    size_t pos;
};

cairo_status_t readMemoryPng(void *closure, unsigned char *out, unsigned int length)
{
    MemoryPng *src = static_cast<MemoryPng *>(closure);
    if (src->pos + length > src->size)
        return CAIRO_STATUS_READ_ERROR;
    std::memcpy(out, src->data + src->pos, length);
    src->pos += length;
    return CAIRO_STATUS_SUCCESS;
}

} // namespace

//------------------------------------------------------------------------
ImageCache::~ImageCache()
{
    purgeScaled();
    for (auto &entry : mCache)
        if (entry.second)
            cairo_surface_destroy(entry.second);
}

//------------------------------------------------------------------------
cairo_surface_t *ImageCache::get(const char *name)
{
    const std::string key(name);
    auto it = mCache.find(key);
    if (it != mCache.end())
        return it->second; // may be null: a previous failure, already warned

    // A file on disk first, so the bundle's own art — and anything a user has replaced it with —
    // always wins; the built-in copy is the fallback for a binary with no resource directory
    // beside it. mDir is empty in exactly that case, and the read simply fails.
    //
    // Read through readFileBytes and decoded from memory rather than with
    // cairo_image_surface_create_from_png(path): that one fopen()s the narrow string, which is
    // code-page-limited on Windows. See platform/respath.h.
    const std::string rel = "img/" + key + ".png";
    const std::string path = mDir + "/" + rel;
    cairo_surface_t *surface = nullptr;

    std::vector<unsigned char> bytes;
    if (readFileBytes(path, bytes)) {
        MemoryPng src{bytes.data(), bytes.size(), 0};
        surface = cairo_image_surface_create_from_png_stream(&readMemoryPng, &src);
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surface);
            surface = nullptr;
        }
    }

    if (!surface) {
        if (const EmbeddedResource *res = findEmbeddedResource(rel)) {
            MemoryPng src{res->data, res->size, 0};
            surface = cairo_image_surface_create_from_png_stream(&readMemoryPng, &src);
            if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
                fprintf(stderr, "Rations: built-in art %s is unreadable (flat fallback)\n",
                        rel.c_str());
                cairo_surface_destroy(surface);
                surface = nullptr;
            }
        } else {
            fprintf(stderr, "Rations: missing art %s (flat fallback)\n", path.c_str());
        }
    }

    mCache[key] = surface;
    return surface;
}

//------------------------------------------------------------------------
cairo_surface_t *ImageCache::getScaled(const char *name, int w, int h)
{
    if (w <= 0 || h <= 0)
        return nullptr;

    const ScaledKey key{std::string(name), w, h};
    auto it = mScaled.find(key);
    if (it != mScaled.end())
        return it->second;

    cairo_surface_t *source = get(name);
    if (!source) {
        mScaled[key] = nullptr; // the miss is already warned about by get()
        return nullptr;
    }
    const int sw = cairo_image_surface_get_width(source);
    const int sh = cairo_image_surface_get_height(source);
    if (sw <= 0 || sh <= 0) {
        mScaled[key] = nullptr;
        return nullptr;
    }

    cairo_surface_t *scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(scaled) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(scaled);
        mScaled[key] = nullptr;
        return nullptr;
    }

    cairo_t *cr = cairo_create(scaled);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
        cairo_scale(cr, w / static_cast<double>(sw), h / static_cast<double>(sh));
        cairo_set_source_surface(cr, source, 0.0, 0.0);
        // This is the one place the expensive filter runs, so it uses the good
        // one: everything downstream is a 1:1 blit of the result.
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(scaled);

    mScaled[key] = scaled;
    return scaled;
}

//------------------------------------------------------------------------
void ImageCache::purgeScaled()
{
    for (auto &entry : mScaled)
        if (entry.second)
            cairo_surface_destroy(entry.second);
    mScaled.clear();
}

} // namespace Rations
