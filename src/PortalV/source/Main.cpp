#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <main.h>
#include <types.h>
#include <nativeCaller.h>
#include <natives.h>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>

#include "../../DevelopmentAnchors.h"
#include "../../PortalVProtocol.h"
#include "Compositor.h"
#include "PortalCalibration.h"

namespace {
HMODULE g_module{};
HANDLE g_mapping{};
portal_v::SharedState* g_state{};
// User-confirmed Grove Street calibration position, 2026-10-10. This is a
// player/vehicle spawn point, deliberately separate from the portal plane.
float g_spawnPosition[3]{development_anchors::gta_v::x,
                         development_anchors::gta_v::y,
                         development_anchors::gta_v::z};
float g_spawnHeading{development_anchors::gta_v::headingDegrees};
bool g_spawnValid{true};
LONG g_lastTeleportRequest{};
LONG g_lastReloadFxRequest{};
bool g_autoTeleportQueued{};
uint64_t g_playerReadySince{};
Cam g_portalCamera{};

void Log(const char* message);

void DisablePortalCamera(){
    if(!g_portalCamera)return;
    CAM::RENDER_SCRIPT_CAMS(FALSE,FALSE,0,TRUE,FALSE);
    CAM::DESTROY_CAM(g_portalCamera,TRUE);g_portalCamera=0;
    if(g_state)InterlockedExchange(&g_state->cameraOverrideActive,0);
}

void ProcessPortalCamera(){
    if(!g_state)return;
    LONG sequence=0;uint32_t flags=0;uint64_t heartbeat=0;
    float position[3]{},rotation[3]{},fov=70.0f;bool read=false;
    for(int attempt=0;attempt<3;++attempt){
        const LONG before=InterlockedCompareExchange(&g_state->cameraCommandSequence,0,0);
        if(before&1)continue;sequence=before;flags=g_state->cameraCommandFlags;
        heartbeat=g_state->cameraCommandHeartbeatTickMs;
        std::memcpy(position,g_state->targetCameraPosition,sizeof(position));
        std::memcpy(rotation,g_state->targetCameraRotation,sizeof(rotation));
        fov=g_state->targetCameraFovDegrees;MemoryBarrier();
        if(before==InterlockedCompareExchange(&g_state->cameraCommandSequence,0,0)){read=true;break;}
    }
    const uint64_t now=GetTickCount64();
    const bool finite=std::isfinite(position[0])&&std::isfinite(position[1])&&
        std::isfinite(position[2])&&std::isfinite(rotation[0])&&
        std::isfinite(rotation[1])&&std::isfinite(rotation[2])&&std::isfinite(fov);
    const bool enabled=read&&(flags&portal_v::kCameraCommandEnabled)&&heartbeat&&
        now>=heartbeat&&now-heartbeat<=500&&finite;
    if(!enabled){DisablePortalCamera();return;}
    if(!g_portalCamera){
        char name[]="DEFAULT_SCRIPTED_CAMERA";
        g_portalCamera=CAM::CREATE_CAM(name,TRUE);
        if(!g_portalCamera)return;
        CAM::RENDER_SCRIPT_CAMS(TRUE,FALSE,0,TRUE,FALSE);
        Log("PortalV scripted portal camera active");
    }
    CAM::SET_CAM_COORD(g_portalCamera,position[0],position[1],position[2]);
    CAM::SET_CAM_ROT(g_portalCamera,rotation[0],rotation[1],rotation[2],2);
    CAM::SET_CAM_FOV(g_portalCamera,std::clamp(fov,5.0f,130.0f));
    InterlockedExchange(&g_state->appliedCameraCommandSequence,sequence);
    InterlockedExchange(&g_state->cameraOverrideActive,1);
}

bool IsGameForeground() {
    DWORD processId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &processId);
    return processId == GetCurrentProcessId();
}

bool GameFilePath(const wchar_t* name, wchar_t (&path)[MAX_PATH]) {
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), name);
    return true;
}

