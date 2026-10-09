#include "D3D9ExBridge.h"

#include <d3d9.h>
#include <windows.h>
#include <cstdio>
#include <new>

#include <safetyhook.hpp>

#include "../../PortalProtocol.h"

namespace portal_sa {
namespace {

safetyhook::InlineHook g_create9Hook;
volatile LONG g_createdExDevice = 0;
LONG g_mode = 0;
thread_local bool g_insideCreate9 = false;
void (*g_trace)(const char*) = nullptr;

void Trace(const char* operation, HRESULT result) {
    if (!g_trace) return;
    char message[96]{};
    std::snprintf(message, sizeof(message), "D3D9Ex bridge %s hresult=0x%08X",
                  operation, static_cast<unsigned>(result));
    g_trace(message);
}

// The game asks for IDirect3D9. Forward its queries, but create a 9Ex device.
class D3D9Proxy final : public IDirect3D9 {
public:
    D3D9Proxy(IDirect3D9* legacy, IDirect3D9Ex* real) : legacy_(legacy), real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirect3D9)) {
            *result = this;
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(iid, result);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* function) override { return real_->RegisterSoftwareDevice(function); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return real_->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) override { return real_->GetAdapterIdentifier(adapter, flags, id); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT format) override { return real_->GetAdapterModeCount(adapter, format); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* output) override { return real_->EnumAdapterModes(adapter, format, mode, output); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* output) override { return real_->GetAdapterDisplayMode(adapter, output); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT backbuffer, BOOL windowed) override { return real_->CheckDeviceType(adapter, type, display, backbuffer, windowed); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT format) override { return real_->CheckDeviceFormat(adapter, type, display, usage, resource, format); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE sample, DWORD* quality) override { return real_->CheckDeviceMultiSampleType(adapter, type, format, windowed, sample, quality); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT render, D3DFORMAT depth) override { return real_->CheckDepthStencilMatch(adapter, type, display, render, depth); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT source, D3DFORMAT target) override { return real_->CheckDeviceFormatConversion(adapter, type, source, target); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) override { return real_->GetDeviceCaps(adapter, type, caps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override { return real_->GetAdapterMonitor(adapter); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                           D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** device) override {
        if (!device) return D3DERR_INVALIDCALL;
        *device = nullptr;
        if (g_trace && params) {
            char message[160]{};
            std::snprintf(message, sizeof(message),
                "D3D9Ex bridge CreateDevice adapter=%u windowed=%d backbuffer=%ux%u format=%u flags=0x%X",
                adapter, params->Windowed, params->BackBufferWidth, params->BackBufferHeight,
                static_cast<unsigned>(params->BackBufferFormat), flags);
            g_trace(message);
        }
        IDirect3DDevice9Ex* ex = nullptr;
        D3DDISPLAYMODEEX fullscreen{};
        fullscreen.Size = sizeof(fullscreen);
        D3DDISPLAYMODEEX* mode = nullptr;
        if (params && !params->Windowed) {
            D3DDISPLAYMODE current{};
            real_->GetAdapterDisplayMode(adapter, &current);
            fullscreen.Width = params->BackBufferWidth ? params->BackBufferWidth : current.Width;
            fullscreen.Height = params->BackBufferHeight ? params->BackBufferHeight : current.Height;
            fullscreen.RefreshRate = params->FullScreen_RefreshRateInHz ?
                params->FullScreen_RefreshRateInHz : current.RefreshRate;
            fullscreen.Format = params->BackBufferFormat != D3DFMT_UNKNOWN ?
                params->BackBufferFormat : current.Format;
            fullscreen.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            mode = &fullscreen;
        }
        const HRESULT result = real_->CreateDeviceEx(adapter, type, window, flags, params, mode, &ex);
        Trace("CreateDeviceEx", result);
        if (SUCCEEDED(result)) {
            *device = ex;
            InterlockedExchange(&g_createdExDevice, 1);
            return result;
        }
        const HRESULT fallback = legacy_->CreateDevice(adapter, type, window, flags, params, device);
        Trace("legacy CreateDevice fallback", fallback);
        if (SUCCEEDED(fallback)) {
            IDirect3DDevice9Ex* detected = nullptr;
            if (SUCCEEDED((*device)->QueryInterface(IID_IDirect3DDevice9Ex,
                                                    reinterpret_cast<void**>(&detected)))) {
                InterlockedExchange(&g_createdExDevice, 1);
                detected->Release();
            }
        }
        return fallback;
    }

private:
    ~D3D9Proxy() { real_->Release(); legacy_->Release(); }
    IDirect3D9* legacy_;
    IDirect3D9Ex* real_;
    volatile LONG refs_ = 1;
};

IDirect3D9* WINAPI OnDirect3DCreate9(UINT version) {
    if (g_insideCreate9) return g_create9Hook.stdcall<IDirect3D9*>(version);
    if (g_trace) g_trace("D3D9Ex bridge Direct3DCreate9 entered");
    g_insideCreate9 = true;
    IDirect3D9* legacy = g_create9Hook.stdcall<IDirect3D9*>(version);
    Trace("legacy Direct3DCreate9", legacy ? S_OK : E_FAIL);
    IDirect3D9Ex* ex = nullptr;
    const HRESULT result = legacy ? Direct3DCreate9Ex(version, &ex) : E_FAIL;
    g_insideCreate9 = false;
    Trace("Direct3DCreate9Ex", result);
    if (SUCCEEDED(result)) {
        if (auto* proxy = new (std::nothrow) D3D9Proxy(legacy, ex)) return proxy;
        ex->Release();
    }
    return legacy;
}

} // namespace

bool InstallD3D9ExBridge(void (*trace)(const char*)) {
    g_trace = trace;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, portal::kMappingName);
    if (!mapping) return false;
    const auto* frames = static_cast<const portal::SharedFrames*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(portal::SharedFrames)));
    const bool valid = frames && frames->magic == portal::kMagic &&
        frames->version == portal::kVersion && frames->size == sizeof(portal::SharedFrames) &&
        frames->bridgeEnabled != 0;
    g_mode = valid ? frames->bridgeEnabled : 0;
    if (frames) UnmapViewOfFile(frames);
    CloseHandle(mapping);
    if (!g_mode) return false;
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryW(L"d3d9.dll");
    if (!d3d9) return false;
    auto* create9 = GetProcAddress(d3d9, "Direct3DCreate9");
    if (!create9) return false;
    g_create9Hook = safetyhook::create_inline(reinterpret_cast<void*>(create9), &OnDirect3DCreate9);
    return static_cast<bool>(g_create9Hook);
}

bool CreatedExDevice() { return InterlockedCompareExchange(&g_createdExDevice, 0, 0) != 0; }
LONG BridgeMode() { return g_mode; }

} // namespace portal_sa
