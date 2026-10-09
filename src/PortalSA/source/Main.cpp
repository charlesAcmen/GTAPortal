#include <d3d9.h>
#include <windows.h>

#include <array>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>

#include <plugin.h>
#include <CCamera.h>
#include <CCutsceneMgr.h>
#include <CLoadingScreen.h>
#include <CMenuManager.h>
#include <CPlayerPed.h>
#include <common.h>
#include <safetyhook.hpp>

#include "D3D9ExBridge.h"
#include "../../PortalProtocol.h"

namespace {

constexpr DWORD kStateLogIntervalMs = 100;
constexpr DWORD kErrorLogIntervalMs = 5000;
// GTA SA 1.0 US HOODLUM; plugin-sdk uses this address in RenderWare.cpp.
constexpr uintptr_t kShowRasterAddress = 0x7EE370;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 Copy(const CVector& value) {
    return {value.x, value.y, value.z};
}

struct State {
    DWORD tick = 0;
    bool playerValid = false;
    bool cameraValid = false;
    bool velocityFromVehicle = false;
    Vec3 playerPosition;
    Vec3 playerVelocity;
    float playerHeading = 0.0f;
    Vec3 cameraPosition;
    Vec3 cameraForward;
    Vec3 cameraUp;
    float fovDegrees = 0.0f;
    float nearClip = 0.0f;
    float farClip = 0.0f;
    float viewWindowX = 0.0f;
    float viewWindowY = 0.0f;
    unsigned cameraIndex = 0;
};

State g_state;
std::array<float, 16> g_viewMatrix{};
std::array<float, 16> g_projectionMatrix{};
DWORD g_renderMatrixTick = 0;
uint64_t g_renderMatrixSerial = 0;
uint64_t g_lastPublishedMatrixSerial = 0;
DWORD g_lastStateLogTick = 0;
DWORD g_lastErrorLogTick = 0;
bool g_initialized = false;
bool g_supported = false;
bool g_bridgeInstalled = false;
HANDLE g_log = INVALID_HANDLE_VALUE;
wchar_t g_logPath[MAX_PATH]{};
safetyhook::InlineHook g_showRasterHook;
safetyhook::InlineHook g_resetHook;
HANDLE g_mapping = nullptr;
portal::SharedFrames* g_frames = nullptr;
IDirect3DDevice9* g_captureDevice = nullptr;
UINT g_captureWidth = 0;
UINT g_captureHeight = 0;
uint64_t g_frameId = 0;
DWORD g_lastCreateAttemptTick = 0;
LARGE_INTEGER g_qpcFrequency{};

struct GpuSlot {
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DQuery9* ready = nullptr;
    bool pending = false;
};

std::array<GpuSlot, portal::kSlotCount> g_gpuSlots{};

bool WriteAll(HANDLE file, const void* data, DWORD size) {
    DWORD written = 0;
    return WriteFile(file, data, size, &written, nullptr) && written == size;
}

void Log(const std::string& text) {
    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteAll(g_log, text.data(), static_cast<DWORD>(text.size()));
    static constexpr char newline[] = "\r\n";
    WriteAll(g_log, newline, 2);
}

bool Initialize() {
    if (g_initialized) {
        return g_supported;
    }
    g_initialized = true;

    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 32) {
        return false;
    }
    wchar_t* separator = wcsrchr(directory, L'\\');
    if (!separator) {
        return false;
    }
    separator[1] = L'\0';
    wcscpy_s(g_logPath, directory);
    wcscat_s(g_logPath, L"portal.log");
    g_log = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    QueryPerformanceFrequency(&g_qpcFrequency);
    g_supported = plugin::GetGameVersion() == GAME_10US_HOODLUM;
    Log(g_supported ? "PortalSA loaded: GTA SA 1.0 US HOODLUM"
                    : "PortalSA disabled: unsupported GTA SA executable");
    return g_supported;
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

void OnGameProcess() {
    if (!Initialize()) {
        return;
    }

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
    Log(line.str());
}

void OnRenderScene() {
    if (!g_supported) {
        return;
    }
    const _D3DMATRIX* view = GetD3DViewTransform();
    const _D3DMATRIX* projection = GetD3DProjTransform();
    if (view && projection) {
        std::memcpy(g_viewMatrix.data(), view, sizeof(_D3DMATRIX));
        std::memcpy(g_projectionMatrix.data(), projection, sizeof(_D3DMATRIX));
        g_renderMatrixTick = GetTickCount();
        ++g_renderMatrixSerial;
    }
}

int64_t Qpc() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

void GpuError(const char* operation, HRESULT result) {
    if (g_frames) InterlockedIncrement(&g_frames->errors);
    const DWORD now = GetTickCount();
    if (now - g_lastErrorLogTick < kErrorLogIntervalMs) return;
    g_lastErrorLogTick = now;
    std::ostringstream line;
    line << "gpu operation=" << operation << " hresult=0x" << std::hex << static_cast<unsigned>(result);
    Log(line.str());
}

void ReleaseGpuSlots() {
    for (int index = 0; index < portal::kSlotCount; ++index) {
        auto& gpu = g_gpuSlots[index];
        if (gpu.ready) gpu.ready->Release();
        if (gpu.surface) gpu.surface->Release();
        if (gpu.texture) gpu.texture->Release();
        gpu = {};
        if (g_frames) {
            auto& slot = g_frames->slots[index];
            InterlockedExchange(&slot.state, portal::kFree);
            slot.sharedHandle = 0;
        }
    }
    g_captureWidth = 0;
    g_captureHeight = 0;
}

bool AttachFrames() {
    if (g_frames) return true;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, portal::kMappingName);
    if (!mapping) return false;
    auto* frames = static_cast<portal::SharedFrames*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(portal::SharedFrames)));
    if (!frames || frames->magic != portal::kMagic || frames->version != portal::kVersion ||
        frames->size != sizeof(portal::SharedFrames)) {
        if (frames) UnmapViewOfFile(frames);
        CloseHandle(mapping);
        return false;
    }
    g_mapping = mapping;
    g_frames = frames;
    if (g_frames->producerPid && g_frames->producerPid != GetCurrentProcessId()) {
        for (auto& slot : g_frames->slots) {
            InterlockedExchange(&slot.state, portal::kFree);
            slot.sharedHandle = 0;
        }
        InterlockedIncrement(&g_frames->generation);
    }
    InterlockedExchange(&g_frames->producerPid, static_cast<LONG>(GetCurrentProcessId()));
    Log("GPU frame mapping attached");
    return true;
}

