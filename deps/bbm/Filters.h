/*
 * Filters.h -- copied from the author's own BigBubbleMuff (haiku/source/dsp).
 * Copyright (C) 2026 BigBubbleMuff contributors. Original licence: MIT,
 * declared per-file as SPDX-License-Identifier: MIT and in that subtree's own
 * LICENSE. See NOTICE for the full licence text and for why the per-file grant
 * governs rather than that repository's JUCE-linkage COPYING.
 *
 * Redistributed as part of Rations under the MIT Licence; see LICENSE.
 * Verbatim: everything below this block is byte-identical to the original.
 */

// BigBubbleMuff — small TPT one-pole filters used between WDF stages.
// Copyright (C) 2026  BigBubbleMuff contributors. SPDX-License-Identifier: MIT
//
// Topology-preserving-transform (TPT) one-pole sections: numerically robust,
// per-sample, no allocation. Used for the AC-coupling high-passes and the
// high-frequency rolloffs that surround the nonlinear clipping cores.
#pragma once

#include <algorithm> // std::clamp (reached transitively in the Linux tree)
#include <cmath>

namespace bbm {

// Shared TPT one-pole core. g = tan(pi*fc/fs); lowpass output = the integrator
// state, highpass output = input - lowpass.
class OnePole {
public:
  void prepare(double sampleRate, float cutoffHz) {
    setCutoff(sampleRate, cutoffHz);
    reset();
  }

  void setCutoff(double sampleRate, float cutoffHz) {
    const float fs = static_cast<float>(sampleRate);
    const float fc = std::clamp(cutoffHz, 1.0f, 0.45f * fs);
    const float g = std::tan(3.14159265358979323846f * fc / fs);
    g_ = g / (1.0f + g);
  }

  void reset() { z_ = 0.0f; }

  // Returns {lowpass, highpass} for one input sample.
  struct Out {
    float lp;
    float hp;
  };
  Out process(float x) {
    const float v = (x - z_) * g_;
    const float lp = v + z_;
    z_ = lp + v;
    return {lp, x - lp};
  }

  float processLowpass(float x) { return process(x).lp; }
  float processHighpass(float x) { return process(x).hp; }

private:
  float g_ = 0.0f;
  float z_ = 0.0f;
};

} // namespace bbm
