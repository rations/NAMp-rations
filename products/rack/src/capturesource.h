// CaptureSource — the parsed, reusable form of one .nam capture file.
//
// This is NOT nam::dspData, deliberately. Two reasons:
//
//   * get_dsp(dspData&) MOVES the weights out of the struct, so a cached dspData is consumed by
//     the first model it builds and cannot build a second. A bank has to be rebuildable — a Slim
//     change re-derives every entry — so the cache must survive being used.
//   * A .nam file's top-level "config" JSON for a slimmable container carries every weight as a
//     separate numeric node (tens of thousands of them, one to two megabytes per file once
//     parsed). What is actually needed to rebuild is the shape-only config block plus the raw
//     float vector, which is about 56 KB per file.
//
// So a whole ten-capture bank's source cache is well under a megabyte, and rebuilding the bank
// after a Slim change costs no file I/O and no large parse.
//
// Everything here runs off the audio thread — parsing, allocation and model construction all
// allocate freely.

#pragma once

#include "NAM/dsp.h"
#include "NAM/model_config.h"

#include "json.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
// One size variant of a capture. A plain (non-slimmable) capture has exactly one of these with
// maxValue 1.0; a slimmable container has one per submodel, ascending by maxValue.
struct SubmodelSource {
    nlohmann::json config;      // shape only — no weights
    std::vector<float> weights; // the weights, kept out of the JSON
    nam::ModelMetadata metadata;
    std::string architecture;
    double maxValue = 1.0;
};

//------------------------------------------------------------------------
struct CaptureSource {
    std::string path;     // absolute path, as loaded
    std::string filename; // basename, which is what state stores so a bank survives a move
    std::vector<SubmodelSource> submodels;

    bool valid() const
    {
        return !submodels.empty();
    }
};

//------------------------------------------------------------------------
// Parse one .nam file. Returns false and fills `error` rather than throwing, because a capture
// directory is untrusted input and one bad file must not take the bank down.
//
// Rejects, with a reason:
//   * anything whose expected sample rate is not the native rate — a bank is crossfaded inside a
//     single resampler, so mixed rates cannot be reconciled;
//   * any architecture that is not finite-memory. The whole crossfade argument is that a model's
//     output depends only on a bounded window of past input, so feeding the incoming model for
//     one receptive field makes it exact. That holds for WaveNet and ConvNet. It is FALSE for
//     LSTM, whose cell state has unbounded memory and which therefore cannot be primed by any
//     finite fade, however long.
bool loadCaptureSource(const std::filesystem::path &file, CaptureSource &out, std::string &error);

//------------------------------------------------------------------------
// Build a playable model from a parsed source. `slim` is 0 .. 1 and selects the size variant with
// the same rule the DSP core's own container uses: the first variant whose maxValue exceeds slim,
// else the largest. The model is Reset for the native rate and the fixed chunk size, which also
// prewarms it — so do NOT call prewarm() afterwards, that would do 132 ms of inference twice.
//
// Returns nullptr and fills `error` on failure. Off the audio thread only.
std::unique_ptr<nam::DSP> buildCaptureModel(const CaptureSource &source, double slim,
                                            int maxBufferSize, std::string &error);

//------------------------------------------------------------------------
// Order captures the way the amp's own gain marks are ordered: by the numeric token at the end of
// the filename, with a "MAX" token sorting after every number. Files with no trailing number sort
// last, lexicographically, so a stray file cannot silently displace a numbered one.
//
// This is filename order on purpose. The metadata "gain" field looks like it should be the knob
// position and is not — the trainer defines it as a measured compression statistic ("how much
// gain / compression does the model seem to have"), which is not monotonic in knob position and
// says nothing about spacing. The amp's physical marks are evenly spaced by construction, so
// filename order is both correct and the thing the user actually chose.
bool captureFilenameLess(const std::string &a, const std::string &b);

} // namespace Rations