bool CreateGpuSlots(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc) {
    if (!g_frames) return false;
    for (auto& slot : g_frames->slots) {
        if (InterlockedCompareExchange(&slot.state, 0, 0) == portal::kReading) return false;
    }
    ReleaseGpuSlots();

    IDirect3DDevice9Ex* exDevice = nullptr;
    HRESULT result = device->QueryInterface(IID_IDirect3DDevice9Ex, reinterpret_cast<void**>(&exDevice));
    if (FAILED(result)) {
        GpuError("device_is_not_D3D9Ex", result);
        return false;
    }
    exDevice->Release();

    IDirect3D9* d3d = nullptr;
    IDirect3D9Ex* d3dEx = nullptr;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    LUID luid{};
    result = device->GetDirect3D(&d3d);
    if (SUCCEEDED(result)) result = d3d->QueryInterface(IID_IDirect3D9Ex, reinterpret_cast<void**>(&d3dEx));
    if (SUCCEEDED(result)) result = device->GetCreationParameters(&creation);
    if (SUCCEEDED(result)) result = d3dEx->GetAdapterLUID(creation.AdapterOrdinal, &luid);
    if (d3dEx) d3dEx->Release();
    if (d3d) d3d->Release();
    if (FAILED(result)) {
        GpuError("adapter_LUID", result);
        return false;
    }

    for (int index = 0; index < portal::kSlotCount; ++index) {
        auto& gpu = g_gpuSlots[index];
        HANDLE handle = nullptr;
        result = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
                                       desc.Format, D3DPOOL_DEFAULT, &gpu.texture, &handle);
        if (SUCCEEDED(result)) result = gpu.texture->GetSurfaceLevel(0, &gpu.surface);
        if (SUCCEEDED(result)) result = device->CreateQuery(D3DQUERYTYPE_EVENT, &gpu.ready);
        if (FAILED(result) || !handle) {
            ReleaseGpuSlots();
            GpuError("create_shared_texture", FAILED(result) ? result : E_FAIL);
            return false;
        }
        auto& slot = g_frames->slots[index];
        slot.sharedHandle = reinterpret_cast<uintptr_t>(handle);
        slot.width = desc.Width;
        slot.height = desc.Height;
        slot.format = static_cast<uint32_t>(desc.Format);
    }
    g_captureDevice = device;
    g_captureWidth = desc.Width;
    g_captureHeight = desc.Height;
    g_frames->adapterLuidLow = luid.LowPart;
    g_frames->adapterLuidHigh = luid.HighPart;
    InterlockedIncrement(&g_frames->generation);
    std::ostringstream line;
    line << "GPU shared textures ready width=" << desc.Width << " height=" << desc.Height
         << " format=" << static_cast<unsigned>(desc.Format);
    Log(line.str());
    return true;
}

