#define WIN32_LEAN_AND_MEAN
#include "PortalVClient.h"
#include "../../PortalVProtocol.h"

#include <cstring>

struct PortalVClient::SharedStateView {
    portal_v::SharedState* state;
};

PortalVClient::~PortalVClient() { Disconnect(); }

bool PortalVClient::Connect() {
    if (view_) return true;
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, portal_v::kMappingName);
    if (!mapping_) return false;
    auto* state = static_cast<portal_v::SharedState*>(MapViewOfFile(
        mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(portal_v::SharedState)));
    if (!state) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    view_ = new SharedStateView{state};
    return true;
}

void PortalVClient::Disconnect() {
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

bool PortalVClient::Read(Snapshot& snapshot) {
    if (!Connect()) return false;
    auto* state = view_->state;
    if (state->magic != portal_v::kMagic || state->version != portal_v::kVersion ||
        state->size != sizeof(*state)) {
        Disconnect();
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG before = InterlockedCompareExchange(&state->sequence, 0, 0);
        if (before & 1) continue;
        const DWORD processId = state->processId;
        const uint64_t heartbeat = state->heartbeatTickMs;
        const uint64_t frameId = state->frameId;
        float position[3]{};
        float rotation[3]{};
        float renderedPosition[3]{};
        float renderedRotation[3]{};
        std::memcpy(position, state->cameraPosition, sizeof(position));
        std::memcpy(rotation, state->cameraRotation, sizeof(rotation));
        std::memcpy(renderedPosition, state->renderedCameraPosition, sizeof(renderedPosition));
        std::memcpy(renderedRotation, state->renderedCameraRotation, sizeof(renderedRotation));
        const float fov = state->fovDegrees;
        const float renderedFov = state->renderedFovDegrees;
        const bool valid = state->cameraValid != 0;
        MemoryBarrier();
        if (before != InterlockedCompareExchange(&state->sequence, 0, 0)) continue;
        const uint64_t now = GetTickCount64();
        if (!heartbeat || now < heartbeat || now - heartbeat > 3000) {
            Disconnect();
            return false;
        }
        snapshot.processId = processId;
        snapshot.ageMs = now - heartbeat;
        snapshot.frameId = frameId;
        std::memcpy(snapshot.cameraPosition, position, sizeof(position));
        std::memcpy(snapshot.cameraRotation, rotation, sizeof(rotation));
        snapshot.fovDegrees = fov;
        std::memcpy(snapshot.renderedCameraPosition, renderedPosition, sizeof(renderedPosition));
        std::memcpy(snapshot.renderedCameraRotation, renderedRotation, sizeof(renderedRotation));
        snapshot.renderedFovDegrees = renderedFov;
        snapshot.cameraValid = valid;
        snapshot.overlayEnabled = InterlockedCompareExchange(&state->overlayEnabled, 0, 0) != 0;
        snapshot.teleportResult = InterlockedCompareExchange(&state->teleportResult, 0, 0);
        snapshot.teleportAppliedRequest =
            InterlockedCompareExchange(&state->teleportAppliedRequest, 0, 0);
        return true;
    }
    return false;
}

bool PortalVClient::SetOverlayEnabled(bool enabled) {
    Snapshot snapshot;
    if (!Read(snapshot)) return false;
    InterlockedExchange(&view_->state->overlayEnabled, enabled ? 1 : 0);
    return true;
}

bool PortalVClient::RequestTeleport(const float (&position)[3], float headingDegrees) {
    Snapshot snapshot;
    if (!Read(snapshot)) return false;
    std::memcpy(view_->state->teleportPosition, position, sizeof(position));
    view_->state->teleportHeadingDegrees = headingDegrees;
    MemoryBarrier();
    InterlockedExchange(&view_->state->teleportResult, portal_v::kTeleportPending);
    InterlockedIncrement(&view_->state->teleportRequest);
    return true;
}
