#include "VConsumer.h"

#include <d3d9.h>
#include <windows.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "StateCapture.h"
#include "../../VToSAFrames.h"

namespace portal_sa::v_consumer {
namespace {

struct Vec3 { float x, y, z; };
struct Plane { Vec3 center, right, up; bool placed = false; };
struct Slot { IDirect3DTexture9* texture = nullptr; IDirect3DQuery9* readDone = nullptr; };

HANDLE g_mapping = nullptr;
v_to_sa::SharedFrames* g_shared = nullptr;
std::array<Slot, v_to_sa::kSlotCount> g_slots{};
IDirect3DDevice9* g_device = nullptr;
Plane g_plane{};
wchar_t g_planePath[MAX_PATH]{};
int g_current = -1;
bool g_queryPending = false;
bool g_f9Down = false;
DWORD g_lastAttempt = 0;
uint64_t g_lastFrame = 0;
uint64_t g_lastReadyTick = 0;
LONG g_nextGeneration = 1;

Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 Unit(Vec3 a) {
    const float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return length > 0.0001f ? Scale(a, 1.0f / length) : Vec3{};
}
Vec3 Copy(state::Vec3 a) { return {a.x, a.y, a.z}; }

void PlanePath() {
    if (g_planePath[0]) return;
    const DWORD length = GetModuleFileNameW(nullptr, g_planePath, MAX_PATH);
    if (!length || length >= MAX_PATH - 24) { g_planePath[0] = 0; return; }
    wchar_t* slash = std::wcsrchr(g_planePath, L'\\');
    if (!slash) { g_planePath[0] = 0; return; }
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_planePath), L"PortalSAPlane.txt");
}

void LoadPlane() {
    PlanePath();
    if (!g_planePath[0]) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_planePath, L"r") || !file) return;
    Plane value{};
    const int read = ::fscanf_s(file, "%f %f %f %f %f %f %f %f %f",
        &value.center.x, &value.center.y, &value.center.z,
        &value.right.x, &value.right.y, &value.right.z,
        &value.up.x, &value.up.y, &value.up.z);
    std::fclose(file);
    if (read == 9) { value.placed = true; g_plane = value; }
}

void SavePlane() {
    PlanePath();
    if (!g_planePath[0]) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_planePath, L"w") || !file) return;
    std::fprintf(file, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
        g_plane.center.x, g_plane.center.y, g_plane.center.z,
        g_plane.right.x, g_plane.right.y, g_plane.right.z,
        g_plane.up.x, g_plane.up.y, g_plane.up.z);
    std::fclose(file);
}

bool IsGameForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool AttachMapping(void (*log)(const char*)) {
    if (g_shared) return true;
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(v_to_sa::SharedFrames), v_to_sa::kMappingName);
    if (!g_mapping) { log("V consumer mapping creation failed"); return false; }
    g_shared = static_cast<v_to_sa::SharedFrames*>(MapViewOfFile(g_mapping,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(v_to_sa::SharedFrames)));
    if (!g_shared) { CloseHandle(g_mapping); g_mapping = nullptr; log("V consumer mapping view failed"); return false; }
    ZeroMemory(g_shared, sizeof(*g_shared));
    g_shared->version = v_to_sa::kVersion;
    g_shared->size = sizeof(*g_shared);
    g_shared->ownerPid = GetCurrentProcessId();
    g_shared->width = v_to_sa::kWidth;
    g_shared->height = v_to_sa::kHeight;
    g_shared->format = v_to_sa::kFormat;
    g_nextGeneration = static_cast<LONG>(GetTickCount() | 1);
    MemoryBarrier();
    g_shared->magic = v_to_sa::kMagic;
    log("V consumer control mapping ready");
    return true;
}

void ReleaseSlots() {
    if (g_shared) {
        InterlockedExchange(&g_shared->stage, v_to_sa::kStarting);
        // Invalidate the handles before releasing D3D9 resources; the tool acknowledges closing its D3D11 views.
        InterlockedExchange(&g_shared->generation, 0);
        const DWORD start = GetTickCount();
        while (g_shared->readerPid && g_shared->readerGeneration && GetTickCount() - start < 250) Sleep(1);
        if (g_current >= 0 && g_queryPending && g_slots[g_current].readDone) {
            const DWORD readStart = GetTickCount();
            while (g_slots[g_current].readDone->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE &&
                GetTickCount() - readStart < 200) Sleep(1);
        }
        for (auto& slot : g_shared->slots) {
            InterlockedExchange(&slot.state, v_to_sa::kFree);
            slot.sharedHandle = 0;
        }
        g_shared->adapterLuidLow = 0;
        g_shared->adapterLuidHigh = 0;
    }
    for (auto& slot : g_slots) {
        if (slot.readDone) slot.readDone->Release();
        if (slot.texture) slot.texture->Release();
        slot = {};
    }
    g_current = -1;
    g_queryPending = false;
    g_lastFrame = 0;
    g_lastReadyTick = 0;
    g_device = nullptr;
}