void CompleteGpuSlots() {
    if (!g_frames) return;
    for (int index = 0; index < portal::kSlotCount; ++index) {
        auto& gpu = g_gpuSlots[index];
        if (!gpu.pending) continue;
        const HRESULT result = gpu.ready->GetData(nullptr, 0, 0);
        if (result == S_FALSE) continue;
        gpu.pending = false;
        auto& slot = g_frames->slots[index];
        if (SUCCEEDED(result)) {
            slot.readyQpc = Qpc();
            MemoryBarrier();
            InterlockedExchange(&slot.state, portal::kReady);
            InterlockedIncrement(&g_frames->produced);
        } else {
            InterlockedExchange(&slot.state, portal::kFree);
            GpuError("gpu_completion", result);
        }
    }
}

int ClaimGpuSlot() {
    for (int index = 0; index < portal::kSlotCount; ++index) {
        if (InterlockedCompareExchange(&g_frames->slots[index].state, portal::kWriting,
                                       portal::kFree) == portal::kFree) return index;
    }
    int oldest = -1;
    for (int index = 0; index < portal::kSlotCount; ++index) {
        auto& slot = g_frames->slots[index];
        if (InterlockedCompareExchange(&slot.state, 0, 0) == portal::kReady &&
            (oldest < 0 || slot.frameId < g_frames->slots[oldest].frameId)) oldest = index;
    }
    if (oldest >= 0 && InterlockedCompareExchange(&g_frames->slots[oldest].state,
                                                   portal::kWriting, portal::kReady) == portal::kReady) {
        InterlockedIncrement(&g_frames->dropped);
        return oldest;
    }
    InterlockedIncrement(&g_frames->dropped);
    return -1;
}

