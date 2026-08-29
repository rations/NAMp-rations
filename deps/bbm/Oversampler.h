/*
 * Oversampler.h -- copied from the author's own BigBubbleMuff (haiku/source/dsp).
 * Copyright (C) 2026 BigBubbleMuff contributors. Original licence: MIT,
 * declared per-file as SPDX-License-Identifier: MIT and in that subtree's own
 * LICENSE. See NOTICE for the full licence text and for why the per-file grant
 * governs rather than that repository's JUCE-linkage COPYING.
 *
 * Redistributed as part of Rations under the MIT Licence; see LICENSE.
 * Verbatim: everything below this block is byte-identical to the original.
 */

// BigBubbleMuff — 4x half-band polyphase-IIR oversampler (mono, JUCE-free).
// Copyright (C) 2026  BigBubbleMuff contributors. SPDX-License-Identifier: MIT
//
// Two cascaded 2x half-band stages wrapped around the nonlinear clipping core, the
// same arrangement (and the same four filter specifications) the Linux build uses.
//
// Each 2x stage is the classic two-path polyphase half-band:
//
//     H(z) = 1/2 * [ A0(z^2) + z^-1 * A1(z^2) ],  A_p(w) = prod_i (a_i + w^-1)
//                                                            / (1 + a_i * w^-1)
//
// Because both paths contain only even lags, every allpass section runs in the
// DECIMATED domain, where z^-2 is a single-sample delay -- so a section costs one
// multiply-add per low-rate sample regardless of direction:
//
//     y[n] = a * (x[n] - y[n-1]) + x[n-1]
//
// Interpolating, the two paths simply produce the two output phases (the z^-1 is
// absorbed by the interleave, and the missing 1/2 cancels the 6 dB zero-stuffing
// loss). Decimating, the even input samples feed A0, the odd ones feed A1 delayed
// by one low-rate sample, and the two are averaged.
//
// The coefficients below were produced by tools/design_halfband.cpp, which solves
//     minimise  max |H(e^jw)|  over the stopband
// directly and then verifies the result. Every filter beats the specification it
// was designed to; the measured figures are recorded beside each table.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace bbmh {

namespace halfband {

// Stage 0 (base rate <-> 2x), up: transition width 0.050, target -90 dB.
//   7 sections (overall order 15)
//   measured stopband -93.35 dB, passband ripple -1.93e+02 dB, DC delay 3.300523
inline constexpr std::array<float, 4> kStage0UpDirect{
    0.045728174864450147f,
    0.33250126199928798f,
    0.66320218962887367f,
    0.93385584985571923f,
};
inline constexpr std::array<float, 3> kStage0UpDelayed{
    0.16808763527759013f,
    0.50448592941478587f,
    0.80378098804857878f,
};
inline constexpr double kStage0UpDelay = 3.3005227243414921;

// Stage 0 (base rate <-> 2x), down: transition width 0.060, target -75 dB.
//   6 sections (overall order 13)
//   measured stopband -85.27 dB, passband ripple -1.77e+02 dB, DC delay 2.974119
inline constexpr std::array<float, 3> kStage0DownDirect{
    0.054217512203022476f,
    0.38308727230810996f,
    0.74872091136704522f,
};
inline constexpr std::array<float, 3> kStage0DownDelayed{
    0.19679792986717792f,
    0.57313636091909148f,
    0.91429369776758063f,
};
inline constexpr double kStage0DownDelay = 2.9741185509854535;

// Stage 1 (2x <-> 4x), up: transition width 0.100, target -80 dB.
//   5 sections (overall order 11)
//   measured stopband -86.95 dB, passband ripple -1.80e+02 dB, DC delay 2.801319
inline constexpr std::array<float, 3> kStage1UpDirect{
    0.054230770931398845f,
    0.39879693480469325f,
    0.86291779872546559f,
};
inline constexpr std::array<float, 2> kStage1UpDelayed{
    0.19969955045484661f,
    0.62109681249914672f,
};
inline constexpr double kStage1UpDelay = 2.8013194422055090;

// Stage 1 (2x <-> 4x), down: transition width 0.120, target -65 dB.
//   4 sections (overall order 9)
//   measured stopband -76.07 dB, passband ripple -1.58e+02 dB, DC delay 2.380087
inline constexpr std::array<float, 2> kStage1DownDirect{
    0.070765936095488272f,
    0.51316753697110817f,
};
inline constexpr std::array<float, 2> kStage1DownDelayed{
    0.25785304455026892f,
    0.81731733582122523f,
};
inline constexpr double kStage1DownDelay = 2.3800867967105201;

} // namespace halfband

// One first-order allpass section, running in the decimated domain.
class Allpass1 {
public:
  void reset() noexcept { x1_ = y1_ = 0.0f; }

  inline float process(float x, float a) noexcept {
    const float y = a * (x - y1_) + x1_;
    x1_ = x;
    y1_ = y;
    return y;
  }

private:
  float x1_ = 0.0f;
  float y1_ = 0.0f;
};

// One 2x half-band stage. The up and down directions carry independent filters
// (and independent state), matching the Linux build's asymmetric configuration.
template <std::size_t UpDirect, std::size_t UpDelayed, std::size_t DownDirect,
          std::size_t DownDelayed>
class HalfBandStage2x {
public:
  using UpDirectCoeffs = std::array<float, UpDirect>;
  using UpDelayedCoeffs = std::array<float, UpDelayed>;
  using DownDirectCoeffs = std::array<float, DownDirect>;
  using DownDelayedCoeffs = std::array<float, DownDelayed>;

