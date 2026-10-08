#pragma once
// TSShapeInstance detail selection (setDetailFromDistance,
// selectCurrentDetail), as ported by t2-mapper DTSModel.selectDetail
// without its hysteresis. Pure math.
#include <algorithm>
#include <cstdint>

namespace DTSDetail {

// The shape's projected radius in pixels, scaled by $pref::TS::detailAdjust:
// radius x scale / distance x (viewport height / 2 / tan(vfov / 2)).
inline float pixelSize(float radius, float scale, float distance, float pixelScale, float detailAdjust) {
    return radius * scale / std::max(distance, 0.001f) * pixelScale * detailAdjust;
}

// TSShape::init: the last detail with a non-negative size is the smallest
// one drawn. Details are a sequence of objects with a `size` member.
template <typename Details>
inline void smallestVisible(const Details& details, float& size, int32_t& detail) {
    size = 0.0f;
    detail = -1;
    for (size_t i = 0; i < details.size(); ++i)
        if (details[i].size >= 0.0f) {
            detail = (int32_t)i;
            size = details[i].size;
        }
}

// The first detail whose size the projected size reaches, or -1 (not drawn)
// at or below the smallest visible size. Utility details (negative size)
// are never selected.
template <typename Details>
inline int32_t select(const Details& details, int32_t smallestVisibleDL, float smallestVisibleSize,
                      float pixels) {
    if (smallestVisibleDL < 0 || !(pixels > smallestVisibleSize)) return -1;
    for (int32_t i = 0; i < smallestVisibleDL && i < (int32_t)details.size(); ++i)
        if (details[i].size >= 0.0f && details[i].size <= pixels) return i;
    return smallestVisibleDL;
}

} // namespace DTSDetail
