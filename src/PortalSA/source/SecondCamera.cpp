#include "SecondCamera.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <CCamera.h>
#include <CDraw.h>
#include <CGame.h>
#include <CMirrors.h>
#include <CPlayerPed.h>
#include <CScene.h>
#include <CVisibilityPlugins.h>
#include <common.h>

#include "CameraOverride.h"
#include "CameraProjection.h"
#include "../../SAToVOffscreenFrames.h"

namespace portal_sa::second_camera {
namespace {

constexpr UINT kWidth = camera_projection::kRenderWidth;
constexpr UINT kHeight = camera_projection::kRenderHeight;
constexpr DWORD kLogIntervalMs = 2000;

HANDLE g_controlMapping{};
HANDLE g_pixelMapping{};
sa_to_v_offscreen::SharedFrames* g_shared{};
void* g_pixels{};
RwRaster* g_colorRaster{};
RwRaster* g_depthRaster{};
RwCamera* g_camera{};
bool g_cameraInWorld{};
IDirect3DSurface9* g_readback{};
D3DFORMAT g_readbackFormat = D3DFMT_UNKNOWN;
uint64_t g_frameId{};
DWORD g_lastLogTick{};
bool g_loggedActive{};
uint32_t g_traceMask{};

void TraceOnce(uint32_t bit, const char* message, void (*log)(const char*)) {
    if (g_traceMask & bit) return;
    g_traceMask |= bit;
    log(message);
}

void SetStage(sa_to_v_offscreen::Stage stage, HRESULT error = S_OK) {
    if (!g_shared) return;
    InterlockedExchange(&g_shared->stage, stage);
    InterlockedExchange(&g_shared->lastError, static_cast<LONG>(error));
}

void ReleaseRenderResources() {
    if (g_readback) g_readback->Release();
    g_readback = nullptr;
    g_readbackFormat = D3DFMT_UNKNOWN;

    if (g_camera) {
        if (g_cameraInWorld && Scene.m_pWorld) RpWorldRemoveCamera(Scene.m_pWorld, g_camera);
        RwCameraSetRaster(g_camera, nullptr);
        RwCameraSetZRaster(g_camera, nullptr);
        RwFrame* frame = RwCameraGetFrame(g_camera);
        RwCameraSetFrame(g_camera, nullptr);
        RwCameraDestroy(g_camera);
        if (frame) RwFrameDestroy(frame);
    }
    g_cameraInWorld = false;
    if (g_colorRaster) RwRasterDestroy(g_colorRaster);
    if (g_depthRaster) RwRasterDestroy(g_depthRaster);
    g_camera = nullptr;
    g_colorRaster = nullptr;
    g_depthRaster = nullptr;
}

void ReleaseMappings() {
    if (g_pixels) UnmapViewOfFile(g_pixels);
    if (g_pixelMapping) CloseHandle(g_pixelMapping);
    if (g_shared) UnmapViewOfFile(g_shared);
    if (g_controlMapping) CloseHandle(g_controlMapping);
    g_pixels = nullptr;
    g_pixelMapping = nullptr;
    g_shared = nullptr;
    g_controlMapping = nullptr;
}

bool EnsureControlMapping() {
    if (g_shared) return true;
    g_controlMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(sa_to_v_offscreen::SharedFrames), sa_to_v_offscreen::kMappingName);
    if (!g_controlMapping) return false;
    g_shared = static_cast<sa_to_v_offscreen::SharedFrames*>(MapViewOfFile(
        g_controlMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(sa_to_v_offscreen::SharedFrames)));
    if (!g_shared) {
        CloseHandle(g_controlMapping);
        g_controlMapping = nullptr;
        return false;
    }
    ZeroMemory(g_shared, sizeof(*g_shared));
    g_shared->version = sa_to_v_offscreen::kVersion;
    g_shared->size = sizeof(*g_shared);
    g_shared->ownerPid = GetCurrentProcessId();
    g_shared->width = kWidth;
    g_shared->height = kHeight;
    g_shared->rowBytes = kWidth * sa_to_v_offscreen::kBytesPerPixel;
    g_shared->format = sa_to_v_offscreen::kFormatBgra8;
    g_shared->slotBytes = static_cast<uint64_t>(g_shared->rowBytes) * kHeight;
    g_shared->stage = sa_to_v_offscreen::kStarting;
    MemoryBarrier();
    g_shared->magic = sa_to_v_offscreen::kMagic;
    return true;
}

bool EnsurePixelMapping() {
    if (g_pixels) return true;
    if (!EnsureControlMapping()) return false;
    const LONG generation = InterlockedIncrement(&g_shared->generation);
    swprintf_s(g_shared->pixelMappingName, L"%s.%lu.%ld",
        sa_to_v_offscreen::kPixelMappingPrefix, GetCurrentProcessId(), generation);
    const uint64_t totalBytes = g_shared->slotBytes * sa_to_v_offscreen::kSlotCount;
    g_pixelMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(totalBytes >> 32), static_cast<DWORD>(totalBytes),
        g_shared->pixelMappingName);
    if (!g_pixelMapping) return false;
    g_pixels = MapViewOfFile(g_pixelMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        static_cast<SIZE_T>(totalBytes));
    if (!g_pixels) {
        CloseHandle(g_pixelMapping);
        g_pixelMapping = nullptr;
        return false;
    }
    for (auto& slot : g_shared->slots) InterlockedExchange(&slot.state, sa_to_v_offscreen::kFree);
    return true;
}

