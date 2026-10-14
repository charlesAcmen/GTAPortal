#pragma once

#include <windows.h>
#include <cstdint>

namespace portal_v {

constexpr wchar_t kMappingName[] = L"Local\\GTAPortal.VState.v3";
constexpr uint32_t kMagic = 0x31565447; // GTV1
constexpr uint32_t kVersion = 6;
constexpr uint32_t kCameraCommandEnabled=1u<<0;

enum TeleportResult : LONG {
    kTeleportIdle = 0,
    kTeleportPending = 1,
    kTeleportSuccess = 2,
    kTeleportRejectedOnline = -1,
    kTeleportPlayerUnavailable = -2,
    kTeleportGroundUnavailable = -3,
};

// The ASI owns this mapping. The tool reads a latest-only snapshot and writes
// one atomic overlay command; no frame images or pointers cross this boundary.
struct SharedState {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t processId;
    volatile LONG sequence;
    volatile LONG overlayEnabled;
    uint64_t heartbeatTickMs;
    uint64_t frameId;
    float cameraPosition[3];
    float cameraRotation[3];
    float fovDegrees;
    uint32_t cameraValid;
    float renderedCameraPosition[3];
    float renderedCameraRotation[3];
    float renderedFovDegrees;
    volatile LONG teleportRequest;
    volatile LONG teleportAppliedRequest;
    volatile LONG teleportResult;
    float teleportPosition[3];
    float teleportHeadingDegrees;
    float portalPlaneCenter[3];
    float portalPlaneYawDegrees;
    uint32_t portalPlaneValid;
    volatile LONG portalVisible;
    volatile LONG reloadFxRequest;
    volatile LONG reloadFxApplied;
    volatile LONG cameraCommandSequence;
    uint32_t cameraCommandFlags;
    uint64_t cameraCommandHeartbeatTickMs;
    float targetCameraPosition[3];
    float targetCameraRotation[3];
    float targetCameraFovDegrees;
    volatile LONG appliedCameraCommandSequence;
    volatile LONG cameraOverrideActive;
};

static_assert(sizeof(SharedState) == 216);

} // namespace portal_v
