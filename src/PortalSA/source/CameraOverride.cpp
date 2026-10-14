#include "CameraOverride.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

#include <CCamera.h>
#include <CDraw.h>
#include <CPlayerPed.h>
#include <common.h>

#include "CameraProjection.h"
#include "StateCapture.h"
#include "../../PortalCameraProtocol.h"

namespace portal_sa::camera_override {
namespace {

HANDLE g_mapping{};
portal_camera::SharedState* g_shared{};
bool g_overrideActive{};
bool g_f8Down{};
bool g_portalVisible{};
CPlayerPed* g_hiddenPlayer{};
bool g_hiddenPlayerWasVisible{};

bool EnsureMapping(void (*log)(const std::string&)) {
    if (g_shared) return true;
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(portal_camera::SharedState), portal_camera::kMappingName);
    if (!g_mapping) return false;
    g_shared = static_cast<portal_camera::SharedState*>(MapViewOfFile(
        g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(portal_camera::SharedState)));
    if (!g_shared) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }
    ZeroMemory(g_shared, sizeof(*g_shared));
    g_shared->version = portal_camera::kVersion;
    g_shared->size = sizeof(*g_shared);
    g_shared->ownerPid = GetCurrentProcessId();
    MemoryBarrier();
    g_shared->magic = portal_camera::kMagic;
    log("Portal camera control mapping ready");
    return true;
}

bool IsGameForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void PublishStatus() {
    InterlockedIncrement(&g_shared->statusSequence);
    g_shared->statusHeartbeatTickMs = GetTickCount64();
    g_shared->playerValid = state::g_state.playerValid ? 1u : 0u;
    g_shared->cameraValid = state::g_state.cameraValid ? 1u : 0u;
    const auto copy = [](float (&out)[3], const state::Vec3& value) {
        out[0] = value.x; out[1] = value.y; out[2] = value.z;
    };
    copy(g_shared->playerPosition, state::g_state.playerPosition);
    copy(g_shared->cameraPosition, state::g_state.cameraPosition);
    copy(g_shared->cameraForward, state::g_state.cameraForward);
    copy(g_shared->cameraUp, state::g_state.cameraUp);
    g_shared->cameraFovDegrees=state::g_state.fovDegrees;
    MemoryBarrier();
    InterlockedIncrement(&g_shared->statusSequence);
}

struct Command {
    LONG sequence{};
    uint32_t flags{};
    uint64_t heartbeat{};
    float position[3]{};
    float forward[3]{};
    float up[3]{};
    float fov{};
};

bool ReadCommand(Command& command) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG before = InterlockedCompareExchange(&g_shared->commandSequence, 0, 0);
        if (before & 1) continue;
        command.sequence = before;
        command.flags = g_shared->commandFlags;
        command.heartbeat = g_shared->commandHeartbeatTickMs;
        std::memcpy(command.position, g_shared->targetPosition, sizeof(command.position));
        std::memcpy(command.forward, g_shared->targetForward, sizeof(command.forward));
        std::memcpy(command.up, g_shared->targetUp, sizeof(command.up));
        command.fov = g_shared->targetFovDegrees;
        MemoryBarrier();
        if (before == InterlockedCompareExchange(&g_shared->commandSequence, 0, 0)) return true;
    }
    return false;
}

bool FiniteVector(const float (&value)[3]) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

void Restore(void (*log)(const std::string&), const char* reason) {
    if (!g_overrideActive) return;
    TheCamera.RestoreWithJumpCut();
    if (g_hiddenPlayer && FindPlayerPed(0) == g_hiddenPlayer)
        g_hiddenPlayer->bIsVisible = g_hiddenPlayerWasVisible;
    g_hiddenPlayer = nullptr;
    g_overrideActive = false;
    InterlockedExchange(&g_shared->overrideActive, 0);
    log(std::string("Portal main camera restored: ") + reason);
}

void LogMarker(void (*log)(const std::string&)) {
    std::ostringstream line;
    line << std::fixed << std::setprecision(6)
         << "SA_CALIBRATION_MARKER player_valid=" << state::g_state.playerValid
         << " player_position=(" << state::g_state.playerPosition.x << ','
         << state::g_state.playerPosition.y << ',' << state::g_state.playerPosition.z << ')'
         << " player_heading_rad=" << state::g_state.playerHeading
         << " camera_position=(" << state::g_state.cameraPosition.x << ','
         << state::g_state.cameraPosition.y << ',' << state::g_state.cameraPosition.z << ')'
         << " camera_forward=(" << state::g_state.cameraForward.x << ','
         << state::g_state.cameraForward.y << ',' << state::g_state.cameraForward.z << ')';
    log(line.str());
}

} // namespace

