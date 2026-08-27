// Bank — an immutable-once-published set of captures, ordered by gain.
//
// Ownership and threading, which is the whole point of this type:
//
//   * The worker thread allocates a Bank, fills in everything that does not require building a
//     model (names, count), and only then publishes the pointer. So every field outside
//     BankEntry::model and BankEntry::ready is written exactly once, before any other thread can
//     see the object, and is safe to read afterwards without synchronisation.
//   * The worker then builds models one at a time, and marks each entry ready with a release
//     store. The audio thread reads `ready` with an acquire load and touches `model` only when it
//     was true. That pair is the entire publication barrier — there is no lock anywhere.
//   * The audio thread never allocates a Bank and never deletes one. When it stops using a bank
//     it hands the pointer back through the retirement queue; the worker owns the only delete.
//
// A Bank is a fixed array rather than a vector so that publishing it involves no indirection the
// audio thread has to chase, and so BankEntry can hold an atomic without fighting vector's move
// requirements.

#pragma once

#include "NAM/dsp.h"
#include "engineconfig.h"

#include <atomic>
#include <memory>
#include <string>

namespace Rations
{

//------------------------------------------------------------------------
struct BankEntry {
    // Built by the worker. Only valid to read once `ready` has been observed true.
    std::unique_ptr<nam::DSP> model;

    // Release-stored by the worker after `model` and the fields below are complete.
    std::atomic<bool> ready{false};

    // How many samples this model must be fed before its output is exact. Read from the model
    // itself, never assumed: the crossfade's correctness is defined in terms of this number, so
    // hard-coding it would be a silent way to break the feature.
    int prewarmSamples = 0;

    // Level metadata, copied out of the model so the audio thread never has to call a virtual
    // accessor mid-mix. These come from the specific size variant that was built, not from the
    // file's top-level metadata — the two differ.
    bool hasLoudness = false;
    double loudnessDb = 0.0;
    bool hasInputLevel = false;
    double inputLevelDbu = 0.0;
    bool hasOutputLevel = false;
    double outputLevelDbu = 0.0;

    // Written before publication, never afterwards.
    std::string name;
};

//------------------------------------------------------------------------
struct Bank {
    static constexpr int kMaxEntries = engine::kMaxBankEntries;

    BankEntry entries[kMaxEntries];

    // Written before publication, never afterwards.
    int count = 0;
    unsigned long long epoch = 0;
    // True when this bank came from a directory of captures rather than a single file. A single
    // capture is a bank of one, which the engine handles without a special case, but the Gain
    // knob has nothing to sweep and the editor says so.
    bool isDirectory = false;

    // Audio thread only. Counts how many crossfade branches currently point into this bank; a
    // bank may be retired only when it is neither current nor referenced. Single mutator, so a
    // plain int is correct and an atomic would be misleading.
    int rtRefs = 0;

    bool entryReady(int index) const
    {
        return index >= 0 && index < count && entries[index].ready.load(std::memory_order_acquire);
    }

    // Highest index that is playable right now. Returns -1 when nothing is ready yet, which the
    // audio thread answers with ramped silence rather than with dry signal.
    int highestReady() const
    {
        int highest = -1;
        for (int i = 0; i < count; ++i)
            if (entries[i].ready.load(std::memory_order_acquire))
                highest = i;
        return highest;
    }
};

} // namespace Rations
