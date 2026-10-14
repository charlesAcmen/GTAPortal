#pragma once

#include <algorithm>
#include <cmath>

namespace portal_sa::camera_projection {

// The independent portal camera always renders to a 960x540 raster.  Projection
// conversion must use that raster, not the aspect ratio of the SA game window.
inline constexpr unsigned kRenderWidth = 960;
inline constexpr unsigned kRenderHeight = 540;
inline constexpr float kRenderAspect =
    static_cast<float>(kRenderWidth) / static_cast<float>(kRenderHeight);

inline float VerticalToHorizontalFovDegrees(float verticalFovDegrees) {
    constexpr float kRadians = 0.01745329251994329577f;
    constexpr float kDegrees = 57.295779513082320876f;
    const float vertical = std::clamp(verticalFovDegrees, 5.0f, 140.0f);
    return 2.0f * std::atan(std::tan(vertical * kRadians * 0.5f) * kRenderAspect) * kDegrees;
}

inline float HorizontalToVerticalFovDegrees(float horizontalFovDegrees) {
    constexpr float kRadians = 0.01745329251994329577f;
    constexpr float kDegrees = 57.295779513082320876f;
    return 2.0f * std::atan(std::tan(horizontalFovDegrees * kRadians * 0.5f) / kRenderAspect) * kDegrees;
}

} // namespace portal_sa::camera_projection