void OnGameProcess(void (*log)(const std::string&)) {
    if (!EnsureMapping(log)) return;
    PublishStatus();

    const bool f8 = IsGameForeground() && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !g_f8Down) LogMarker(log);
    g_f8Down = f8;

    Command command{};
    if (!ReadCommand(command)) return;
    g_portalVisible = (command.flags & portal_camera::kCommandShowSaPortal) != 0;
    const uint64_t now = GetTickCount64();
    const bool fresh = command.heartbeat && now >= command.heartbeat &&
                       now - command.heartbeat <= 500;
    const bool valid = FiniteVector(command.position) && FiniteVector(command.forward) &&
                       FiniteVector(command.up);
    if (!(command.flags & portal_camera::kCommandEnabled)) {
        Restore(log, "disabled");
        return;
    }
    if (!fresh || !valid || !state::g_state.playerValid) {
        Restore(log, fresh ? "invalid target or player unavailable" : "command stale");
        return;
    }

    const CVector position(command.position[0], command.position[1], command.position[2]);
    const CVector target(position.x + command.forward[0] * 10.0f,
                         position.y + command.forward[1] * 10.0f,
                         position.z + command.forward[2] * 10.0f);
    // Process_Fixed adds world-up before normalizing this value.
    const CVector upOffset(command.up[0], command.up[1], command.up[2] - 1.0f);
    TheCamera.SetCamPositionForFixedMode(&position, &upOffset);
    TheCamera.TakeControlNoEntity(&target, SWITCHTYPE_JUMPCUT, 1);

    // GTA V reports vertical FOV. SA's camera field uses horizontal FOV, so
    // convert using the same fixed 16:9 portal aspect used by the compositor.
    const float verticalFov = std::clamp(command.fov, 5.0f, 140.0f);
    const float horizontalFov = camera_projection::VerticalToHorizontalFovDegrees(verticalFov);
    TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV = horizontalFov;
    CDraw::SetFOV(horizontalFov);
    CPlayerPed* player = FindPlayerPed(0);
    if (player) {
        if (g_hiddenPlayer != player) {
            if (g_hiddenPlayer && FindPlayerPed(0) == g_hiddenPlayer)
                g_hiddenPlayer->bIsVisible = g_hiddenPlayerWasVisible;
            g_hiddenPlayer = player;
            g_hiddenPlayerWasVisible = player->bIsVisible;
        }
        player->bIsVisible = false;
    }
    if (!g_overrideActive) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2)
                << "Portal main camera sync active (V render pose, vertical FOV "
                << verticalFov << " -> SA horizontal FOV " << horizontalFov
                << ", WGC output)";
        log(message.str());
    }
    g_overrideActive = true;
    InterlockedExchange(&g_shared->appliedCommandSequence, command.sequence);
    InterlockedExchange(&g_shared->overrideActive, 1);
}

void PublishPortalPlacement(float x, float y, float centerZ, float yawDegrees) {
    if (!g_shared) return;
    InterlockedIncrement(&g_shared->portalPlacementSequence);
    g_shared->portalPlacement[0] = x;
    g_shared->portalPlacement[1] = y;
    g_shared->portalPlacement[2] = centerZ;
    g_shared->portalPlacement[3] = yawDegrees;
    MemoryBarrier();
    InterlockedIncrement(&g_shared->portalPlacementSequence);
}

bool PortalVisible() { return g_portalVisible; }

void Shutdown() {
    if (g_overrideActive) TheCamera.RestoreWithJumpCut();
    if (g_hiddenPlayer && FindPlayerPed(0) == g_hiddenPlayer)
        g_hiddenPlayer->bIsVisible = g_hiddenPlayerWasVisible;
    g_hiddenPlayer = nullptr;
    if (g_shared) UnmapViewOfFile(g_shared);
    if (g_mapping) CloseHandle(g_mapping);
    g_shared = nullptr;
    g_mapping = nullptr;
    g_overrideActive = false;
    g_portalVisible = false;
}

} // namespace portal_sa::camera_override
