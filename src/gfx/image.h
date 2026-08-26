// ImageCache — the panel's raster art layers, loaded from the bundle as PNG.
//
// Cairo reads PNG natively (cairo_image_surface_create_from_png), so nothing
// links libpng directly. gui/make_assets.sh stores most layers above their
// on-screen size and they are drawn scaled down, which is why the Canvas image
// calls pick a good-quality filter.
//
// Every load is checked: a missing or corrupt file yields a null surface, one
// warning, and a cached null so the warning does not repeat every frame. Null
// surfaces are silently skipped by the Canvas image calls, so the panel falls
// back to its flat-colour drawing instead of failing.
//
// PRE-SCALING. The editor is host-resizable, so the art is almost never drawn
// at its stored size, and re-running Cairo's filter over every layer of every
// frame is the most expensive thing the editor could do. Measured on base.png
// (1133x403), against a 33 ms tick:
//
//   scale   rescaled each frame   pre-scaled, blitted 1:1
//   0.66          3.731 ms               0.028 ms      (132x)
//   1.00          0.381 ms               0.060 ms        (6x)
//   1.50          1.882 ms               0.136 ms       (14x)
//
// Downscaling is the expensive direction, and 3.7 ms is 11% of a tick for one
// layer. getScaled() therefore caches a layer at an exact pixel size, keyed on
// it; the panel rebuilds those entries only when the window size changes.
// purgeScaled() drops the old set, so dragging a window edge does not
// accumulate one surface per pixel of travel.

#pragma once

#include <cairo/cairo.h>

#include <map>
#include <string>
#include <tuple>

namespace Rations
{

//------------------------------------------------------------------------
class ImageCache
{
public:
    ImageCache() = default;
    ~ImageCache();

    ImageCache(const ImageCache &) = delete;
    ImageCache &operator=(const ImageCache &) = delete;

    // Layers are loaded from <resourceDir>/img/<name>.png.
    void setResourceDir(const std::string &resourceDir)
    {
        mDir = resourceDir;
    }

    // Returns a surface owned by the cache — never destroy it — or null.
    cairo_surface_t *get(const char *name);

    // The same layer resampled to exactly w x h pixels, cached and owned here.
    // Null if the layer is missing or the size is not positive.
    cairo_surface_t *getScaled(const char *name, int w, int h);

    // Drop every pre-scaled entry, keeping the originals. Call on a resize.
    void purgeScaled();

private:
    struct ScaledKey {
        std::string name;
        int w, h;
        bool operator<(const ScaledKey &o) const
        {
            return std::tie(name, w, h) < std::tie(o.name, o.w, o.h);
        }
    };

    std::string mDir;
    std::map<std::string, cairo_surface_t *> mCache; // null entry = load failed
    std::map<ScaledKey, cairo_surface_t *> mScaled;
};

} // namespace Rations
