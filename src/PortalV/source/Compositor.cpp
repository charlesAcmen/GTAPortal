#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "Compositor.h"
#include "../../PortalVFrames.h"

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <reshade.hpp>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;
namespace api = reshade::api;
namespace frames = portal_v_frames;

namespace portal_v::compositor {
namespace {

struct SharedSource {
    std::wstring name;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
};

struct DisplaySlot {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D11Texture2D> open11;
    api::resource_view view{};
    uint64_t retireValue = 0;
};

std::mutex g_poseMutex;
Pose g_pose;
bool g_registered = false;
api::effect_runtime* g_runtime = nullptr;
HANDLE g_mapping = nullptr;
frames::SharedFrames* g_frames = nullptr;
LONG g_generation = 0;
DWORD g_producerPid = 0;
uint64_t g_frameId = 0;
uint64_t g_lastFrameTick = 0;
uint64_t g_received = 0;
uint64_t g_skipped = 0;
std::array<SharedSource, frames::kSlotCount> g_sources;
std::array<DisplaySlot, 3> g_display;
int g_activeDisplay = -1;
int g_pendingRetire = -1;
UINT g_width = 0;
UINT g_height = 0;
ComPtr<ID3D11Device> g_reader;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11DeviceContext4> g_context4;
ComPtr<ID3D11Device1> g_reader1;
ComPtr<ID3D11Device5> g_reader5;
ComPtr<ID3D12Fence> g_fence;
ComPtr<ID3D11Fence> g_fence11;
uint64_t g_nextFence = 0;

void Log(const char* message) { reshade::log::message(reshade::log::level::warning, message); }
void Info(const char* message) { reshade::log::message(reshade::log::level::info, message); }

void CloseSource() {
    for (auto& source : g_sources) source = {};
    if (g_frames) UnmapViewOfFile(g_frames);
    g_frames = nullptr;
    if (g_mapping) CloseHandle(g_mapping);
    g_mapping = nullptr;
    g_generation = 0;
    g_producerPid = 0;
    g_frameId = 0;
}

void DestroyDisplay(api::effect_runtime* runtime) {
    if (runtime) runtime->get_command_queue()->wait_idle();
    for (auto& slot : g_display) {
        slot.open11.Reset();
        if (runtime && slot.view.handle) runtime->get_device()->destroy_resource_view(slot.view);
        slot = {};
    }
    g_width = g_height = 0;
    g_activeDisplay = -1;
    g_pendingRetire = -1;
}

void ResetRuntime(api::effect_runtime* runtime) {
    DestroyDisplay(runtime);
    CloseSource();
    g_fence11.Reset();
    g_fence.Reset();
    g_reader5.Reset();
    g_reader1.Reset();
    g_context4.Reset();
    g_context.Reset();
    g_reader.Reset();
    g_nextFence = 0;
    g_lastFrameTick = 0;
    g_received = g_skipped = 0;
}

bool EnsureReader(api::effect_runtime* runtime) {
    if (g_reader) return true;
    if (runtime->get_device()->get_api() != api::device_api::d3d12) {
        static bool warned = false;
        if (!warned) Log("PortalV SA consumer requires a D3D12 ReShade runtime");
        warned = true;
        return false;
    }
    auto* d12 = reinterpret_cast<ID3D12Device*>(runtime->get_device()->get_native());
    const LUID luid = d12->GetAdapterLuid();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) == S_OK; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if (description.AdapterLuid.LowPart == luid.LowPart &&
            description.AdapterLuid.HighPart == luid.HighPart) break;
        adapter.Reset();
    }
    if (!adapter) return false;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &g_reader, nullptr, &g_context))) return false;
    if (FAILED(g_reader.As(&g_reader1)) || FAILED(g_reader.As(&g_reader5)) ||
        FAILED(g_context.As(&g_context4))) return false;
    if (FAILED(d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_fence)))) return false;
    HANDLE sharedFence = nullptr;
    HRESULT result = d12->CreateSharedHandle(g_fence.Get(), nullptr, GENERIC_ALL,
                                              nullptr, &sharedFence);
    if (FAILED(result)) return false;
    result = g_reader5->OpenSharedFence(sharedFence, IID_PPV_ARGS(&g_fence11));
    CloseHandle(sharedFence);
    if (SUCCEEDED(result)) Info("PortalV SA consumer initialized D3D11/D3D12 GPU bridge");
    return SUCCEEDED(result);
}

bool OpenFrames() {
    if (g_frames) return true;
    g_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, frames::kSaMappingName);
    if (!g_mapping) return false;
    g_frames = static_cast<frames::SharedFrames*>(MapViewOfFile(
        g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(frames::SharedFrames)));
    if (!g_frames || g_frames->magic != frames::kSaMagic ||
        g_frames->version != frames::kVersion || g_frames->size != sizeof(*g_frames)) {
        CloseSource();
        return false;
    }
    return true;
}