bool EnsureRenderResources(IDirect3DDevice9*) {
    if (g_camera && g_colorRaster && g_depthRaster) return EnsurePixelMapping();
    ReleaseRenderResources();
    if (!Scene.m_pWorld || !Scene.m_pCamera) return false;
    const int depth = RwCameraGetRaster(Scene.m_pCamera)
        ? RwRasterGetDepth(RwCameraGetRaster(Scene.m_pCamera)) : 32;
    g_camera = RwCameraCreate();
    if (!g_camera) return false;
    RwFrame* frame = RwFrameCreate();
    if (!frame) {
        ReleaseRenderResources();
        return false;
    }
    RwCameraSetFrame(g_camera, frame);
    g_colorRaster = RwRasterCreate(kWidth, kHeight, depth, rwRASTERTYPECAMERATEXTURE);
    g_depthRaster = RwRasterCreate(kWidth, kHeight, depth, rwRASTERTYPEZBUFFER);
    if (!g_colorRaster || !g_depthRaster) {
        ReleaseRenderResources();
        return false;
    }
    RwCameraSetRaster(g_camera, g_colorRaster);
    RwCameraSetZRaster(g_camera, g_depthRaster);
    RwCameraSetProjection(g_camera, rwPERSPECTIVE);
    RwCameraSetNearClipPlane(g_camera, RwCameraGetNearClipPlane(Scene.m_pCamera));
    RwCameraSetFarClipPlane(g_camera, RwCameraGetFarClipPlane(Scene.m_pCamera));
    if (!RpWorldAddCamera(Scene.m_pWorld, g_camera)) {
        ReleaseRenderResources();
        return false;
    }
    g_cameraInWorld = true;
    return EnsurePixelMapping();
}

