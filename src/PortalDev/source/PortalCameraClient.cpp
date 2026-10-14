#define WIN32_LEAN_AND_MEAN
#include "PortalCameraClient.h"

#include <cstring>

#include "../../PortalCameraProtocol.h"

struct PortalCameraClient::SharedStateView {
    portal_camera::SharedState* state;
};

PortalCameraClient::~PortalCameraClient() { Disconnect(); }

bool PortalCameraClient::Connect() {
    if (view_) return true;
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                portal_camera::kMappingName);
    if (!mapping_) return false;
    auto* state = static_cast<portal_camera::SharedState*>(MapViewOfFile(
        mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(portal_camera::SharedState)));
    if (!state) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    if (state->magic != portal_camera::kMagic || state->version != portal_camera::kVersion ||
        state->size != sizeof(*state)) {
        UnmapViewOfFile(state);
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    view_ = new SharedStateView{state};
    return true;
}

void PortalCameraClient::Disconnect() {
    if (view_) {
        UnmapViewOfFile(view_->state);
        delete view_;
        view_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

bool PortalCameraClient::Read(Snapshot& snapshot) {
    if (!Connect()) return false;
    auto* state = view_->state;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG before = InterlockedCompareExchange(&state->statusSequence, 0, 0);
        if (before & 1) continue;
        const DWORD processId = state->ownerPid;
        const uint64_t heartbeat = state->statusHeartbeatTickMs;
        float player[3]{}, camera[3]{}, forward[3]{}, up[3]{}, placement[4]{};
        std::memcpy(player, state->playerPosition, sizeof(player));
        std::memcpy(camera, state->cameraPosition, sizeof(camera));
        std::memcpy(forward, state->cameraForward, sizeof(forward));
        std::memcpy(up, state->cameraUp, sizeof(up));
        const float cameraFov=state->cameraFovDegrees;
        const LONG placementSequence =
            InterlockedCompareExchange(&state->portalPlacementSequence, 0, 0);
        if (!(placementSequence & 1))
            std::memcpy(placement, state->portalPlacement, sizeof(placement));
        const bool playerValid = state->playerValid != 0;
        const bool cameraValid = state->cameraValid != 0;
        MemoryBarrier();
        const LONG placementAfter =
            InterlockedCompareExchange(&state->portalPlacementSequence, 0, 0);
        if (before != InterlockedCompareExchange(&state->statusSequence, 0, 0) ||
            placementSequence != placementAfter) continue;
        const uint64_t now = GetTickCount64();
        if (!heartbeat || now < heartbeat || now - heartbeat > 3000) return false;
        snapshot.processId = processId;
        snapshot.ageMs = now - heartbeat;
        std::memcpy(snapshot.playerPosition, player, sizeof(player));
        std::memcpy(snapshot.cameraPosition, camera, sizeof(camera));
        std::memcpy(snapshot.cameraForward, forward, sizeof(forward));
        std::memcpy(snapshot.cameraUp, up, sizeof(up));
        snapshot.cameraFovDegrees=cameraFov;
        snapshot.playerValid = playerValid;
        snapshot.cameraValid = cameraValid;
        snapshot.overrideActive = InterlockedCompareExchange(&state->overrideActive, 0, 0) != 0;
        snapshot.portalPlacementSequence =
            (placementSequence & 1) ? 0u : static_cast<uint32_t>(placementSequence);
        std::memcpy(snapshot.portalPlacement, placement, sizeof(placement));
        return true;
    }
    return false;
}

bool PortalCameraClient::Publish(bool enabled, bool showSaPortal, const float (&position)[3],
                                 const float (&forward)[3], const float (&up)[3],
                                 float fovDegrees) {
    if (!Connect()) return false;
    auto* state = view_->state;
    InterlockedIncrement(&state->commandSequence);
    state->commandFlags = (enabled ? portal_camera::kCommandEnabled : 0) |
        (showSaPortal ? portal_camera::kCommandShowSaPortal : 0);
    state->commandHeartbeatTickMs = GetTickCount64();
    std::memcpy(state->targetPosition, position, sizeof(position));
    std::memcpy(state->targetForward, forward, sizeof(forward));
    std::memcpy(state->targetUp, up, sizeof(up));
    state->targetFovDegrees = fovDegrees;
    MemoryBarrier();
    InterlockedIncrement(&state->commandSequence);
    return true;
}