void LoadSpawn() {
    wchar_t path[MAX_PATH]{};
    if (!GameFilePath(L"PortalVSpawn.txt", path)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"r") != 0 || !file) return;
    g_spawnValid = fscanf_s(file, "%f %f %f %f", &g_spawnPosition[0], &g_spawnPosition[1],
                            &g_spawnPosition[2], &g_spawnHeading) == 4;
    fclose(file);
}

void SaveSpawn() {
    wchar_t path[MAX_PATH]{};
    if (!GameFilePath(L"PortalVSpawn.txt", path)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w") != 0 || !file) return;
    fprintf(file, "%.6f %.6f %.6f %.6f\n", g_spawnPosition[0], g_spawnPosition[1],
            g_spawnPosition[2], g_spawnHeading);
    fclose(file);
}

void Log(const char* message) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"PortalV.log");
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"a") == 0 && file) {
        fprintf(file, "%llu %s\n", static_cast<unsigned long long>(GetTickCount64()), message);
        fclose(file);
    }
}

void ProcessTeleport() {
    if (!g_state) return;
    const LONG request = InterlockedCompareExchange(&g_state->teleportRequest, 0, 0);
    if (!request || request == g_lastTeleportRequest) return;
    g_lastTeleportRequest = request;
    InterlockedExchange(&g_state->teleportResult, portal_v::kTeleportPending);

    if (NETWORK::NETWORK_IS_IN_SESSION()) {
        InterlockedExchange(&g_state->teleportResult, portal_v::kTeleportRejectedOnline);
        InterlockedExchange(&g_state->teleportAppliedRequest, request);
        Log("PortalV teleport rejected: Online session detected");
        return;
    }
    const Ped ped = PLAYER::PLAYER_PED_ID();
    if (!ENTITY::DOES_ENTITY_EXIST(ped) || !PLAYER::IS_PLAYER_PLAYING(PLAYER::PLAYER_ID())) {
        InterlockedExchange(&g_state->teleportResult, portal_v::kTeleportPlayerUnavailable);
        InterlockedExchange(&g_state->teleportAppliedRequest, request);
        Log("PortalV teleport rejected: Story Mode player unavailable");
        return;
    }

    Entity entity = ped;
    const bool playerInVehicle=PED::IS_PED_IN_ANY_VEHICLE(ped,FALSE)!=FALSE;
    if (playerInVehicle) {
        entity = PED::GET_VEHICLE_PED_IS_IN(ped, FALSE);
    }
    const float x = g_state->teleportPosition[0];
    const float y = g_state->teleportPosition[1];
    const float requestedZ = g_state->teleportPosition[2];
    const float heading = g_state->teleportHeadingDegrees;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(requestedZ) ||
        !std::isfinite(heading)) {
        InterlockedExchange(&g_state->teleportResult, portal_v::kTeleportGroundUnavailable);
        InterlockedExchange(&g_state->teleportAppliedRequest, request);
        Log("PortalV teleport rejected: non-finite target");
        return;
    }

    ENTITY::FREEZE_ENTITY_POSITION(entity, TRUE);
    STREAMING::REQUEST_COLLISION_AT_COORD(x, y, requestedZ);
    STREAMING::LOAD_SCENE(x, y, requestedZ);
    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(entity, x, y, requestedZ + 20.0f, FALSE, FALSE, TRUE);

    float groundZ = requestedZ;
    bool groundFound = false;
    for (int frame = 0; frame < 120 && !groundFound; ++frame) {
        STREAMING::REQUEST_COLLISION_AT_COORD(x, y, requestedZ);
        const float probeZ = frame < 40 ? requestedZ + 100.0f :
                             frame < 80 ? requestedZ + 300.0f : requestedZ + 800.0f;
        groundFound = GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(x, y, probeZ, &groundZ, FALSE) != FALSE;
        if (!groundFound) WAIT(0);
    }
    const float finalZ = groundFound ? groundZ + 1.0f : requestedZ;
    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(entity, x, y, finalZ, FALSE, FALSE, TRUE);
    ENTITY::SET_ENTITY_HEADING(entity, heading);
    ENTITY::FREEZE_ENTITY_POSITION(entity, FALSE);
    if(!playerInVehicle){
        constexpr float clearRadius=12.0f;
        VEHICLE::SET_ALL_VEHICLE_GENERATORS_ACTIVE_IN_AREA(
            x-clearRadius,y-clearRadius,finalZ-5.0f,
            x+clearRadius,y+clearRadius,finalZ+5.0f,FALSE,FALSE);
        GAMEPLAY::CLEAR_AREA_OF_VEHICLES(x,y,finalZ,clearRadius,FALSE,FALSE,FALSE,FALSE,FALSE);
    }
    InterlockedExchange(&g_state->teleportResult,
                        groundFound ? portal_v::kTeleportSuccess : portal_v::kTeleportGroundUnavailable);
    InterlockedExchange(&g_state->teleportAppliedRequest, request);
    Log(groundFound ? "PortalV teleport completed with ground collision"
                    : "PortalV teleport completed at configured Z; ground probe unavailable");
}