CVector Cross(const CVector& left, const CVector& right) {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

float Length(const CVector& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

CVector Unit(const CVector& value, const CVector& fallback) {
    const float length = Length(value);
    return length > 0.0001f ? value * (1.0f / length) : fallback;
}

void ResetWeaponPedsForPC() {
    // plugin-sdk's older CLinkList declaration exposes the layout but not the
    // inline Clear() used by the game and gta-reversed. Return every used node
    // to the free list exactly like CLinkList::Clear/Remove.
    auto& list = CVisibilityPlugins::ms_weaponPedsForPC;
    while (list.usedListHead.next != &list.usedListTail) {
        CLink<CPed*>* link = list.usedListHead.next;
        link->next->prev = link->prev;
        link->prev->next = link->next;
        link->next = list.freeListHead.next;
        link->next->prev = link;
        link->prev = &list.freeListHead;
        link->prev->next = link;
    }
}

int ClaimSlot() {
    for (int index = 0; index < sa_to_v_offscreen::kSlotCount; ++index) {
        if (InterlockedCompareExchange(&g_shared->slots[index].state,
                sa_to_v_offscreen::kWriting, sa_to_v_offscreen::kFree) ==
            sa_to_v_offscreen::kFree) return index;
    }
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    int candidate = -1;
    for (int index = 0; index < sa_to_v_offscreen::kSlotCount; ++index) {
        if (InterlockedCompareExchange(&g_shared->slots[index].state, 0, 0) ==
                sa_to_v_offscreen::kReady && g_shared->slots[index].frameId < oldest) {
            oldest = g_shared->slots[index].frameId;
            candidate = index;
        }
    }
    if (candidate >= 0 && InterlockedCompareExchange(&g_shared->slots[candidate].state,
            sa_to_v_offscreen::kWriting, sa_to_v_offscreen::kReady) ==
        sa_to_v_offscreen::kReady) {
        InterlockedIncrement(&g_shared->overwritten);
        return candidate;
    }
    InterlockedIncrement(&g_shared->dropped);
    return -1;
}

bool PublishReadback(IDirect3DDevice9* device) {
    IDirect3DSurface9* target = nullptr;
    HRESULT result = device->GetRenderTarget(0, &target);
    if (FAILED(result) || !target) return false;
    D3DSURFACE_DESC description{};
    result = target->GetDesc(&description);
    if (FAILED(result)) {
        target->Release();
        return false;
    }
    if (description.Width != kWidth || description.Height != kHeight ||
        (description.Format != D3DFMT_A8R8G8B8 && description.Format != D3DFMT_X8R8G8B8)) {
        target->Release();
        SetStage(sa_to_v_offscreen::kReadbackError, D3DERR_INVALIDCALL);
        return false;
    }
    if (!g_readback || g_readbackFormat != description.Format) {
        if (g_readback) g_readback->Release();
        g_readback = nullptr;
        g_readbackFormat = D3DFMT_UNKNOWN;
        result = device->CreateOffscreenPlainSurface(description.Width, description.Height,
            description.Format, D3DPOOL_SYSTEMMEM, &g_readback, nullptr);
        if (FAILED(result)) {
            target->Release();
            SetStage(sa_to_v_offscreen::kReadbackError, result);
            return false;
        }
        g_readbackFormat = description.Format;
    }
    result = device->GetRenderTargetData(target, g_readback);
    target->Release();
    if (FAILED(result)) {
        SetStage(sa_to_v_offscreen::kReadbackError, result);
        return false;
    }
    const int slotIndex = ClaimSlot();
    if (slotIndex < 0) return false;
    D3DLOCKED_RECT locked{};
    result = g_readback->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(result)) {
        InterlockedExchange(&g_shared->slots[slotIndex].state, sa_to_v_offscreen::kFree);
        SetStage(sa_to_v_offscreen::kReadbackError, result);
        return false;
    }
    auto* destination = sa_to_v_offscreen::SlotPixels(g_pixels, g_shared->slotBytes, slotIndex);
    for (UINT row = 0; row < kHeight; ++row) {
        auto* outputRow = destination + static_cast<size_t>(row) * g_shared->rowBytes;
        std::memcpy(outputRow,
            static_cast<const uint8_t*>(locked.pBits) + static_cast<size_t>(row) * locked.Pitch,
            g_shared->rowBytes);
        if (description.Format == D3DFMT_X8R8G8B8) {
            auto* outputPixels = reinterpret_cast<uint32_t*>(outputRow);
            for (UINT column = 0; column < kWidth; ++column) outputPixels[column] |= 0xFF000000u;
        }
    }
    g_readback->UnlockRect();

    LARGE_INTEGER qpc{}, frequency{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&frequency);
    auto& slot = g_shared->slots[slotIndex];
    slot.frameId = ++g_frameId;
    slot.readyTickMs = GetTickCount64();
    slot.captureTime100ns = frequency.QuadPart > 0
        ? static_cast<uint64_t>(static_cast<long double>(qpc.QuadPart) * 10000000.0L /
                                static_cast<long double>(frequency.QuadPart))
        : 0;
    MemoryBarrier();
    InterlockedExchange(&slot.state, sa_to_v_offscreen::kReady);
    InterlockedIncrement(&g_shared->produced);
    SetStage(sa_to_v_offscreen::kStreaming);
    return true;
}

struct CameraBackup {
    RwCamera* sceneCamera{};
    RwCamera* visibilityCamera{};
    CPlayerPed* player{};
    bool playerVisible{};
};

bool ApplyTarget(const camera_override::Target& target, CameraBackup& backup) {
    if (!Scene.m_pCamera || !g_camera || !RwCameraGetFrame(g_camera)) return false;
    backup.sceneCamera = Scene.m_pCamera;
    backup.visibilityCamera = CVisibilityPlugins::ms_pCamera;
    backup.player = FindPlayerPed(0);
    if (backup.player) {
        backup.playerVisible = backup.player->bIsVisible;
        backup.player->bIsVisible = false;
    }

    const CVector position(target.position[0], target.position[1], target.position[2]);
    const CVector forward = Unit(CVector(target.forward[0], target.forward[1], target.forward[2]),
                                 CVector(0.0f, 1.0f, 0.0f));
    CVector requestedUp = Unit(CVector(target.up[0], target.up[1], target.up[2]),
                               CVector(0.0f, 0.0f, 1.0f));
    const CVector right = Unit(Cross(forward, requestedUp), CVector(1.0f, 0.0f, 0.0f));
    requestedUp = Unit(Cross(right, forward), CVector(0.0f, 0.0f, 1.0f));

    RwMatrix* matrix = RwFrameGetMatrix(RwCameraGetFrame(g_camera));
    RwMatrixSetIdentity(matrix);
    matrix->right = {right.x, right.y, right.z};
    matrix->up = {forward.x, forward.y, forward.z};
    matrix->at = {requestedUp.x, requestedUp.y, requestedUp.z};
    matrix->pos = {position.x, position.y, position.z};
    RwFrameUpdateObjects(RwCameraGetFrame(g_camera));

    RwCameraSetNearClipPlane(g_camera, RwCameraGetNearClipPlane(backup.sceneCamera));
    RwCameraSetFarClipPlane(g_camera, RwCameraGetFarClipPlane(backup.sceneCamera));
    const float viewX = std::tan(target.horizontalFovDegrees * 0.00872664625997164788f);
    const RwV2d view{viewX, viewX * static_cast<float>(kHeight) / static_cast<float>(kWidth)};
    RwCameraSetViewWindow(g_camera, &view);

    Scene.m_pCamera = g_camera;
    CVisibilityPlugins::SetRenderWareCamera(g_camera);
    return true;
}

void RestoreCamera(const CameraBackup& backup) {
    Scene.m_pCamera = backup.sceneCamera;
    CVisibilityPlugins::SetRenderWareCamera(
        backup.visibilityCamera ? backup.visibilityCamera : backup.sceneCamera);
    if (backup.player && FindPlayerPed(0) == backup.player)
        backup.player->bIsVisible = backup.playerVisible;
}

} // namespace

