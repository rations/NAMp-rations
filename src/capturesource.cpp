// CaptureSource implementation. See capturesource.h for why this exists instead of nam::dspData.

#include "capturesource.h"
#include "engineconfig.h"
#include "nativeresampler.h"

#include "NAM/get_dsp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <limits>

namespace Rations
{

namespace
{

// Architectures whose output depends only on a bounded window of past input. Only these can be
// primed by a finite fade, which is the entire basis of the crossfade being exact rather than
// merely smooth. LSTM is deliberately absent.
bool isFiniteMemoryArchitecture(const std::string &architecture)
{
    return architecture == "WaveNet" || architecture == "ConvNet" || architecture == "Linear";
}

// Read one metadata number if present and non-null.
bool readMetaNumber(const nlohmann::json &metadata, const char *key, double &out)
{
    if (!metadata.is_object())
        return false;
    auto it = metadata.find(key);
    if (it == metadata.end() || it->is_null() || !it->is_number())
        return false;
    out = it->get<double>();
    return true;
}

// Pull one submodel-shaped JSON object ({version, architecture, config, weights, sample_rate,
// metadata}) into a SubmodelSource.
bool readSubmodel(const nlohmann::json &j, double maxValue, SubmodelSource &out, std::string &error)
{
    if (!j.is_object()) {
        error = "submodel is not a JSON object";
        return false;
    }
    auto arch = j.find("architecture");
    if (arch == j.end() || !arch->is_string()) {
        error = "submodel has no architecture";
        return false;
    }
    out.architecture = arch->get<std::string>();
    if (!isFiniteMemoryArchitecture(out.architecture)) {
        // This is the one rejection that is about the feature rather than about file integrity,
        // so it says why.
        error = "architecture '" + out.architecture +
                "' is not finite-memory, so it cannot be crossfaded (its state has unbounded "
                "history and no fade length makes it exact)";
        return false;
    }

    auto config = j.find("config");
    if (config == j.end() || !config->is_object()) {
        error = "submodel has no config block";
        return false;
    }
    out.config = *config;

    auto weights = j.find("weights");
    if (weights == j.end() || !weights->is_array() || weights->empty()) {
        error = "submodel has no weights";
        return false;
    }
    out.weights = weights->get<std::vector<float>>();

    out.metadata = nam::ModelMetadata();
    auto version = j.find("version");
    if (version != j.end() && version->is_string())
        out.metadata.version = version->get<std::string>();

    out.metadata.sample_rate = nam::get_sample_rate_from_nam_file(j);
    if (out.metadata.sample_rate != kNativeSampleRate) {
        error = "submodel expects " + std::to_string(out.metadata.sample_rate) +
                " Hz; a bank is crossfaded inside one resampler and must be all-native";
        return false;
    }

    // The SUBMODEL's own loudness, not the file's. They differ, and using the file's would apply
    // the wrong compensation to whichever size variant is actually playing.
    const nlohmann::json meta = j.value("metadata", nlohmann::json::object());
    double v = 0.0;
    if (readMetaNumber(meta, "loudness", v))
        out.metadata.loudness = v;
    if (readMetaNumber(meta, "input_level_dbu", v))
        out.metadata.input_level = v;
    if (readMetaNumber(meta, "output_level_dbu", v))
        out.metadata.output_level = v;

    out.maxValue = maxValue;
    return true;
}

// The sort key described in captureFilenameLess: rank, then value.
struct GainKey {
    int rank = 2; // 0 = numbered, 1 = MAX, 2 = neither
    double num = 0.0;
    std::string lower;
};

GainKey gainKeyOf(const std::string &filename)
{
    GainKey key;
    key.lower = filename;
    std::transform(key.lower.begin(), key.lower.end(), key.lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Work on the stem, so a ".nam" extension cannot contribute digits.
    std::string stem = key.lower;
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos)
        stem.resize(dot);

    // Trailing "max" token wins over any earlier digits in the name.
    size_t end = stem.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(stem[end - 1])))
        --end;
    if (end >= 3 && stem.compare(end - 3, 3, "max") == 0) {
        const bool boundary = end == 3 || !std::isalnum(static_cast<unsigned char>(stem[end - 4]));
        if (boundary) {
            key.rank = 1;
            return key;
        }
    }