void QueueAutoTeleport(uint64_t tick) {
    if (g_autoTeleportQueued || !g_state || !g_spawnValid) return;
    const Ped ped = PLAYER::PLAYER_PED_ID();
    const bool ready = ENTITY::DOES_ENTITY_EXIST(ped) &&
                       PLAYER::IS_PLAYER_PLAYING(PLAYER::PLAYER_ID());
    if (!ready) {
        g_playerReadySince = 0;
        return;
    }
    if (!g_playerReadySince) {
        g_playerReadySince = tick;
        return;
    }
    if (tick - g_playerReadySince < 2000) return;

    g_state->teleportPosition[0] = g_spawnPosition[0];
    g_state->teleportPosition[1] = g_spawnPosition[1];
    g_state->teleportPosition[2] = g_spawnPosition[2];
    g_state->teleportHeadingDegrees = g_spawnHeading;
    MemoryBarrier();
    InterlockedIncrement(&g_state->teleportRequest);
    g_autoTeleportQueued = true;
    Log("PortalV Story Mode player ready; automatic saved-spawn teleport queued");
}

void OpenState() {
    if (g_state) return;
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   sizeof(portal_v::SharedState), portal_v::kMappingName);
    if (!g_mapping) return;
    g_state = static_cast<portal_v::SharedState*>(MapViewOfFile(
        g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(portal_v::SharedState)));
    if (!g_state) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return;
    }
    ZeroMemory(g_state, sizeof(*g_state));
    g_state->version = portal_v::kVersion;
    g_state->size = sizeof(*g_state);
    g_state->processId = GetCurrentProcessId();
    g_state->overlayEnabled = 0;
    g_state->portalVisible = 1;
    MemoryBarrier();
    g_state->magic = portal_v::kMagic;
}

void DrawTestPattern(uint64_t tick) {
    const float phase = static_cast<float>(tick % 3000) / 3000.0f;
    const float movingX = 0.12f + phase * 0.16f;
    constexpr UINT64 drawRectHash = 0x3A618A217E5154F0;
    invoke<Void>(drawRectHash, 0.18f, 0.18f, 0.29f, 0.19f, 12, 18, 29, 210);
    invoke<Void>(drawRectHash, movingX, 0.18f, 0.045f, 0.16f, 255, 45, 153, 240);
    invoke<Void>(drawRectHash, 0.18f, 0.27f, 0.29f * phase, 0.012f, 80, 220, 255, 230);
}