bool EnsureDisplay(api::effect_runtime* runtime, UINT width, UINT height) {
    if (g_width == width && g_height == height) return true;
    DestroyDisplay(runtime);
    auto* d12 = reinterpret_cast<ID3D12Device*>(runtime->get_device()->get_native());
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Both flags are required for this adapter's D3D11 OpenSharedResource1 path.
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                        D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    for (auto& slot : g_display) {
        HRESULT result = d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED,
            &description, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.texture));
        if (FAILED(result)) { DestroyDisplay(runtime); return false; }
        HANDLE handle = nullptr;
        result = d12->CreateSharedHandle(slot.texture.Get(), nullptr, GENERIC_ALL,
                                         nullptr, &handle);
        if (FAILED(result)) { DestroyDisplay(runtime); return false; }
        result = g_reader1->OpenSharedResource1(handle, IID_PPV_ARGS(&slot.open11));
        CloseHandle(handle);
        if (FAILED(result)) { DestroyDisplay(runtime); return false; }
        const api::resource resource{reinterpret_cast<uintptr_t>(slot.texture.Get())};
        if (!runtime->get_device()->create_resource_view(resource, api::resource_usage::shader_resource,
                api::resource_view_desc(api::format::b8g8r8a8_unorm), &slot.view)) {
            DestroyDisplay(runtime);
            return false;
        }
    }
    g_width = width;
    g_height = height;
    return true;
}

void SetUniforms(api::effect_runtime* runtime, const Pose& pose, bool active) {
    auto setFloat = [runtime](const char* name, float x, float y = 0, float z = 0, float w = 0) {
        const auto variable = runtime->find_uniform_variable("PortalSA.fx", name);
        if (variable.handle) runtime->set_uniform_value_float(variable, x, y, z, w);
    };
    const auto enabled = runtime->find_uniform_variable("PortalSA.fx", "SAActive");
    if (enabled.handle) runtime->set_uniform_value_bool(enabled, active);
    if (!active) return;
    setFloat("CameraPosition", pose.camera[0], pose.camera[1], pose.camera[2]);
    setFloat("CameraRotation", pose.rotation[0], pose.rotation[1], pose.rotation[2]);
    setFloat("CameraFov", pose.fov);
    setFloat("CameraPlanes", pose.nearClip, pose.farClip);
    setFloat("PlaneCenter", pose.planeCenter[0], pose.planeCenter[1], pose.planeCenter[2]);
    setFloat("PlaneYaw", pose.planeYaw);
    setFloat("PlaneSize", 2.4f, 1.35f);
    setFloat("SAFrameSize", static_cast<float>(g_width), static_cast<float>(g_height));
}