bool CreateSlots(IDirect3DDevice9* device, void (*log)(const char*)) {
    ReleaseSlots();
    IDirect3D9* direct3D = nullptr;
    IDirect3D9Ex* direct3DEx = nullptr;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (SUCCEEDED(device->GetDirect3D(&direct3D)) &&
        SUCCEEDED(direct3D->QueryInterface(IID_IDirect3D9Ex,
            reinterpret_cast<void**>(&direct3DEx))) &&
        SUCCEEDED(device->GetCreationParameters(&creation))) {
        LUID luid{};
        if (SUCCEEDED(direct3DEx->GetAdapterLUID(creation.AdapterOrdinal, &luid))) {
            g_shared->adapterLuidLow = luid.LowPart;
            g_shared->adapterLuidHigh = luid.HighPart;
        }
    }
    if (direct3DEx) direct3DEx->Release();
    if (direct3D) direct3D->Release();
    for (int index = 0; index < v_to_sa::kSlotCount; ++index) {
        HANDLE handle = nullptr;
        HRESULT hr = device->CreateTexture(v_to_sa::kWidth, v_to_sa::kHeight, 1,
            D3DUSAGE_RENDERTARGET, D3DFMT_A8B8G8R8, D3DPOOL_DEFAULT,
            &g_slots[index].texture, &handle);
        if (SUCCEEDED(hr)) hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &g_slots[index].readDone);
        if (FAILED(hr) || !handle) {
            const HRESULT error = FAILED(hr) ? hr : E_FAIL;
            ReleaseSlots();
            InterlockedExchange(&g_shared->lastError, error);
            InterlockedExchange(&g_shared->stage, v_to_sa::kTextureError);
            char message[96]{};
            std::snprintf(message, sizeof(message), "V consumer CreateTexture/Query failed hr=0x%08X",
                static_cast<unsigned>(error));
            log(message);
            return false;
        }
        g_shared->slots[index].sharedHandle = reinterpret_cast<uintptr_t>(handle);
    }
    g_device = device;
    MemoryBarrier();
    InterlockedExchange(&g_shared->lastError, S_OK);
    InterlockedExchange(&g_shared->generation, ++g_nextGeneration);
    InterlockedExchange(&g_shared->stage, v_to_sa::kTexturesReady);
    log("V consumer shared D3D9 textures ready");
    return true;
}

int NewestReady() {
    int best = -1;
    for (int index = 0; index < v_to_sa::kSlotCount; ++index) {
        auto& slot = g_shared->slots[index];
        if (InterlockedCompareExchange(&slot.state, 0, 0) == v_to_sa::kReady &&
            (best < 0 || slot.readyTickMs > g_shared->slots[best].readyTickMs ||
             (slot.readyTickMs == g_shared->slots[best].readyTickMs &&
              slot.frameId > g_shared->slots[best].frameId))) best = index;
    }
    return best;
}

HRESULT Draw(IDirect3DDevice9* device, IDirect3DTexture9* texture) {
    IDirect3DStateBlock9* saved = nullptr;
    const HRESULT stateBlock = device->CreateStateBlock(D3DSBT_ALL, &saved);
    if (FAILED(stateBlock)) return stateBlock;
    saved->Capture();
    D3DMATRIX world{};
    world._11 = world._22 = world._33 = world._44 = 1.0f;
    D3DMATRIX view{}, projection{};
    std::memcpy(&view, state::g_viewMatrix.data(), sizeof(view));
    std::memcpy(&projection, state::g_projectionMatrix.data(), sizeof(projection));
    device->SetTransform(D3DTS_WORLD, &world);
    device->SetTransform(D3DTS_VIEW, &view);
    device->SetTransform(D3DTS_PROJECTION, &projection);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    device->SetTexture(0, texture);
    struct Vertex { float x, y, z, u, v; };
    const Vec3 right = Scale(g_plane.right, 1.2f);
    const Vec3 up = Scale(g_plane.up, 0.675f);
    const Vec3 a = Add(Add(g_plane.center, Scale(right, -1)), up);
    const Vec3 b = Add(Add(g_plane.center, right), up);
    const Vec3 c = Add(Add(g_plane.center, Scale(right, -1)), Scale(up, -1));
    const Vec3 d = Add(Add(g_plane.center, right), Scale(up, -1));
    const Vertex vertices[] = {
        {a.x, a.y, a.z, 0, 0}, {b.x, b.y, b.z, 1, 0},
        {c.x, c.y, c.z, 0, 1}, {d.x, d.y, d.z, 1, 1},
    };
    const HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    saved->Apply();
    saved->Release();
    return drawn;
}

} // namespace

