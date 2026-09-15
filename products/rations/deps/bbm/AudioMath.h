/*
 * AudioMath.h -- copied from the author's own BigBubbleMuff (haiku/source/dsp).
 * Copyright (C) 2026 BigBubbleMuff contributors. Original licence: MIT,
 * declared per-file as SPDX-License-Identifier: MIT and in that subtree's own
 * LICENSE. See NOTICE for the full licence text and for why the per-file grant
 * governs rather than that repository's JUCE-linkage COPYING.
 *
 * Redistributed as part of Rations under the MIT Licence; see LICENSE.
 * Verbatim: everything below this block is byte-identical to the original.
 */

// BigBubbleMuff — small audio utilities for the JUCE-free (Haiku) engine.
// Copyright (C) 2026  BigBubbleMuff contributors. SPDX-License-Identifier: MIT
//
// The Linux build gets these from juce_dsp. This header supplies the same handful
// of facilities with no framework attached, so the circuit model can be compiled
// against nothing but the standard library.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace bbmh {

// Mirrors juce::dsp::ProcessSpec: everything prepare() needs to size its state.
struct ProcessSpec {
  double sampleRate = 44100.0;
  std::uint32_t maximumBlockSize = 512;
  std::uint32_t numChannels = 2;
};

// Convert decibels to a linear gain.
inline float decibelsToGain(float dB) {
  return std::pow(10.0f, dB * 0.05f);
}

// Replace non-finite samples with silence so NaN/Inf never escapes the engine.
inline float sanitise(float x) {
  return std::isfinite(x) ? x : 0.0f;
}

// Tolerant float comparison, matching what juce::SmoothedValue uses to decide a
// new target is really new: equal within one epsilon, relative to magnitude.
inline bool approximatelyEqual(float a, float b) {
  if (!(std::isfinite(a) && std::isfinite(b)))
    return std::isnan(a) == std::isnan(b) && std::signbit(a) == std::signbit(b) &&
           std::isinf(a) == std::isinf(b);
  const float diff = std::abs(a - b);
  return diff <= std::numeric_limits<float>::min() ||
         diff <=
             std::numeric_limits<float>::epsilon() * std::max(std::abs(a), std::abs(b));
}

// Linear parameter ramp, matching juce::SmoothedValue<float> (whose default
// smoothing type is Linear): reset() fixes the ramp length in samples, a new
// target restarts the ramp from wherever the value currently is, and the value
// lands exactly on the target on the final step.
class LinearSmoother {
public:
  LinearSmoother() = default;
  explicit LinearSmoother(float initial) : current_(initial), target_(initial) {}

  // rampSeconds is measured at the rate the smoother is advanced at, so reset it
  // with the rate whose getNextValue() will be called (base or oversampled).
  void reset(double sampleRate, double rampSeconds) {
    stepsToTarget_ = static_cast<int>(std::floor(rampSeconds * sampleRate));
    setCurrentAndTargetValue(target_);
  }

  void setCurrentAndTargetValue(float value) {
    current_ = target_ = value;
    countdown_ = 0;
  }

  void setTargetValue(float value) {
    if (approximatelyEqual(value, target_))
      return;
    if (stepsToTarget_ <= 0) {
      setCurrentAndTargetValue(value);
      return;
    }
    target_ = value;
    countdown_ = stepsToTarget_;
    step_ = (target_ - current_) / static_cast<float>(countdown_);
  }

  float getTargetValue() const { return target_; }
  float getCurrentValue() const { return current_; }
  bool isSmoothing() const { return countdown_ > 0; }

  inline float getNextValue() noexcept {
    if (countdown_ <= 0)
      return target_;
    --countdown_;
    current_ = (countdown_ > 0) ? current_ + step_ : target_;
    return current_;
  }

private:
  float current_ = 0.0f;
  float target_ = 0.0f;
  float step_ = 0.0f;
  int countdown_ = 0;
  int stepsToTarget_ = 0;
};

} // namespace bbmh