void ScriptMain() {
    Log("PortalV script started");
    OpenState();
    portal_v::calibration::Initialize(g_module);
    LoadSpawn();
    if (!g_state) Log("PortalV state mapping unavailable");
    uint64_t nextRegister = 0;
    bool registered = false;
    bool previousF9 = false;
    bool previousF8 = false;
    bool previousAdd = false, previousSubtract = false;
    bool previousUp = false, previousDown = false;
    bool previousF10 = false;
    while (true) {
        const uint64_t tick = GetTickCount64();
        if (!registered && tick >= nextRegister) {
            registered = portal_v::compositor::TryRegister(g_module);
            if (registered) Log("PortalV ReShade add-on registered");
            nextRegister = tick + 3000;
        }
        // Native hashes are supplied by the user's Script Hook V SDK.
        const Vector3 position = invoke<Vector3>(0x14D6F5678D8F1B37);
        const Vector3 rotation = invoke<Vector3>(0x837765A25378F0BB, 2);
        const float fov = invoke<float>(0x65019750A0324133);
        // The final camera includes GTA's rendered shake and is the depth buffer's camera.
        const Vector3 renderedPosition = invoke<Vector3>(0xA200EB1EE790F448);
        const Vector3 renderedRotation = invoke<Vector3>(0x5B4E4C817FCC2DFB, 2);
        const float renderedFov = invoke<float>(0x80EC114669DAEFF4);
        const float nearClip = invoke<float>(0xD0082607100D7193);
        const float farClip = invoke<float>(0xDFC8CBC606FDB0FC);
        QueueAutoTeleport(tick);
        ProcessTeleport();
        ProcessPortalCamera();
        if(g_state){
            const LONG reload=InterlockedCompareExchange(&g_state->reloadFxRequest,0,0);
            if(reload&&reload!=g_lastReloadFxRequest){
                g_lastReloadFxRequest=reload;
                portal_v::compositor::ReloadEffects();
                InterlockedExchange(&g_state->reloadFxApplied,reload);
                Log("PortalV queued PortalSA.fx hot reload");
            }
        }
        const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !previousF9 && IsGameForeground()) {
            const Ped ped = PLAYER::PLAYER_PED_ID();
            if (ENTITY::DOES_ENTITY_EXIST(ped)) {
                const Vector3 current = ENTITY::GET_ENTITY_COORDS(ped, TRUE);
                const float currentPosition[3]{current.x, current.y, current.z};
                const float currentHeading = ENTITY::GET_ENTITY_HEADING(ped);
                portal_v::calibration::BeginAtPortalBase(currentPosition, currentHeading);
                Log("PortalV circle placed and saved at current player (F9)");
            }
        }
        previousF9 = f9;
        const bool f8=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
        if(f8&&!previousF8&&IsGameForeground()){
            portal_v::calibration::BeginEditing();
            portal_v::compositor::OpenCalibrationOverlay();
        }
        previousF8=f8;
        const bool add=(GetAsyncKeyState(VK_ADD)&0x8000)!=0;
        const bool subtract=(GetAsyncKeyState(VK_SUBTRACT)&0x8000)!=0;
        const bool up=(GetAsyncKeyState(VK_NUMPAD8)&0x8000)!=0;
        const bool down=(GetAsyncKeyState(VK_NUMPAD2)&0x8000)!=0;
        if(IsGameForeground()){
            if(add&&!previousAdd)portal_v::calibration::AdjustCircle(0.10f,0.0f);
            if(subtract&&!previousSubtract)portal_v::calibration::AdjustCircle(-0.10f,0.0f);
            if(up&&!previousUp)portal_v::calibration::AdjustCircle(0.0f,0.05f);
            if(down&&!previousDown)portal_v::calibration::AdjustCircle(0.0f,-0.05f);
        }
        previousAdd=add;previousSubtract=subtract;previousUp=up;previousDown=down;
        const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10 && !previousF10 && IsWindowVisible(GetForegroundWindow())) {
            const Ped ped = PLAYER::PLAYER_PED_ID();
            Entity entity = ped;
            if (PED::IS_PED_IN_ANY_VEHICLE(ped, FALSE)) {
                entity = PED::GET_VEHICLE_PED_IS_IN(ped, FALSE);
            }
            if (ENTITY::DOES_ENTITY_EXIST(entity)) {
                const Vector3 spawn = ENTITY::GET_ENTITY_COORDS(entity, TRUE);
                g_spawnPosition[0] = spawn.x;
                g_spawnPosition[1] = spawn.y;
                g_spawnPosition[2] = spawn.z;
                g_spawnHeading = ENTITY::GET_ENTITY_HEADING(entity);
                g_spawnValid = std::isfinite(spawn.x) && std::isfinite(spawn.y) &&
                               std::isfinite(spawn.z) && std::isfinite(g_spawnHeading);
                if (g_spawnValid) {
                    SaveSpawn();
                    Log("PortalV development spawn saved (F10)");
                }
            }
        }
        previousF10 = f10;
        const auto portal = portal_v::calibration::GetSnapshot();
        // Visual-only billboard uses the vector from the portal centre to GTA V's
        // final rendered camera position. Camera look direction and player are irrelevant.
        constexpr float radiansToDegrees=57.295779513082320876f;
        const float runtimePortalYaw=std::atan2(
            -(renderedPosition.x-portal.center[0]),
              renderedPosition.y-portal.center[1])*radiansToDegrees+180.0f;
        portal_v::compositor::Pose pose{};
        pose.camera[0] = renderedPosition.x; pose.camera[1] = renderedPosition.y; pose.camera[2] = renderedPosition.z;
        pose.rotation[0] = renderedRotation.x; pose.rotation[1] = renderedRotation.y; pose.rotation[2] = renderedRotation.z;
        pose.fov = renderedFov;
        pose.nearClip = nearClip;
        pose.farClip = farClip;
        pose.planeCenter[0] = portal.center[0];
        pose.planeCenter[1] = portal.center[1];
        pose.planeCenter[2] = portal.center[2];
        pose.planeYaw = runtimePortalYaw;
        pose.planeSize[0] = portal.width; pose.planeSize[1] = portal.height;
        for (int i = 0; i < 4; ++i) pose.planeCrop[i] = portal.crop[i];
        pose.cornerRadius = portal.cornerRadius;
        pose.edgeFeather = portal.edgeFeather;
        pose.planeShape = static_cast<int>(portal.shape);
        const bool portalVisible = !g_state ||
            InterlockedCompareExchange(&g_state->portalVisible, 0, 0) != 0;
        pose.planeValid = portalVisible && portal.valid &&
                          std::isfinite(renderedFov) && renderedFov > 1.0f &&
                          renderedFov < 179.0f && std::isfinite(nearClip) &&
                          std::isfinite(farClip) && nearClip > 0.0f && farClip > nearClip;
        portal_v::compositor::SetPose(pose);
        if (g_state) {
            InterlockedIncrement(&g_state->sequence);
            g_state->heartbeatTickMs = tick;
            ++g_state->frameId;
            g_state->cameraPosition[0] = position.x;
            g_state->cameraPosition[1] = position.y;
            g_state->cameraPosition[2] = position.z;
            g_state->cameraRotation[0] = rotation.x;
            g_state->cameraRotation[1] = rotation.y;
            g_state->cameraRotation[2] = rotation.z;
            g_state->fovDegrees = fov;
            g_state->renderedCameraPosition[0] = renderedPosition.x;
            g_state->renderedCameraPosition[1] = renderedPosition.y;
            g_state->renderedCameraPosition[2] = renderedPosition.z;
            g_state->renderedCameraRotation[0] = renderedRotation.x;
            g_state->renderedCameraRotation[1] = renderedRotation.y;
            g_state->renderedCameraRotation[2] = renderedRotation.z;
            g_state->renderedFovDegrees = renderedFov;
            g_state->portalPlaneCenter[0] = portal.center[0];
            g_state->portalPlaneCenter[1] = portal.center[1];
            g_state->portalPlaneCenter[2] = portal.center[2];
            // Publish the locked calibration basis, not the visual billboard yaw.
            g_state->portalPlaneYawDegrees = portal.yawDegrees;
            g_state->portalPlaneValid = portal.valid ? 1u : 0u;
            g_state->cameraValid = std::isfinite(renderedFov) && renderedFov > 0.0f;
            InterlockedIncrement(&g_state->sequence);
            if (InterlockedCompareExchange(&g_state->overlayEnabled, 0, 0)) DrawTestPattern(tick);
        }
        WAIT(0);
    }
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        scriptRegister(module, ScriptMain);
    } else if (reason == DLL_PROCESS_DETACH) {
        portal_v::compositor::Unregister(module);
        scriptUnregister(module);
        if (g_state) UnmapViewOfFile(g_state);
        if (g_mapping) CloseHandle(g_mapping);
    }
    return TRUE;
}