void PublishGpuFrame(IDirect3DDevice9* device) {
    IDirect3DSurface9* backBuffer = nullptr;
    HRESULT result = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(result)) { GpuError("get_backbuffer", result); return; }
    D3DSURFACE_DESC desc{};
    result = backBuffer->GetDesc(&desc);
    if (FAILED(result) || (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)) {
        backBuffer->Release();
        GpuError("backbuffer_format", FAILED(result) ? result : D3DERR_INVALIDCALL);
        return;
    }
    if (g_captureDevice != device || g_captureWidth != desc.Width || g_captureHeight != desc.Height) {
        const DWORD now = GetTickCount();
        if (now - g_lastCreateAttemptTick < 2000) { backBuffer->Release(); return; }
        g_lastCreateAttemptTick = now;
        if (!CreateGpuSlots(device, desc)) { backBuffer->Release(); return; }
    }
    const int index = ClaimGpuSlot();
    if (index < 0) { backBuffer->Release(); return; }
    auto& slot = g_frames->slots[index];
    slot.frameId = ++g_frameId;
    slot.captureQpc = Qpc();
    slot.cameraValid = g_state.cameraValid && g_renderMatrixSerial != g_lastPublishedMatrixSerial;
    slot.stateAgeMs = g_state.tick ? GetTickCount() - g_state.tick : 0;
    slot.cameraPosition[0] = g_state.cameraPosition.x;
    slot.cameraPosition[1] = g_state.cameraPosition.y;
    slot.cameraPosition[2] = g_state.cameraPosition.z;
    slot.cameraForward[0] = g_state.cameraForward.x;
    slot.cameraForward[1] = g_state.cameraForward.y;
    slot.cameraForward[2] = g_state.cameraForward.z;
    slot.cameraUp[0] = g_state.cameraUp.x;
    slot.cameraUp[1] = g_state.cameraUp.y;
    slot.cameraUp[2] = g_state.cameraUp.z;
    slot.fovDegrees = g_state.fovDegrees;
    slot.nearClip = g_state.nearClip;
    slot.farClip = g_state.farClip;
    slot.viewWindowX = g_state.viewWindowX;
    slot.viewWindowY = g_state.viewWindowY;
    if (slot.cameraValid) {
        std::memcpy(slot.view, g_viewMatrix.data(), sizeof(slot.view));
        std::memcpy(slot.projection, g_projectionMatrix.data(), sizeof(slot.projection));
    }
    g_lastPublishedMatrixSerial = g_renderMatrixSerial;
    result = device->StretchRect(backBuffer, nullptr, g_gpuSlots[index].surface, nullptr, D3DTEXF_NONE);
    backBuffer->Release();
    if (SUCCEEDED(result)) result = g_gpuSlots[index].ready->Issue(D3DISSUE_END);
    slot.submitUs = static_cast<uint32_t>(1000000 * (Qpc() - slot.captureQpc) / g_qpcFrequency.QuadPart);
    if (SUCCEEDED(result)) {
        g_gpuSlots[index].pending = true;
    } else {
        InterlockedExchange(&slot.state, portal::kFree);
        GpuError("gpu_copy", result);
    }
}

HRESULT STDMETHODCALLTYPE OnReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    if (device == g_captureDevice) {
        ReleaseGpuSlots();
        g_captureDevice = nullptr;
        if (g_frames) InterlockedIncrement(&g_frames->generation);
    }
    return g_resetHook.stdcall<HRESULT>(device, params);
}

RwCamera* __cdecl OnShowRaster(RwCamera* camera, void* window, RwUInt32 flags) {
    if (g_bridgeInstalled && AttachFrames()) {
        CompleteGpuSlots();
        if (auto* device = static_cast<IDirect3DDevice9*>(GetD3DDevice())) {
            if (!g_resetHook) {
                auto** vtable = *reinterpret_cast<void***>(device);
                g_resetHook = safetyhook::create_inline(vtable[16], &OnReset);
            }
            PublishGpuFrame(device);
        }
    }
    RwCamera* result = g_showRasterHook.ccall<RwCamera*>(camera, window, flags);
    CompleteGpuSlots();
    return result;
}

void OnInitRw() {
    if (!Initialize() || g_showRasterHook) {
        return;
    }
    g_showRasterHook = safetyhook::create_inline(reinterpret_cast<void*>(kShowRasterAddress),
                                                 &OnShowRaster);
    Log(g_showRasterHook ? "Render output hook installed at RwCameraShowRaster"
                         : "Render output hook failed at RwCameraShowRaster");
    Log(g_bridgeInstalled ? "D3D9Ex creation bridge installed" : "D3D9Ex bridge disabled or unavailable");
    Log(portal_sa::CreatedExDevice() ? "D3D9Ex device created" : "D3D9Ex device not observed");
    if (g_bridgeInstalled) AttachFrames();
    if (g_bridgeInstalled) {
        if (auto* device = static_cast<IDirect3DDevice9*>(GetD3DDevice())) {
            auto** vtable = *reinterpret_cast<void***>(device);
            g_resetHook = safetyhook::create_inline(vtable[16], &OnReset);
            Log(g_resetHook ? "D3D9 Reset hook installed" : "D3D9 Reset hook failed");
        }
    }
}

struct PortalSA {
    PortalSA() {
        if (plugin::GetGameVersion() != GAME_10US_HOODLUM) {
            return;
        }
        Initialize();
        Log("PortalSA startup: native D3D9 mode");
        plugin::Events::initRwEvent += OnInitRw;
        plugin::Events::gameProcessEvent += OnGameProcess;
        plugin::Events::renderSceneEvent += OnRenderScene;
    }
} g_portalSA;

} // namespace
