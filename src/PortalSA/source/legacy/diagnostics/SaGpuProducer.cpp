#define WIN32_LEAN_AND_MEAN
#include "SaGpuProducer.h"

#include <d3d9.h>
#include <windows.h>

#include <array>
#include <cstring>
#include <sstream>

#include "../../StateCapture.h"
#include "../../../../PortalProtocol.h"

namespace portal_sa::legacy::sa_gpu_producer {
namespace {

constexpr DWORD kErrorLogIntervalMs = 5000;

struct GpuSlot {
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DQuery9* ready = nullptr;
    bool pending = false;
};

bool g_enabled = false;
LogFunction g_log = nullptr;
HANDLE g_mapping = nullptr;
portal::SharedFrames* g_frames = nullptr;
IDirect3DDevice9* g_captureDevice = nullptr;
UINT g_captureWidth = 0;
UINT g_captureHeight = 0;
uint64_t g_frameId = 0;
uint64_t g_lastPublishedMatrixSerial = 0;
DWORD g_lastErrorLogTick = 0;
DWORD g_lastCreateAttemptTick = 0;
LARGE_INTEGER g_qpcFrequency{};
std::array<GpuSlot, portal::kSlotCount> g_gpuSlots{};

void Log(const std::string& text) {
    if (g_log) g_log(text.c_str());
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
    line << "legacy SA GPU producer operation=" << operation << " hresult=0x"
         << std::hex << static_cast<unsigned>(result);
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
    auto* frames = static_cast<portal::SharedFrames*>(MapViewOfFile(
        mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(portal::SharedFrames)));
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
    Log("Legacy SA GPU producer mapping attached");
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
    line << "Legacy SA GPU producer textures ready width=" << desc.Width
         << " height=" << desc.Height << " format=" << static_cast<unsigned>(desc.Format);
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
    const auto& state = portal_sa::state::g_state;
    slot.frameId = ++g_frameId;
    slot.captureQpc = Qpc();
    slot.cameraValid = state.cameraValid &&
        portal_sa::state::g_renderMatrixSerial != g_lastPublishedMatrixSerial;
    slot.stateAgeMs = state.tick ? GetTickCount() - state.tick : 0;
    slot.cameraPosition[0] = state.cameraPosition.x;
    slot.cameraPosition[1] = state.cameraPosition.y;
    slot.cameraPosition[2] = state.cameraPosition.z;
    slot.cameraForward[0] = state.cameraForward.x;
    slot.cameraForward[1] = state.cameraForward.y;
    slot.cameraForward[2] = state.cameraForward.z;
    slot.cameraUp[0] = state.cameraUp.x;
    slot.cameraUp[1] = state.cameraUp.y;
    slot.cameraUp[2] = state.cameraUp.z;
    slot.fovDegrees = state.fovDegrees;
    slot.nearClip = state.nearClip;
    slot.farClip = state.farClip;
    slot.viewWindowX = state.viewWindowX;
    slot.viewWindowY = state.viewWindowY;
    if (slot.cameraValid) {
        std::memcpy(slot.view, portal_sa::state::g_viewMatrix.data(), sizeof(slot.view));
        std::memcpy(slot.projection, portal_sa::state::g_projectionMatrix.data(), sizeof(slot.projection));
    }
    g_lastPublishedMatrixSerial = portal_sa::state::g_renderMatrixSerial;
    result = device->StretchRect(backBuffer, nullptr, g_gpuSlots[index].surface, nullptr, D3DTEXF_NONE);
    backBuffer->Release();
    if (SUCCEEDED(result)) result = g_gpuSlots[index].ready->Issue(D3DISSUE_END);
    slot.submitUs = static_cast<uint32_t>(
        1000000 * (Qpc() - slot.captureQpc) / g_qpcFrequency.QuadPart);
    if (SUCCEEDED(result)) {
        g_gpuSlots[index].pending = true;
    } else {
        InterlockedExchange(&slot.state, portal::kFree);
        GpuError("gpu_copy", result);
    }
}

} // namespace

void Configure(bool enabled, LogFunction log) {
    g_enabled = enabled;
    g_log = log;
    QueryPerformanceFrequency(&g_qpcFrequency);
    if (g_enabled) AttachFrames();
}

void BeforeReset(IDirect3DDevice9* device) {
    if (!g_enabled || device != g_captureDevice) return;
    ReleaseGpuSlots();
    g_captureDevice = nullptr;
    if (g_frames) InterlockedIncrement(&g_frames->generation);
}

void BeforeShowRaster(IDirect3DDevice9* device) {
    if (!g_enabled || !device || !AttachFrames()) return;
    CompleteGpuSlots();
    PublishGpuFrame(device);
}

void AfterShowRaster() {
    if (g_enabled) CompleteGpuSlots();
}

} // namespace portal_sa::legacy::sa_gpu_producer
