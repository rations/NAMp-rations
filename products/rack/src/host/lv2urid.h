// Lv2UridMap — the LV2 urid:map / urid:unmap pair, mapping URIs to integers.
//
// WHY THIS IS NOT jalv's symap. jalv interns URIs in a symap (a sorted string interner) and guards
// it with a semaphore in its map callback (its mapper.c takes zix_sem_wait around every map and
// every unmap). That is a lock, and urid:map is documented as callable at any time, which includes
// from a plug-in's run() — so jalv's design puts a semaphore on the audio thread. A lock shared
// with non-real-time code is forbidden on that thread outright, and "no plug-in really does it" is
// a hope, not a contract. So this host maps with a fixed-capacity, append-only, lock-free table
// instead: a hit is a couple of relaxed loads and a strcmp, a miss appends by CAS into
// pre-allocated storage, and neither path allocates, locks or blocks. That makes the callback safe
// to hand to a plug-in without knowing what thread it will be called from — which is the only way
// to hand it to a stranger.
//
// APPEND-ONLY IS WHAT MAKES IT SAFE. A URI, once mapped, keeps its id and its storage for the life
// of the process. Nothing is ever moved, rehashed, resized or freed, so a reader can hold a
// `const char*` returned by unmap() without any lifetime question at all, and a reader racing an
// appending writer either sees a fully published entry or does not see it yet. Both are correct
// answers; neither is a torn one.
//
// The map is process-wide on purpose. URIDs are only meaningful relative to the map that issued
// them, and a plug-in that receives an atom from another plug-in's state blob (or from a UI, or
// from a saved preset restored through sratom) must resolve the same integers. One map per process
// is also what lilv and sratom assume when they are handed the same LV2_URID_Map.
//
// CAPACITY IS FINITE AND THAT IS DELIBERATE. Growing would mean allocating, which is exactly what
// the audio thread may not do. The limits below are far above what a rack of pedals uses (the LV2
// specification's own vocabulary is a few hundred URIs, and a plug-in adds a handful), and
// overflowing returns 0 — which urid:map documents as the error value — plus one warning and a
// counter the diagnostics overlay can show. A silent wrong id would be far worse than a refusal.

#pragma once

#include <atomic>
#include <cstdint>

// LV2_URID_Map / LV2_URID_Unmap. This is the one place in the host layer that needs them.
#include <lv2/urid/urid.h>

namespace NAMp::host
{

//------------------------------------------------------------------------
class Lv2UridMap
{
public:
    // Comfortably above what a full rack needs; see the capacity note in the file comment.
    static constexpr uint32_t kMaxUris = 4096;
    static constexpr uint32_t kArenaBytes = 256 * 1024;

    // The single process-wide instance. Constructed on first use on whatever thread asks first,
    // which is always an owning thread: a plug-in cannot call map() before it has been
    // instantiated.
    static Lv2UridMap &instance();

    // Callable from ANY thread, including the audio thread. Returns 0 only if the table or the
    // string arena is exhausted, or `uri` is null or empty.
    uint32_t map(const char *uri) noexcept;

    // Callable from any thread. Returns null for an id this map never issued. The pointer stays
    // valid for the life of the process.
    const char *unmap(uint32_t urid) const noexcept;

    // Feature structs to hand to a plug-in. Their handles point at this object, which outlives
    // every plug-in because it is never destroyed.
    LV2_URID_Map *mapFeature() noexcept
    {
        return &mMapFeature;
    }
    LV2_URID_Unmap *unmapFeature() noexcept
    {
        return &mUnmapFeature;
    }

    // How many URIs are mapped, and how many map() calls had to be refused. A non-zero refusal
    // count means a plug-in is being told the truth about a limit rather than being lied to, and is
    // worth surfacing.
    uint32_t count() const noexcept
    {
        return mCount.load(std::memory_order_acquire);
    }
    uint32_t overflowCount() const noexcept
    {
        return mOverflows.load(std::memory_order_relaxed);
    }

private:
    Lv2UridMap();

    Lv2UridMap(const Lv2UridMap &) = delete;
    Lv2UridMap &operator=(const Lv2UridMap &) = delete;

    static LV2_URID mapCallback(LV2_URID_Map_Handle handle, const char *uri);
    static const char *unmapCallback(LV2_URID_Unmap_Handle handle, LV2_URID urid);

    // Open addressing with linear probing. A slot holds `id` (1-based), or 0 for empty. Power of
    // two so the modulus is a mask; kept well above kMaxUris so probe chains stay short even when
    // full.
    static constexpr uint32_t kSlotCount = 16384;
    static_assert((kSlotCount & (kSlotCount - 1)) == 0, "slot count must be a power of two");
    static_assert(kSlotCount >= 4 * kMaxUris, "keep the table at most a quarter full");

    struct Entry {
        // Points into mArena, and is atomic rather than plain because unmap() reaches an entry
        // WITHOUT going through a slot. Reserving an index and filling the entry are two steps, so
        // between them the index exists and the entry does not; a release store here and an acquire
        // load in unmap() are what make that window return null instead of a dangling pointer.
        std::atomic<const char *> uri{nullptr};
    };

    std::atomic<uint32_t> mSlots[kSlotCount];
    Entry mEntries[kMaxUris];
    char mArena[kArenaBytes];

    // Indices handed out so far. Only ever increases — a failed reservation is never given back,
    // because giving it back would let the counter move down past entries that are already
    // published, and unmap() uses it as a bound. The cost of not undoing is one wasted slot in a
    // table that is by definition already full.
    std::atomic<uint32_t> mCount{0};
    std::atomic<uint32_t> mArenaUsed{0}; // bump allocator offset into mArena
    std::atomic<uint32_t> mOverflows{0};

    LV2_URID_Map mMapFeature{};
    LV2_URID_Unmap mUnmapFeature{};
};

} // namespace NAMp::host
