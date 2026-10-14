#pragma once

#include <windows.h>
#include <cstdint>

namespace portal_camera {

constexpr wchar_t kMappingName[] = L"Local\\GTAPortal.PortalCamera.v1";
constexpr uint32_t kMagic = 0x31435047; // GPC1
constexpr uint32_t kVersion = 3;
constexpr uint32_t kCommandEnabled = 1u << 0;
constexpr uint32_t kCommandShowSaPortal = 1u << 1;

#pragma pack(push, 8)
struct SharedState {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t ownerPid;

    // Written only by PortalSA. Readers use the even sequence as a seqlock.
    volatile LONG statusSequence;
    uint32_t playerValid;
    uint32_t cameraValid;
    uint32_t reserved0;
    uint64_t statusHeartbeatTickMs;
    float playerPosition[3];
    float cameraPosition[3];
    float cameraForward[3];
    float cameraUp[3];
    float cameraFovDegrees;

    // Written only by PortalDev. PortalSA uses the even sequence as a seqlock.
    volatile LONG commandSequence;
    uint32_t commandFlags;
    uint64_t commandHeartbeatTickMs;
    float targetPosition[3];
    float targetForward[3];
    float targetUp[3];
    float targetFovDegrees;

    // Acknowledgement/status written by PortalSA.
    volatile LONG appliedCommandSequence;
    volatile LONG overrideActive;

    // Written by PortalSA when F9 places its world portal. PortalDev uses this
    // as the new SA camera-mapping anchor and persists it.
    volatile LONG portalPlacementSequence;
    float portalPlacement[4]; // circle center x, y, z, yaw degrees
};
#pragma pack(pop)

static_assert(sizeof(SharedState) == 184);

} // namespace portal_camera
