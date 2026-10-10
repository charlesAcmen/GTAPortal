#include "D3D9ExBridge.h"

#include <d3d9.h>
#include <windows.h>
#include <cstdio>
#include <new>

#include <safetyhook.hpp>

#include "../../../../PortalProtocol.h"

namespace portal_sa {
namespace {

safetyhook::InlineHook g_create9Hook;
volatile LONG g_createdExDevice = 0;
volatile LONG g_exceptionProbeInstalled = 0;
volatile LONG g_exceptionProbeLogged = 0;
LONG g_mode = 0;
thread_local bool g_insideCreate9 = false;
void (*g_trace)(const char*) = nullptr;
void TraceMessage(const char* message);

LONG CALLBACK ExceptionProbe(EXCEPTION_POINTERS* exception) {
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        InterlockedCompareExchange(&g_exceptionProbeLogged, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto* record = exception->ExceptionRecord;
    const auto* context = exception->ContextRecord;
    const ULONG_PTR operation = record->NumberParameters > 0 ? record->ExceptionInformation[0] : 0;
    const ULONG_PTR target = record->NumberParameters > 1 ? record->ExceptionInformation[1] : 0;
    char message[512]{};
#if defined(_M_IX86)
    std::snprintf(message, sizeof(message),
        "D3D9Ex bridge exception probe code=0x%08lX address=%p operation=%llu target=%p "
        "eip=%08lX eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX "
        "ebp=%08lX esp=%08lX",
        record->ExceptionCode, record->ExceptionAddress,
        static_cast<unsigned long long>(operation), reinterpret_cast<void*>(target),
        context->Eip, context->Eax, context->Ebx, context->Ecx, context->Edx,
        context->Esi, context->Edi, context->Ebp, context->Esp);
#else
    std::snprintf(message, sizeof(message),
        "D3D9Ex bridge exception probe code=0x%08lX address=%p operation=%llu target=%p",
        record->ExceptionCode, record->ExceptionAddress,
        static_cast<unsigned long long>(operation), reinterpret_cast<void*>(target));
#endif
    TraceMessage(message);
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallExceptionProbe() {
    if (InterlockedCompareExchange(&g_exceptionProbeInstalled, 1, 0) != 0) return;
    if (AddVectoredExceptionHandler(1, ExceptionProbe)) {
        TraceMessage("D3D9Ex bridge post-device exception probe installed");
    } else {
        InterlockedExchange(&g_exceptionProbeInstalled, 0);
        TraceMessage("D3D9Ex bridge post-device exception probe install failed");
    }
}

void Trace(const char* operation, HRESULT result) {
    if (!g_trace) return;
    char message[96]{};
    std::snprintf(message, sizeof(message), "D3D9Ex bridge %s hresult=0x%08X",
                  operation, static_cast<unsigned>(result));
    g_trace(message);
}

void TraceMessage(const char* message) {
    if (g_trace) g_trace(message);
}

void TracePresentParameters(const char* stage, const D3DPRESENT_PARAMETERS* params) {
    if (!g_trace || !params) return;
    char message[512]{};
    std::snprintf(message, sizeof(message),
        "D3D9Ex bridge %s windowed=%d deviceWindow=%p size=%ux%u format=%u "
        "backBuffers=%u swap=%u msaa=%u quality=%lu depth=%d depthFormat=%u "
        "flags=0x%lX refresh=%u interval=0x%X",
        stage, params->Windowed, static_cast<void*>(params->hDeviceWindow),
        params->BackBufferWidth, params->BackBufferHeight,
        static_cast<unsigned>(params->BackBufferFormat), params->BackBufferCount,
        static_cast<unsigned>(params->SwapEffect),
        static_cast<unsigned>(params->MultiSampleType), params->MultiSampleQuality,
        params->EnableAutoDepthStencil, static_cast<unsigned>(params->AutoDepthStencilFormat),
        params->Flags, params->FullScreen_RefreshRateInHz,
        params->PresentationInterval);
    g_trace(message);
}

// Stage 2 wraps the original IDirect3D9 while forwarding every operation to it.
// Later stages attach a D3D9Ex backend and change only CreateDevice.
class D3D9Proxy final : public IDirect3D9 {
public:
    D3D9Proxy(IDirect3D9* legacy, IDirect3D9Ex* ex = nullptr) : legacy_(legacy), ex_(ex) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirect3D9)) {
            *result = this;
            AddRef();
            return S_OK;
        }
        // Keep the factory behavior legacy-compatible even in stage 3. Only
        // CreateDevice changes, so a failure has one narrow cause.
        return legacy_->QueryInterface(iid, result);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* function) override { return legacy_->RegisterSoftwareDevice(function); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return legacy_->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) override { return legacy_->GetAdapterIdentifier(adapter, flags, id); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT format) override { return legacy_->GetAdapterModeCount(adapter, format); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* output) override { return legacy_->EnumAdapterModes(adapter, format, mode, output); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* output) override { return legacy_->GetAdapterDisplayMode(adapter, output); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT backbuffer, BOOL windowed) override { return legacy_->CheckDeviceType(adapter, type, display, backbuffer, windowed); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT format) override { return legacy_->CheckDeviceFormat(adapter, type, display, usage, resource, format); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE sample, DWORD* quality) override { return legacy_->CheckDeviceMultiSampleType(adapter, type, format, windowed, sample, quality); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT render, D3DFORMAT depth) override { return legacy_->CheckDepthStencilMatch(adapter, type, display, render, depth); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT source, D3DFORMAT target) override { return legacy_->CheckDeviceFormatConversion(adapter, type, source, target); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) override { return legacy_->GetDeviceCaps(adapter, type, caps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override { return legacy_->GetAdapterMonitor(adapter); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                           D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** device) override {
        if (!device) return D3DERR_INVALIDCALL;
        *device = nullptr;
        if (g_trace) {
            char message[160]{};
            std::snprintf(message, sizeof(message),
                "D3D9Ex bridge CreateDevice entered adapter=%u type=%u focusWindow=%p behavior=0x%X",
                adapter, static_cast<unsigned>(type), static_cast<void*>(window), flags);
            g_trace(message);
        }
        TracePresentParameters("CreateDevice input", params);
        if (g_mode == 2 || !ex_) {
            const HRESULT result = legacy_->CreateDevice(adapter, type, window, flags, params, device);
            Trace("stage 2 legacy CreateDevice", result);
            return result;
        }
        IDirect3DDevice9Ex* ex = nullptr;
        D3DDISPLAYMODEEX fullscreen{};
        fullscreen.Size = sizeof(fullscreen);
        D3DDISPLAYMODEEX* mode = nullptr;
        if (params && !params->Windowed) {
            D3DDISPLAYMODE current{};
            ex_->GetAdapterDisplayMode(adapter, &current);
            fullscreen.Width = params->BackBufferWidth ? params->BackBufferWidth : current.Width;
            fullscreen.Height = params->BackBufferHeight ? params->BackBufferHeight : current.Height;
            fullscreen.RefreshRate = params->FullScreen_RefreshRateInHz ?
                params->FullScreen_RefreshRateInHz : current.RefreshRate;
            fullscreen.Format = params->BackBufferFormat != D3DFMT_UNKNOWN ?
                params->BackBufferFormat : current.Format;
            fullscreen.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            mode = &fullscreen;
        }
        TraceMessage("D3D9Ex bridge stage 3 calling CreateDeviceEx");
        const HRESULT result = ex_->CreateDeviceEx(adapter, type, window, flags, params, mode, &ex);
        Trace("CreateDeviceEx", result);
        TracePresentParameters("CreateDeviceEx output", params);
        if (SUCCEEDED(result)) {
            *device = ex;
            InterlockedExchange(&g_createdExDevice, 1);
            InstallExceptionProbe();
            if (g_trace) {
                char message[128]{};
                std::snprintf(message, sizeof(message),
                              "D3D9Ex bridge stage 3 Ex device ready device=%p window=%p",
                              static_cast<void*>(ex), static_cast<void*>(window));
                g_trace(message);
            }
            TraceMessage("D3D9Ex bridge stage 3 returning Ex device to RenderWare");
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
    ~D3D9Proxy() {
        TraceMessage("D3D9Ex bridge proxy released");
        if (ex_) ex_->Release();
        legacy_->Release();
    }
    IDirect3D9* legacy_;
    IDirect3D9Ex* ex_;
    volatile LONG refs_ = 1;
};

IDirect3D9* WINAPI OnDirect3DCreate9(UINT version) {
    if (g_insideCreate9) return g_create9Hook.stdcall<IDirect3D9*>(version);
    if (g_trace) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
                      "D3D9Ex bridge Direct3DCreate9 entered sdk=%u mode=%ld thread=%lu",
                      version, g_mode, GetCurrentThreadId());
        g_trace(message);
    }
    g_insideCreate9 = true;
    IDirect3D9* legacy = g_create9Hook.stdcall<IDirect3D9*>(version);
    Trace("legacy Direct3DCreate9", legacy ? S_OK : E_FAIL);

    // Stage 1 proves that installing and entering the system-function hook is safe.
    // It deliberately returns the game's original object without creating a proxy
    // or changing the device type. Later stages are enabled only after this passes.
    if (g_mode == 1) {
        g_insideCreate9 = false;
        TraceMessage("D3D9Ex bridge stage 1 returning untouched legacy IDirect3D9");
        return legacy;
    }

    if (g_mode == 2) {
        g_insideCreate9 = false;
        if (auto* proxy = new (std::nothrow) D3D9Proxy(legacy)) {
            TraceMessage("D3D9Ex bridge stage 2 returning legacy forwarding proxy");
            return proxy;
        }
        TraceMessage("D3D9Ex bridge stage 2 proxy allocation failed; returning legacy object");
        return legacy;
    }

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
    if (g_trace) {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(d3d9, modulePath, MAX_PATH);
        char message[512]{};
        std::snprintf(message, sizeof(message),
                      "D3D9Ex bridge installing mode=%ld d3d9=%p create9=%p module=%ls",
                      g_mode, static_cast<void*>(d3d9), reinterpret_cast<void*>(create9), modulePath);
        g_trace(message);
    }
    g_create9Hook = safetyhook::create_inline(reinterpret_cast<void*>(create9), &OnDirect3DCreate9);
    TraceMessage(g_create9Hook ? "D3D9Ex bridge inline hook ready"
                               : "D3D9Ex bridge inline hook creation failed");
    return static_cast<bool>(g_create9Hook);
}

bool CreatedExDevice() { return InterlockedCompareExchange(&g_createdExDevice, 0, 0) != 0; }
LONG BridgeMode() { return g_mode; }

} // namespace portal_sa
