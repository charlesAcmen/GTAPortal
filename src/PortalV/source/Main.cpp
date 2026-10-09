#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <main.h>
#include <types.h>
#include <nativeCaller.h>

#include <cmath>
#include <cstdio>

#include "../../PortalVProtocol.h"

namespace {
HMODULE g_module{};
HANDLE g_mapping{};
portal_v::SharedState* g_state{};

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
    g_state->overlayEnabled = 1;
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
    if (!g_state) Log("PortalV state mapping unavailable");
    while (true) {
        const uint64_t tick = GetTickCount64();
        // Native hashes are supplied by the user's Script Hook V SDK.
        const Vector3 position = invoke<Vector3>(0x14D6F5678D8F1B37);
        const Vector3 rotation = invoke<Vector3>(0x837765A25378F0BB, 2);
        const float fov = invoke<float>(0x65019750A0324133);
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
        } else {
            DrawTestPattern(tick);
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
        scriptUnregister(module);
        if (g_state) UnmapViewOfFile(g_state);
        if (g_mapping) CloseHandle(g_mapping);
    }
    return TRUE;
}