  HalfBandStage2x(const UpDirectCoeffs &upDirect, const UpDelayedCoeffs &upDelayed,
                  const DownDirectCoeffs &downDirect,
                  const DownDelayedCoeffs &downDelayed)
      : upDirect_(upDirect), upDelayed_(upDelayed), downDirect_(downDirect),
        downDelayed_(downDelayed) {}

  void reset() noexcept {
    for (auto &s : upDirectState_)
      s.reset();
    for (auto &s : upDelayedState_)
      s.reset();
    for (auto &s : downDirectState_)
      s.reset();
    for (auto &s : downDelayedState_)
      s.reset();
    downPrevOdd_ = 0.0f;
  }

  // One input sample in, two output samples out.
  inline void processUp(float x, float &even, float &odd) noexcept {
    float d = x;
    for (std::size_t i = 0; i < UpDirect; ++i)
      d = upDirectState_[i].process(d, upDirect_[i]);
    float q = x;
    for (std::size_t i = 0; i < UpDelayed; ++i)
      q = upDelayedState_[i].process(q, upDelayed_[i]);
    even = d;
    odd = q;
  }

  // Two input samples in, one output sample out. `even` is u[2n] and `odd` is
  // u[2n+1]; the delayed path consumes the odd sample of the PREVIOUS pair, which
  // is what the z^-1 in the structure amounts to after decimation.
  inline float processDown(float even, float odd) noexcept {
    float d = even;
    for (std::size_t i = 0; i < DownDirect; ++i)
      d = downDirectState_[i].process(d, downDirect_[i]);
    float q = downPrevOdd_;
    for (std::size_t i = 0; i < DownDelayed; ++i)
      q = downDelayedState_[i].process(q, downDelayed_[i]);
    downPrevOdd_ = odd;
    return 0.5f * (d + q);
  }

private:
  const UpDirectCoeffs upDirect_;
  const UpDelayedCoeffs upDelayed_;
  const DownDirectCoeffs downDirect_;
  const DownDelayedCoeffs downDelayed_;

  std::array<Allpass1, UpDirect> upDirectState_{};
  std::array<Allpass1, UpDelayed> upDelayedState_{};
  std::array<Allpass1, DownDirect> downDirectState_{};
  std::array<Allpass1, DownDelayed> downDelayedState_{};
  float downPrevOdd_ = 0.0f;
};

// Mono 4x oversampler: two cascaded half-band stages plus the scratch buffers the
// oversampled region is processed in. prepare() is the only allocating call.
class Oversampler4x {
public:
  static constexpr std::size_t kFactor = 4;

  // Latency in base-rate samples, computed the same way the Linux build reports
  // it: each stage contributes the DC delay of its up and down filters divided by
  // the cumulative oversampling factor at that stage.
  static constexpr double kLatencySamples =
      (halfband::kStage0UpDelay + halfband::kStage0DownDelay) / 2.0 +
      (halfband::kStage1UpDelay + halfband::kStage1DownDelay) / 4.0;

  // Sizes every buffer for blocks of up to maxBlockSamples base-rate samples.
  // Message thread only.
  void prepare(std::size_t maxBlockSamples) {
    maxBlock_ = maxBlockSamples;
    buf2_.assign(maxBlockSamples * 2, 0.0f);
    buf4_.assign(maxBlockSamples * 4, 0.0f);
    reset();
  }

  void reset() noexcept {
    stage0_.reset();
    stage1_.reset();
    std::fill(buf2_.begin(), buf2_.end(), 0.0f);
    std::fill(buf4_.begin(), buf4_.end(), 0.0f);
  }

  float getLatencyInSamples() const noexcept {
    return static_cast<float>(kLatencySamples);
  }

  // Upsamples n base-rate samples and returns the 4x buffer to process in place.
  // Returns nullptr if n exceeds the size prepare() was given.
  inline float *processSamplesUp(const float *in, std::size_t n) noexcept {
    if (n > maxBlock_)
      return nullptr;
    for (std::size_t i = 0; i < n; ++i)
      stage0_.processUp(in[i], buf2_[2 * i], buf2_[2 * i + 1]);
    for (std::size_t i = 0; i < 2 * n; ++i)
      stage1_.processUp(buf2_[i], buf4_[2 * i], buf4_[2 * i + 1]);
    return buf4_.data();
  }

  // Downsamples the 4x buffer back into n base-rate samples.
  inline void processSamplesDown(float *out, std::size_t n) noexcept {
    if (n > maxBlock_)
      return;
    for (std::size_t i = 0; i < 2 * n; ++i)
      buf2_[i] = stage1_.processDown(buf4_[2 * i], buf4_[2 * i + 1]);
    for (std::size_t i = 0; i < n; ++i)
      out[i] = stage0_.processDown(buf2_[2 * i], buf2_[2 * i + 1]);
  }

private:
  HalfBandStage2x<4, 3, 3, 3> stage0_{
      halfband::kStage0UpDirect, halfband::kStage0UpDelayed, halfband::kStage0DownDirect,
      halfband::kStage0DownDelayed};
  HalfBandStage2x<3, 2, 2, 2> stage1_{
      halfband::kStage1UpDirect, halfband::kStage1UpDelayed, halfband::kStage1DownDirect,
      halfband::kStage1DownDelayed};

  std::vector<float> buf2_;
  std::vector<float> buf4_;
  std::size_t maxBlock_ = 0;
};

} // namespace bbmh