void OnRenderMirror(IDirect3DDevice9* device, void (*log)(const char*)) {
    camera_override::Target target{};
    if (!device || !camera_override::ReadTarget(target)) {
        if (EnsureControlMapping()) SetStage(sa_to_v_offscreen::kWaitingForCamera);
        g_loggedActive = false;
        return;
    }
    if (!EnsureRenderResources(device)) {
        if (EnsureControlMapping()) SetStage(sa_to_v_offscreen::kMappingError, GetLastError());
        return;
    }
    TraceOnce(1u << 0, "SA second camera trace: render resources ready", log);

    CameraBackup backup{};
    if (!ApplyTarget(target, backup)) return;
    TraceOnce(1u << 1, "SA second camera trace: independent camera target applied", log);
    SetStage(sa_to_v_offscreen::kRendering);
    RwRGBA clear{0, 0, 0, 255};
    bool rendered = false;
    RwCameraClear(Scene.m_pCamera, &clear, rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
    if (RwCameraBeginUpdate(Scene.m_pCamera)) {
        TraceOnce(1u << 2, "SA second camera trace: BeginUpdate succeeded", log);
        const bool previousReflection = CMirrors::bRenderingReflection;
        CMirrors::bRenderingReflection = true;
        DefinedState();
        RenderScene();
        TraceOnce(1u << 3, "SA second camera trace: RenderScene returned", log);
        CVisibilityPlugins::RenderWeaponPedsForPC();
        ResetWeaponPedsForPC();
        CMirrors::bRenderingReflection = previousReflection;
        RwCameraEndUpdate(Scene.m_pCamera);
        rendered = PublishReadback(device);
        TraceOnce(1u << 4, "SA second camera trace: readback returned", log);
    }
    RestoreCamera(backup);
    TraceOnce(1u << 5, "SA second camera trace: local camera restored", log);

    if (!rendered) SetStage(sa_to_v_offscreen::kRenderError);
    const DWORD now = GetTickCount();
    if (rendered && (!g_loggedActive || now - g_lastLogTick >= kLogIntervalMs)) {
        log("SA second camera: offscreen 960x540 BGRA stream active; local camera restored");
        g_lastLogTick = now;
        g_loggedActive = true;
    }
}

void BeforeReset() {
    ReleaseRenderResources();
    g_traceMask = 0;
    if (g_shared) SetStage(sa_to_v_offscreen::kStarting);
}

void Shutdown() {
    ReleaseRenderResources();
    ReleaseMappings();
    g_loggedActive = false;
    g_traceMask = 0;
}

} // namespace portal_sa::second_camera
