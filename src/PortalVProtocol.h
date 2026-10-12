#pragma once

#include <windows.h>
#include <cstdint>

namespace portal_v {

constexpr wchar_t kMappingName[] = L"Local\\GTAPortal.VState.v3";
constexpr uint32_t kMagic = 0x31565447; // GTV1
constexpr uint32_t kVersion = 3;

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
};

static_assert(sizeof(SharedState) == 152);

} // namespace portal_v
