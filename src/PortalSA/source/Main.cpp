#include <d3d9.h>
#include <windows.h>

#include <cwchar>
#include <string>

#include <plugin.h>
#include <common.h>
#include <safetyhook.hpp>

#include "StateCapture.h"
#include "CameraOverride.h"
#include "DevTeleport.h"
#include "VConsumer.h"
#include "PortalOverlay.h"
#include "legacy/diagnostics/D3D9ExBridge.h"
#include "legacy/diagnostics/SaGpuProducer.h"

namespace {

// GTA SA 1.0 US HOODLUM; plugin-sdk uses this address in RenderWare.cpp.
constexpr uintptr_t kShowRasterAddress = 0x7EE370;

bool g_initialized = false;
bool g_supported = false;
bool g_bridgeInstalled = false;
HANDLE g_log = INVALID_HANDLE_VALUE;
safetyhook::InlineHook g_showRasterHook;
safetyhook::InlineHook g_resetHook;

bool WriteAll(HANDLE file, const void* data, DWORD size) {
    DWORD written = 0;
    return WriteFile(file, data, size, &written, nullptr) && written == size;
}

void Log(const std::string& text) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    WriteAll(g_log, text.data(), static_cast<DWORD>(text.size()));
    static constexpr char newline[] = "\r\n";
    WriteAll(g_log, newline, 2);
}

void DiagnosticLog(const char* text) {
    Log(text ? text : "D3D9Ex diagnostic emitted a null message");
    // Startup crashes can happen before normal process shutdown. Force the small,
    // low-frequency diagnostic records to disk so the last completed stage survives.
    if (g_log != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log);
}

bool Initialize() {
    if (g_initialized) return g_supported;
    g_initialized = true;

    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 32) return false;
    wchar_t* separator = wcsrchr(directory, L'\\');
    if (!separator) return false;
    separator[1] = L'\0';
    wchar_t logPath[MAX_PATH]{};
    wcscpy_s(logPath, directory);
    wcscat_s(logPath, L"portal.log");
    g_log = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_supported = plugin::GetGameVersion() == GAME_10US_HOODLUM;
    Log(g_supported ? "PortalSA loaded: GTA SA 1.0 US HOODLUM"
                    : "PortalSA disabled: unsupported GTA SA executable");
    return g_supported;
}

void OnGameProcess() {
    if (!Initialize()) return;
    portal_sa::state::OnGameProcess(Log);
    portal_sa::dev_teleport::OnGameProcess(Log);
    portal_sa::camera_override::OnGameProcess(Log);
    portal_sa::v_consumer::OnGameProcess([](const char* message) { Log(message); });
}

void OnRenderScene() {
    if (!g_supported) return;
    portal_sa::state::OnRenderScene();
}

void OnShutdownRw() {
    portal_sa::camera_override::Shutdown();
    portal_sa::portal_overlay::Shutdown();
}

HRESULT STDMETHODCALLTYPE OnReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    portal_sa::v_consumer::BeforeReset();
    portal_sa::portal_overlay::BeforeReset();
    portal_sa::legacy::sa_gpu_producer::BeforeReset(device);
    const HRESULT result=g_resetHook.stdcall<HRESULT>(device, params);
    if(SUCCEEDED(result))portal_sa::portal_overlay::AfterReset();
    return result;
}

RwCamera* __cdecl OnShowRaster(RwCamera* camera, void* window, RwUInt32 flags) {
    auto* device=static_cast<IDirect3DDevice9*>(GetD3DDevice());
    // Composite after SA's world, glare and color passes but immediately before Present.
    portal_sa::v_consumer::OnRenderScene(device,[](const char* message){Log(message);});
    portal_sa::portal_overlay::Render(device);
    portal_sa::legacy::sa_gpu_producer::BeforeShowRaster(
        device);
    RwCamera* result = g_showRasterHook.ccall<RwCamera*>(camera, window, flags);
    portal_sa::legacy::sa_gpu_producer::AfterShowRaster();
    return result;
}

void OnInitRw() {
    if (!Initialize() || g_showRasterHook) return;
    DiagnosticLog("PortalSA initRwEvent entered");
    g_showRasterHook = safetyhook::create_inline(reinterpret_cast<void*>(kShowRasterAddress),
                                                 &OnShowRaster);
    Log(g_showRasterHook ? "Render output hook installed at RwCameraShowRaster"
                         : "Render output hook failed at RwCameraShowRaster");
    Log(g_bridgeInstalled ? "D3D9Ex creation bridge installed"
                          : "D3D9Ex bridge disabled or unavailable");
    Log(portal_sa::CreatedExDevice() ? "D3D9Ex device created" : "D3D9Ex device not observed");

    // Mode 1 is hook-only, mode 2 is a legacy forwarding proxy, mode 3 creates
    // the Ex device, and mode 4 additionally enables the archived producer.
    portal_sa::legacy::sa_gpu_producer::Configure(
        g_bridgeInstalled && portal_sa::BridgeMode() == 4,
        [](const char* message) { Log(message); });

    if (auto* device = static_cast<IDirect3DDevice9*>(GetD3DDevice())) {
        auto** vtable = *reinterpret_cast<void***>(device);
        g_resetHook = safetyhook::create_inline(vtable[16], &OnReset);
        Log(g_resetHook ? "D3D9 Reset hook installed" : "D3D9 Reset hook failed");
    }
}

struct PortalSA {
    PortalSA() {
        if (plugin::GetGameVersion() != GAME_10US_HOODLUM) return;
        Initialize();
        g_bridgeInstalled = portal_sa::InstallD3D9ExBridge(DiagnosticLog);
        Log(g_bridgeInstalled ? "PortalSA startup: D3D9Ex diagnostic stage 3 (Ex device, no producer)"
                              : "PortalSA startup: native D3D9 mode");
        plugin::Events::initRwEvent += OnInitRw;
        plugin::Events::gameProcessEvent += OnGameProcess;
        plugin::Events::renderSceneEvent += OnRenderScene;
        plugin::Events::shutdownRwEvent += OnShutdownRw;
    }
} g_portalSA;

} // namespace