void OnBegin(api::effect_runtime* runtime, api::command_list* commands,
             api::resource_view, api::resource_view) {
    if (g_runtime && g_runtime != runtime) ResetRuntime(g_runtime);
    g_runtime = runtime;
    Pose pose;
    { std::lock_guard lock(g_poseMutex); pose = g_pose; }
    if (!pose.planeValid) {
        SetUniforms(runtime, pose, false);
        return;
    }
    bool fresh = g_activeDisplay >= 0 && GetTickCount64() - g_lastFrameTick <= 500;
    if (!EnsureReader(runtime) || !OpenFrames()) {
        SetUniforms(runtime, pose, false);
        return;
    }
    const LONG generation = InterlockedCompareExchange(&g_frames->generation, 0, 0);
    if (!generation) { SetUniforms(runtime, pose, false); return; }
    if (generation != g_generation || g_producerPid != g_frames->producerPid) {
        for (auto& source : g_sources) source = {};
        g_generation = generation;
        g_producerPid = g_frames->producerPid;
        g_frameId = 0;
        g_activeDisplay = -1;
        g_lastFrameTick = 0;
        fresh = false;
    }
    const LUID adapter = reinterpret_cast<ID3D12Device*>(runtime->get_device()->get_native())->GetAdapterLuid();
    if (g_frames->adapterLuidLow != static_cast<uint32_t>(adapter.LowPart) ||
        g_frames->adapterLuidHigh != adapter.HighPart) {
        SetUniforms(runtime, pose, false);
        return;
    }
    int newest = -1;
    for (int index = 0; index < frames::kSlotCount; ++index) {
        auto& slot = g_frames->slots[index];
        if (InterlockedCompareExchange(&slot.state, 0, 0) == frames::kReady &&
            slot.frameId > g_frameId &&
            (newest < 0 || slot.frameId > g_frames->slots[newest].frameId)) newest = index;
    }
    if (newest >= 0) {
        auto& metadata = g_frames->slots[newest];
        if (metadata.width && metadata.height && metadata.width <= 4096 && metadata.height <= 4096 &&
            metadata.format == DXGI_FORMAT_B8G8R8A8_UNORM &&
            EnsureDisplay(runtime, metadata.width, metadata.height)) {
            fresh = g_activeDisplay >= 0 && GetTickCount64() - g_lastFrameTick <= 500;
            int target = -1;
            for (int index = 0; index < static_cast<int>(g_display.size()); ++index) {
                if (g_display[index].retireValue <= g_fence->GetCompletedValue() &&
                    index != g_activeDisplay) { target = index; break; }
            }
            if (target >= 0 && InterlockedCompareExchange(&g_frames->slots[newest].state,
                    frames::kReading, frames::kReady) == frames::kReady) {
                const uint64_t frameId = metadata.frameId;
                auto& source = g_sources[newest];
                if (source.name != metadata.resourceName) {
                    source = {};
                    source.name = metadata.resourceName;
                    HRESULT result = g_reader1->OpenSharedResourceByName(source.name.c_str(),
                        DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&source.texture));
                    if (SUCCEEDED(result)) result = source.texture.As(&source.mutex);
                    if (FAILED(result)) source = {};
                }
                if (source.mutex && source.mutex->AcquireSync(1, 0) == S_OK) {
                    g_context->CopyResource(g_display[target].open11.Get(), source.texture.Get());
                    source.mutex->ReleaseSync(0);
                    InterlockedExchange(&g_frames->slots[newest].state, frames::kFree);
                    const uint64_t upload = ++g_nextFence;
                    if (SUCCEEDED(g_context4->Signal(g_fence11.Get(), upload)) &&
                        runtime->get_command_queue()->wait({reinterpret_cast<uintptr_t>(g_fence.Get())}, upload)) {
                        const api::resource resource{reinterpret_cast<uintptr_t>(g_display[target].texture.Get())};
                        g_activeDisplay = target;
                        if (g_frameId && frameId > g_frameId + 1) g_skipped += frameId - g_frameId - 1;
                        g_frameId = frameId;
                        ++g_received;
                        if (g_received == 1 || g_received % 300 == 0) {
                            char message[160]{};
                            sprintf_s(message, "PortalV SA received=%llu skipped=%llu frame=%llu",
                                      static_cast<unsigned long long>(g_received),
                                      static_cast<unsigned long long>(g_skipped),
                                      static_cast<unsigned long long>(g_frameId));
                            Info(message);
                        }
                        g_lastFrameTick = GetTickCount64();
                        runtime->update_texture_bindings("SAFRAME", g_display[target].view,
                                                         g_display[target].view);
                        fresh = true;
                    }
                } else {
                    InterlockedCompareExchange(&g_frames->slots[newest].state,
                                               frames::kReady, frames::kReading);
                }
            }
        }
    }
    if (fresh && pose.planeValid && g_activeDisplay >= 0) {
        const api::resource resource{reinterpret_cast<uintptr_t>(g_display[g_activeDisplay].texture.Get())};
        commands->barrier(resource, api::resource_usage::general,
                          api::resource_usage::shader_resource);
        g_pendingRetire = g_activeDisplay;
    }
    SetUniforms(runtime, pose, fresh && pose.planeValid);
}

void OnFinish(api::effect_runtime*, api::command_list* commands,
              api::resource_view, api::resource_view) {
    if (g_pendingRetire < 0) return;
    const api::resource resource{reinterpret_cast<uintptr_t>(g_display[g_pendingRetire].texture.Get())};
    commands->barrier(resource, api::resource_usage::shader_resource, api::resource_usage::general);
}

void OnPresent(api::effect_runtime* runtime) {
    if (g_pendingRetire < 0 || !g_fence) return;
    const uint64_t retired = ++g_nextFence;
    if (runtime->get_command_queue()->signal({reinterpret_cast<uintptr_t>(g_fence.Get())}, retired))
        g_display[g_pendingRetire].retireValue = retired;
    g_pendingRetire = -1;
}

void OnDestroy(api::effect_runtime* runtime) {
    if (runtime != g_runtime) return;
    ResetRuntime(runtime);
    g_runtime = nullptr;
}

void OnReload(api::effect_runtime* runtime) {
    if (runtime == g_runtime && g_activeDisplay >= 0) {
        runtime->update_texture_bindings("SAFRAME", g_display[g_activeDisplay].view,
                                         g_display[g_activeDisplay].view);
    }
}

} // namespace

bool TryRegister(HMODULE module) {
    if (g_registered) return true;
    if (!reshade::register_addon(module)) return false;
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnBegin);
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinish);
    reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReload);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroy);
    g_registered = true;
    return true;
}

void SetPose(const Pose& pose) {
    std::lock_guard lock(g_poseMutex);
    g_pose = pose;
}

void Unregister(HMODULE module) {
    if (!g_registered) return;
    reshade::unregister_addon(module);
    g_registered = false;
}

} // namespace portal_v::compositor
