// The product palette, in one place.
//
// It lives in gfx/ rather than in geometry.h because two things that are not the editor draw
// with it: the in-plug-in file browser (which the rack also uses, on a different canvas entirely)
// and anything else built on Canvas alone. geometry.h re-exports every name below into `geo`,
// so existing code goes on saying geo::kAccent and there is still exactly one definition.
//
// 0xRRGGBB throughout, matching Canvas::setColor and gui/geometry.sh — which is the art pipeline's
// source of truth for these values; keep the two in sync by hand.

#pragma once

#include <cstdint>

namespace Rations
{
namespace pal
{

constexpr uint32_t kBgColor = 0x121011;   // letterbox around the head
constexpr uint32_t kFaceColor = 0x1E1C1D; // faceplate fallback if base.png fails
constexpr uint32_t kGold = 0xB88B4C;      // piping / hairlines, sampled from the art
constexpr uint32_t kTextColor = 0xFFFFFF; // labels
constexpr uint32_t kDimColor = 0x9A9490;  // empty / disabled text
// Theme accent: green, inherited from NAMp, which shares this panel art (the sibling
// single-capture plug-in is azure
// (0x5085E8 / 0x6A9FF0).
constexpr uint32_t kAccent = 0x3FD05A;
constexpr uint32_t kAccentBright = 0x7FE89A; // bright tick at the top of a meter fill
constexpr uint32_t kPeakColor = 0xFF3B30;    // meter peak marker

} // namespace pal
} // namespace Rations