    // Otherwise the last run of digits in the stem.
    size_t last = std::string::npos;
    for (size_t i = stem.size(); i > 0; --i) {
        if (std::isdigit(static_cast<unsigned char>(stem[i - 1]))) {
            last = i - 1;
            break;
        }
    }
    if (last == std::string::npos)
        return key; // rank 2: no number at all
    size_t first = last;
    while (first > 0 && std::isdigit(static_cast<unsigned char>(stem[first - 1])))
        --first;
    key.rank = 0;
    key.num = std::strtod(stem.substr(first, last - first + 1).c_str(), nullptr);
    return key;
}

} // namespace

//------------------------------------------------------------------------
bool captureFilenameLess(const std::string &a, const std::string &b)
{
    const GainKey ka = gainKeyOf(a);
    const GainKey kb = gainKeyOf(b);
    if (ka.rank != kb.rank)
        return ka.rank < kb.rank;
    if (ka.rank == 0 && ka.num != kb.num)
        return ka.num < kb.num;
    return ka.lower < kb.lower;
}

//------------------------------------------------------------------------
bool loadCaptureSource(const std::filesystem::path &file, CaptureSource &out, std::string &error)
{
    out = CaptureSource();
    try {
        std::ifstream in(file);
        if (!in) {
            error = "cannot open";
            return false;
        }
        nlohmann::json j;
        in >> j;

        auto arch = j.find("architecture");
        if (arch == j.end() || !arch->is_string()) {
            error = "no architecture";
            return false;
        }
        const std::string architecture = arch->get<std::string>();

        if (architecture == "SlimmableContainer") {
            auto config = j.find("config");
            if (config == j.end() || !config->is_object()) {
                error = "container has no config";
                return false;
            }
            auto subs = config->find("submodels");
            if (subs == config->end() || !subs->is_array() || subs->empty()) {
                error = "container has no submodels";
                return false;
            }
            for (const auto &entry : *subs) {
                auto modelIt = entry.find("model");
                auto maxIt = entry.find("max_value");
                if (modelIt == entry.end() || maxIt == entry.end() || !maxIt->is_number()) {
                    error = "malformed submodel entry";
                    return false;
                }
                SubmodelSource sub;
                if (!readSubmodel(*modelIt, maxIt->get<double>(), sub, error))
                    return false;
                out.submodels.push_back(std::move(sub));
            }
            // The core's size-selection rule assumes ascending thresholds.
            std::sort(out.submodels.begin(), out.submodels.end(),
                      [](const SubmodelSource &l, const SubmodelSource &r) {
                          return l.maxValue < r.maxValue;
                      });
        } else {
            // A plain capture: one variant, always selected.
            SubmodelSource sub;
            if (!readSubmodel(j, 1.0, sub, error))
                return false;
            out.submodels.push_back(std::move(sub));
        }

        out.path = file.string();
        out.filename = file.filename().string();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

//------------------------------------------------------------------------
std::unique_ptr<nam::DSP> buildCaptureModel(const CaptureSource &source, double slim,
                                            int maxBufferSize, std::string &error)
{
    if (!source.valid()) {
        error = "empty capture source";
        return nullptr;
    }
    // Same rule as the DSP core's container: the first variant whose threshold exceeds the
    // requested size, otherwise the largest.
    size_t index = source.submodels.size() - 1;
    for (size_t i = 0; i < source.submodels.size(); ++i) {
        if (slim < source.submodels[i].maxValue) {
            index = i;
            break;
        }
    }
    const SubmodelSource &sub = source.submodels[index];

    try {
        // Weights are copied, not moved: the source has to stay rebuildable for the next Slim
        // change. A copy of ~12k floats is 48 KB and happens off the audio thread.
        auto config =
            nam::parse_model_config_json(sub.architecture, sub.config, sub.metadata.sample_rate);
        if (!config) {
            error = "no config parser for architecture " + sub.architecture;
            return nullptr;
        }
        auto model = nam::create_dsp(std::move(config), sub.weights, sub.metadata);
        if (!model) {
            error = "model construction returned nothing";
            return nullptr;
        }
        if (model->NumInputChannels() != 1 || model->NumOutputChannels() != 1) {
            error = "model is not mono in / mono out";
            return nullptr;
        }
        // Reset prewarms by default, so this both sizes the model and runs it forward through one
        // receptive field. Calling prewarm() as well would pay that cost twice.
        model->Reset(kNativeSampleRate, maxBufferSize);
        return model;
    } catch (const std::exception &e) {
        error = e.what();
        return nullptr;
    }
}

} // namespace Rations
