#pragma once

namespace portal_visual {

constexpr float kRadius = 4.0f;
constexpr float kWidth = kRadius * 2.0f;
constexpr float kHeight = kRadius * 2.0f;
constexpr float kHalfWidth = kRadius;
constexpr float kHalfHeight = kRadius;
constexpr float kSubmergedDepth = 0.60f;
constexpr float kCenterAboveGround = kRadius - kSubmergedDepth;

// View-camera offsets are independent of the calibrated portal geometry.
// Keep the saved circle center authoritative and lift only GTA V's scripted
// destination camera so it does not feel attached to the pavement.
constexpr float kGtaVPortalCameraHeightOffset = 1.20f;

// Fill the tall aperture without stretching a 16:9 source: enlarge it and
// crop equal amounts from the left and right.
constexpr float kCropLeft = 0.21875f;
constexpr float kCropTop = 0.0f;
constexpr float kCropRight = 0.78125f;
constexpr float kCropBottom = 1.0f;

namespace gta_v {
constexpr float groundX = 88.110458f;
constexpr float groundY = -1922.158081f;
constexpr float groundZ = 20.698630f;
constexpr float yawDegrees = -141.490860f;
}

namespace gta_sa {
constexpr float groundX = 2468.084229f;
constexpr float groundY = -1660.620117f;
constexpr float groundZ = 13.311936f;
constexpr float yawDegrees = -99.351112f;
}

} // namespace portal_visual
