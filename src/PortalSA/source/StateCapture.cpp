#include <d3d9.h>
#include <plugin.h>
#include <CCamera.h>
#include <CCutsceneMgr.h>
#include <CLoadingScreen.h>
#include <CMenuManager.h>
#include <CPlayerPed.h>
#include <common.h>
#include "StateCapture.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace portal_sa::state {

constexpr DWORD kStateLogIntervalMs = 100;
State g_state;
std::array<float, 16> g_viewMatrix{};
std::array<float, 16> g_projectionMatrix{};
DWORD g_renderMatrixTick = 0;
uint64_t g_renderMatrixSerial = 0;
DWORD g_lastStateLogTick = 0;

Vec3 Copy(const CVector& value) {
    return {value.x, value.y, value.z};
}

const char* Phase(bool playerValid) {
    if (CLoadingScreen::m_bActive) {
        return "loading";
    }
    if (FrontEndMenuManager.m_bMenuActive) {
        return "menu_or_pause";
    }
    if (CCutsceneMgr::ms_running) {
        return "cutscene";
    }
    return playerValid ? "gameplay" : "no_player";
}

void AppendVec(std::ostringstream& line, const char* name, Vec3 value) {
    line << ' ' << name << "=(" << value.x << ',' << value.y << ',' << value.z << ')';
}

void AppendMatrix(std::ostringstream& line, const char* name, const std::array<float, 16>& matrix) {
    line << ' ' << name << "=[";
    for (size_t index = 0; index < matrix.size(); ++index) {
        if (index) {
            line << ',';
        }
        line << matrix[index];
    }
    line << ']';
}

void OnGameProcess(void (*log)(const std::string&)) {
    const DWORD now = GetTickCount();
    State state;
    state.tick = now;
    if (CPlayerPed* player = FindPlayerPed(0)) {
        state.playerValid = true;
        state.playerPosition = Copy(player->GetPosition());
        if (player->bInVehicle && player->m_pVehicle) {
            state.velocityFromVehicle = true;
            state.playerHeading = player->m_pVehicle->GetHeading();
            state.playerVelocity = Copy(player->m_pVehicle->m_vecMoveSpeed);
        } else {
            state.playerHeading = player->GetHeading();
            state.playerVelocity = Copy(player->m_vecMoveSpeed);
        }
    }

    if (RwCamera* rwCamera = TheCamera.m_pRwCamera) {
        const CMatrix& matrix = TheCamera.m_mCameraMatrix;
        state.cameraValid = true;
        state.cameraPosition = Copy(matrix.pos);
        // CMatrix maps forward to its up field and up to its at field.
        state.cameraForward = Copy(matrix.GetForward());
        state.cameraUp = Copy(matrix.GetUp());
        state.cameraIndex = TheCamera.m_nActiveCam;
        if (state.cameraIndex < 3) {
            state.fovDegrees = TheCamera.m_aCams[state.cameraIndex].m_fFOV;
        }
        state.nearClip = RwCameraGetNearClipPlane(rwCamera);
        state.farClip = RwCameraGetFarClipPlane(rwCamera);
        if (const RwV2d* window = RwCameraGetViewWindow(rwCamera)) {
            state.viewWindowX = window->x;
            state.viewWindowY = window->y;
        }
    }
    g_state = state;

    if (g_lastStateLogTick && now - g_lastStateLogTick < kStateLogIntervalMs) {
        return;
    }
    g_lastStateLogTick = now;

    std::ostringstream line;
    line << std::fixed << std::setprecision(4);
    line << "state tick_ms=" << now << " phase=" << Phase(state.playerValid)
         << " player_valid=" << state.playerValid << " camera_valid=" << state.cameraValid;
    if (state.playerValid) {
        AppendVec(line, "player_position", state.playerPosition);
        line << " player_heading_rad=" << state.playerHeading;
        AppendVec(line, "player_move_speed_per_tick", state.playerVelocity);
        line << " velocity_source=" << (state.velocityFromVehicle ? "vehicle" : "ped");
    }
    if (state.cameraValid) {
        AppendVec(line, "camera_position", state.cameraPosition);
        AppendVec(line, "camera_forward", state.cameraForward);
        AppendVec(line, "camera_up", state.cameraUp);
        line << " camera_index=" << state.cameraIndex << " camera_fov_deg=" << state.fovDegrees
             << " near=" << state.nearClip << " far=" << state.farClip
             << " view_window=(" << state.viewWindowX << ',' << state.viewWindowY << ')';
    }
    if (g_renderMatrixTick && now - g_renderMatrixTick < 1000) {
        line << " render_matrix_age_ms=" << now - g_renderMatrixTick;
        AppendMatrix(line, "render_view_row_major", g_viewMatrix);
        AppendMatrix(line, "render_projection_row_major", g_projectionMatrix);
    }
    log(line.str());
}

void OnRenderScene() {
    const _D3DMATRIX* view = GetD3DViewTransform();
    const _D3DMATRIX* projection = GetD3DProjTransform();
    if (view && projection) {
        std::memcpy(g_viewMatrix.data(), view, sizeof(_D3DMATRIX));
        std::memcpy(g_projectionMatrix.data(), projection, sizeof(_D3DMATRIX));
        g_renderMatrixTick = GetTickCount();
        ++g_renderMatrixSerial;
    }
}

} // namespace portal_sa::state
