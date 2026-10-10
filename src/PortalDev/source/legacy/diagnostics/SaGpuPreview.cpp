#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "SaGpuPreview.h"

#include <d3d9.h>

#include <array>
#include <sstream>

#include "../../../../PortalProtocol.h"

namespace portal_dev::legacy::sa_gpu_preview {
namespace {

IDirect3D9Ex* g_d3d = nullptr;
IDirect3DDevice9Ex* g_device = nullptr;
IDirect3DQuery9* g_readQuery = nullptr;
std::array<IDirect3DTexture9*, portal::kSlotCount> g_sharedTextures{};
std::array<uint64_t, portal::kSlotCount> g_openHandles{};
LONG g_openGeneration = 0;
int g_readingSlot = -1;
uint64_t g_lastFrameId = 0;

void ReleaseSharedTextures() {
    for (int index = 0; index < portal::kSlotCount; ++index) {
        if (g_sharedTextures[index]) g_sharedTextures[index]->Release();
        g_sharedTextures[index] = nullptr;
        g_openHandles[index] = 0;
    }
}

bool StartPreviewDevice(portal::SharedFrames* frames, HWND previewWindow) {
    if (g_device) return true;
    if (!g_d3d && FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d))) return false;
    UINT adapter = D3DADAPTER_DEFAULT;
    bool matched = false;
    for (UINT index = 0; index < g_d3d->GetAdapterCount(); ++index) {
        LUID luid{};
        if (SUCCEEDED(g_d3d->GetAdapterLUID(index, &luid)) &&
            luid.LowPart == frames->adapterLuidLow && luid.HighPart == frames->adapterLuidHigh) {
            adapter = index;
            matched = true;
            break;
        }
    }
    if (!matched) return false;
    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth = 640;
    params.BackBufferHeight = 360;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    params.MultiSampleType = D3DMULTISAMPLE_NONE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = previewWindow;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT result = g_d3d->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, previewWindow,
                                            D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                            &params, nullptr, &g_device);
    if (SUCCEEDED(result)) result = g_device->CreateQuery(D3DQUERYTYPE_EVENT, &g_readQuery);
    if (FAILED(result)) {
        if (g_device) { g_device->Release(); g_device = nullptr; }
        return false;
    }
    return true;
}

} // namespace

void Pump(portal::SharedFrames* frames, HWND previewWindow) {
    if (!frames || frames->bridgeEnabled != 4 || !frames->producerPid) return;
    const LONG generation = InterlockedCompareExchange(&frames->generation, 0, 0);
    if (!generation) return;
    if (g_readingSlot >= 0) {
        const HRESULT done = g_readQuery->GetData(nullptr, 0, 0);
        if (done == S_FALSE) return;
        InterlockedCompareExchange(&frames->slots[g_readingSlot].state,
                                   portal::kFree, portal::kReading);
        g_readingSlot = -1;
        if (FAILED(done)) return;
    }
    if (g_openGeneration != generation) {
        ReleaseSharedTextures();
        g_openGeneration = generation;
        g_lastFrameId = 0;
    }
    if (!StartPreviewDevice(frames, previewWindow)) return;

    int newest = -1;
    for (int index = 0; index < portal::kSlotCount; ++index) {
        auto& slot = frames->slots[index];
        if (InterlockedCompareExchange(&slot.state, 0, 0) == portal::kReady &&
            slot.frameId > g_lastFrameId &&
            (newest < 0 || slot.frameId > frames->slots[newest].frameId)) newest = index;
    }
    if (newest < 0) return;
    auto& slot = frames->slots[newest];
    if (InterlockedCompareExchange(&slot.state, portal::kReading,
                                   portal::kReady) != portal::kReady) return;
    if (g_openHandles[newest] != slot.sharedHandle) {
        if (g_sharedTextures[newest]) g_sharedTextures[newest]->Release();
        g_sharedTextures[newest] = nullptr;
        HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(slot.sharedHandle));
        const HRESULT opened = g_device->CreateTexture(slot.width, slot.height, 1,
            D3DUSAGE_RENDERTARGET, static_cast<D3DFORMAT>(slot.format), D3DPOOL_DEFAULT,
            &g_sharedTextures[newest], &handle);
        if (FAILED(opened)) {
            InterlockedExchange(&slot.state, portal::kFree);
            return;
        }
        g_openHandles[newest] = slot.sharedHandle;
    }

    IDirect3DSurface9* source = nullptr;
    IDirect3DSurface9* target = nullptr;
    HRESULT result = g_sharedTextures[newest]->GetSurfaceLevel(0, &source);
    if (SUCCEEDED(result)) result = g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &target);
    if (SUCCEEDED(result)) result = g_device->StretchRect(source, nullptr, target, nullptr, D3DTEXF_LINEAR);
    if (source) source->Release();
    if (target) target->Release();
    if (SUCCEEDED(result)) result = g_readQuery->Issue(D3DISSUE_END);
    if (SUCCEEDED(result)) {
        g_readingSlot = newest;
        RECT destination{};
        GetClientRect(previewWindow, &destination);
        result = g_device->PresentEx(nullptr, &destination, nullptr, nullptr, 0);
    }
    if (FAILED(result)) {
        if (g_readingSlot < 0) InterlockedExchange(&slot.state, portal::kFree);
        return;
    }
    g_lastFrameId = slot.frameId;
}

void Stop(portal::SharedFrames* frames) {
    if (frames && g_readingSlot >= 0) {
        InterlockedCompareExchange(&frames->slots[g_readingSlot].state,
                                   portal::kFree, portal::kReading);
    }
    g_readingSlot = -1;
    ReleaseSharedTextures();
    if (g_readQuery) { g_readQuery->Release(); g_readQuery = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
    g_openGeneration = 0;
    g_lastFrameId = 0;
}

} // namespace portal_dev::legacy::sa_gpu_preview