void OnGameProcess(void (*log)(const char*)) {
    static bool loaded = false;
    if (!loaded) { LoadPlane(); loaded = true; }
    const bool pressed = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (pressed && !g_f9Down && IsGameForeground() && state::g_state.cameraValid) {
        const Vec3 forward = Unit(Copy(state::g_state.cameraForward));
        const Vec3 up = Unit(Copy(state::g_state.cameraUp));
        const Vec3 right = Unit(Cross(forward, up));
        if (right.x || right.y || right.z) {
            g_plane.center = Add(Copy(state::g_state.cameraPosition), Scale(forward, 3.0f));
            g_plane.right = right;
            g_plane.up = Unit(Cross(right, forward));
            g_plane.placed = true;
            SavePlane();
            log("V consumer plane placed 3 m in front of SA camera");
        }
    }
    g_f9Down = pressed;
}

void OnRenderScene(IDirect3DDevice9* device, void (*log)(const char*)) {
    if (!device || !AttachMapping(log)) return;
    if (g_device != device || !g_slots[0].texture) {
        const DWORD now = GetTickCount();
        if (g_lastAttempt && now - g_lastAttempt < 2000) return;
        g_lastAttempt = now;
        if (!CreateSlots(device, log)) return;
    }
    if (g_current >= 0 && g_queryPending) {
        const HRESULT done = g_slots[g_current].readDone->GetData(nullptr, 0, 0);
        if (done == S_FALSE) return;
        g_queryPending = false;
        if (FAILED(done)) {
            InterlockedExchange(&g_shared->slots[g_current].state, v_to_sa::kFree);
            g_current = -1;
            return;
        }
    }
    const int newest = NewestReady();
    if (newest >= 0 &&
        (g_shared->slots[newest].readyTickMs > g_lastReadyTick ||
         (g_shared->slots[newest].readyTickMs == g_lastReadyTick &&
          g_shared->slots[newest].frameId > g_lastFrame)) &&
        InterlockedCompareExchange(&g_shared->slots[newest].state,
            v_to_sa::kReading, v_to_sa::kReady) == v_to_sa::kReady) {
        if (g_current >= 0) InterlockedExchange(&g_shared->slots[g_current].state, v_to_sa::kFree);
        g_current = newest;
        g_lastFrame = g_shared->slots[newest].frameId;
        g_lastReadyTick = g_shared->slots[newest].readyTickMs;
    }
    if (g_current < 0) return;
    if (!g_plane.placed || !state::g_state.cameraValid ||
        GetTickCount64() - g_shared->slots[g_current].readyTickMs > 500) {
        InterlockedExchange(&g_shared->slots[g_current].state, v_to_sa::kFree);
        g_current = -1;
        return;
    }
    // This hook follows RenderScene; in-game testing must confirm its scene depth buffer remains bound.
    const HRESULT drawn = Draw(device, g_slots[g_current].texture);
    if (FAILED(drawn)) {
        InterlockedExchange(&g_shared->lastError, drawn);
        InterlockedExchange(&g_shared->stage, v_to_sa::kDrawError);
        InterlockedExchange(&g_shared->slots[g_current].state, v_to_sa::kFree);
        g_current = -1;
        return;
    }
    if (SUCCEEDED(g_slots[g_current].readDone->Issue(D3DISSUE_END))) {
        g_queryPending = true;
        InterlockedIncrement(&g_shared->displayed);
    } else {
        // The draw may still be queued; keep this slot unavailable until device reset.
        g_current = -1;
    }
}

void BeforeReset() { ReleaseSlots(); }

} // namespace portal_sa::v_consumer
