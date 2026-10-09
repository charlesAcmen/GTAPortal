#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <main.h>
#include <types.h>
#include <nativeCaller.h>

#include <cmath>
#include <cstdio>

#include "../../PortalVProtocol.h"
#include "Compositor.h"

namespace {
HMODULE g_module{};
HANDLE g_mapping{};
portal_v::SharedState* g_state{};
float g_planeCenter[3]{};
float g_planeYaw{};
bool g_planeValid{};

bool GameFilePath(const wchar_t* name, wchar_t (&path)[MAX_PATH]) {
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), name);
    return true;
}

void LoadPlane() {
    wchar_t path[MAX_PATH]{};
    if (!GameFilePath(L"PortalVPlane.txt", path)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"r") != 0 || !file) return;
    g_planeValid = fscanf_s(file, "%f %f %f %f", &g_planeCenter[0], &g_planeCenter[1],
                            &g_planeCenter[2], &g_planeYaw) == 4;
    fclose(file);
}

void SavePlane() {
    wchar_t path[MAX_PATH]{};
    if (!GameFilePath(L"PortalVPlane.txt", path)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w") != 0 || !file) return;
    fprintf(file, "%.6f %.6f %.6f %.6f\n", g_planeCenter[0], g_planeCenter[1],
            g_planeCenter[2], g_planeYaw);
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
    LoadPlane();
    if (!g_state) Log("PortalV state mapping unavailable");
    uint64_t nextRegister = 0;
    bool registered = false;
    bool previousF9 = false;
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
        const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !previousF9 && std::isfinite(renderedPosition.x) && std::isfinite(renderedRotation.z)) {
            // Anchor in world coordinates once; subsequent camera motion must not move the plane.
            const float yaw = renderedRotation.z * 0.01745329252f;
            g_planeCenter[0] = renderedPosition.x - std::sin(yaw) * 3.0f;
            g_planeCenter[1] = renderedPosition.y + std::cos(yaw) * 3.0f;
            g_planeCenter[2] = renderedPosition.z;
            g_planeYaw = renderedRotation.z;
            g_planeValid = true;
            SavePlane();
            Log("PortalV SA plane anchored 3m in front of camera (F9)");
        }
        previousF9 = f9;
        portal_v::compositor::Pose pose{};
        pose.camera[0] = renderedPosition.x; pose.camera[1] = renderedPosition.y; pose.camera[2] = renderedPosition.z;
        pose.rotation[0] = renderedRotation.x; pose.rotation[1] = renderedRotation.y; pose.rotation[2] = renderedRotation.z;
        pose.fov = renderedFov;
        pose.nearClip = nearClip;
        pose.farClip = farClip;
        pose.planeCenter[0] = g_planeCenter[0];
        pose.planeCenter[1] = g_planeCenter[1];
        pose.planeCenter[2] = g_planeCenter[2];
        pose.planeYaw = g_planeYaw;
        pose.planeValid = g_planeValid && std::isfinite(renderedFov) && renderedFov > 1.0f &&
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
            g_state->cameraValid = std::isfinite(fov) && fov > 0.0f;
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
