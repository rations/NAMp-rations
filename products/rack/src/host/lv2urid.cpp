// Lv2UridMap implementation. See lv2urid.h for why this is lock-free rather than jalv's
// semaphore-guarded symap.

#include "lv2urid.h"

#include <cstdio>
#include <cstring>

namespace NAMp::host
{

namespace
{

// FNV-1a. Chosen because it is a handful of instructions with no table and no allocation, which is
// what a hash called from the audio thread has to be. Collisions cost one extra probe and a strcmp,
// not correctness.
inline uint32_t hashUri(const char *uri) noexcept
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(uri); *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

} // namespace

//------------------------------------------------------------------------
// Never destroyed. A plug-in may hold the LV2_URID_Map we handed it for as long as it lives, and
// static destruction order across a process containing arbitrary third-party shared objects is not
// something this map can win — so it does not enter the race. The storage is fixed-size and owned
// by the process image; leaking it at exit costs nothing that matters.
Lv2UridMap &Lv2UridMap::instance()
{
    static Lv2UridMap *const sMap = new Lv2UridMap();
    return *sMap;
}

//------------------------------------------------------------------------
Lv2UridMap::Lv2UridMap()
{
    for (uint32_t i = 0; i < kSlotCount; ++i)
        mSlots[i].store(0, std::memory_order_relaxed);

    mMapFeature.handle = this;
    mMapFeature.map = &Lv2UridMap::mapCallback;
    mUnmapFeature.handle = this;
    mUnmapFeature.unmap = &Lv2UridMap::unmapCallback;
}

//------------------------------------------------------------------------
LV2_URID Lv2UridMap::mapCallback(LV2_URID_Map_Handle handle, const char *uri)
{
    return static_cast<Lv2UridMap *>(handle)->map(uri);
}

//------------------------------------------------------------------------
const char *Lv2UridMap::unmapCallback(LV2_URID_Unmap_Handle handle, LV2_URID urid)
{
    return static_cast<const Lv2UridMap *>(handle)->unmap(urid);
}

//------------------------------------------------------------------------
uint32_t Lv2UridMap::map(const char *uri) noexcept
{
    if (!uri || !uri[0])
        return 0;

    // A URI arrives from a plug-in binary or a state blob, both untrusted. The cap bounds the
    // strlen and the memcpy, and keeps the arena arithmetic below from being able to wrap: no real
    // URI is anywhere near this long, so refusing one is refusing something malformed.
    constexpr size_t kMaxUriLength = 1024;
    const size_t len = strnlen(uri, kMaxUriLength + 1);
    if (len > kMaxUriLength)
        return 0;

    const uint32_t hash = hashUri(uri);

    // Bounded probe. If the table were ever this congested the capacity check below would already
    // have refused, but the bound is stated rather than implied: an unbounded loop on the audio
    // thread is a hang, and a hang is worse than a refusal.
    for (uint32_t probe = 0; probe < kSlotCount; ++probe) {
        const uint32_t slot = (hash + probe) & (kSlotCount - 1);
        const uint32_t id = mSlots[slot].load(std::memory_order_acquire);

        if (id != 0) {
            // Occupied. The acquire above pairs with the release in the CAS below, so the entry's
            // string pointer and its bytes are both visible here.
            const char *const stored = mEntries[id - 1].uri.load(std::memory_order_acquire);
            if (stored && std::strcmp(stored, uri) == 0)
                return id;
            continue; // a genuine collision; keep probing
        }

        // Empty slot, so this URI is not mapped yet. Reserve storage BEFORE publishing, because
        // publishing is what makes the entry visible to every other thread. Neither reservation is
        // ever undone — see the note on mCount.
        const uint32_t index = mCount.fetch_add(1, std::memory_order_relaxed);
        if (index >= kMaxUris)
            break;

        const uint32_t size = static_cast<uint32_t>(len + 1);
        const uint32_t offset = mArenaUsed.fetch_add(size, std::memory_order_relaxed);
        if (offset > kArenaBytes - size)
            break;

        char *const stored = mArena + offset;
        std::memcpy(stored, uri, size);
        // Release: the bytes above must be visible to anyone who can see this pointer, including an
        // unmap() that reaches the entry directly rather than through the slot.
        mEntries[index].uri.store(stored, std::memory_order_release);

        uint32_t expected = 0;
        if (mSlots[slot].compare_exchange_strong(expected, index + 1, std::memory_order_release,
                                                 std::memory_order_acquire)) {
            return index + 1;
        }

        // Lost the slot to another thread. Its entry and arena bytes are now dead — bounded waste,
        // and the price of not holding a lock. Re-examine this same slot: the winner may have
        // mapped the URI we wanted, or a different one, and only a comparison can tell.
        const char *const winner = mEntries[expected - 1].uri.load(std::memory_order_acquire);
        if (winner && std::strcmp(winner, uri) == 0)
            return expected;
    }

    // One warning, then silence: a plug-in that maps in a loop must not be able to turn a capacity
    // problem into a log flood on the audio thread. The counter keeps the full story.
    if (mOverflows.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::fprintf(
            stderr,
            "namp-rack: the LV2 URID map is full (%u URIs, %u bytes of names). Further URIs "
            "will be refused, which some plug-ins handle badly.\n",
            kMaxUris, kArenaBytes);
    }
    return 0;
}

//------------------------------------------------------------------------
const char *Lv2UridMap::unmap(uint32_t urid) const noexcept
{
    // mCount counts reservations, which run ahead of publications, so it bounds the array but does
    // not prove the entry exists yet. The acquire load of the entry itself is what proves that, and
    // returns null for the brief window in which an id has been reserved and not yet filled.
    if (urid == 0 || urid > kMaxUris || urid > mCount.load(std::memory_order_relaxed))
        return nullptr;
    return mEntries[urid - 1].uri.load(std::memory_order_acquire);
}

} // namespace NAMp::host
