#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "VFramePublisher.h"

#include <dxgi1_2.h>
#include <sstream>

namespace vf = portal_v_frames;

namespace {
volatile LONG g_generationSeed = static_cast<LONG>((GetTickCount64() ^ GetCurrentProcessId()) & 0x7fffffff);
const wchar_t* MappingName(VFramePublisher::Source source) {
    return source == VFramePublisher::Source::Sa ? vf::kSaMappingName : vf::kMappingName;
}
const wchar_t* SourceName(VFramePublisher::Source source) {
    return source == VFramePublisher::Source::Sa ? L"SA" : L"V";
}
uint32_t Magic(VFramePublisher::Source source) {
    return source == VFramePublisher::Source::Sa ? vf::kSaMagic : vf::kMagic;
}
}

VFramePublisher::~VFramePublisher() { Stop(); }

void VFramePublisher::SetError(const wchar_t* action, HRESULT result) {
    if (shared_) InterlockedIncrement(&shared_->errors);
    std::wostringstream text;
    text << action << L" 0x" << std::hex << static_cast<unsigned>(result);
    status_ = text.str();
}

bool VFramePublisher::Start(ID3D11Device* device) {
    Stop();
    // A capture restart in the same process must not reuse the previous generation.
    generation_ = InterlockedIncrement(&g_generationSeed);
    device_.copy_from(device);
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(vf::SharedFrames), MappingName(source_));
    if (!mapping_) {
        SetError(L"V 帧控制区创建失败", HRESULT_FROM_WIN32(GetLastError()));
        Stop();
        return false;
    }
    shared_ = static_cast<vf::SharedFrames*>(MapViewOfFile(
        mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(vf::SharedFrames)));
    if (!shared_) {
        SetError(L"V 帧控制区映射失败", HRESULT_FROM_WIN32(GetLastError()));
        Stop();
        return false;
    }
    ZeroMemory(shared_, sizeof(*shared_));
    shared_->size = sizeof(*shared_);
    shared_->version = vf::kVersion;
    shared_->producerPid = GetCurrentProcessId();
    auto dxgi = device_.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(dxgi->GetAdapter(adapter.put()))) {
        SetError(L"V 帧显卡查询失败", E_FAIL);
        Stop();
        return false;
    }
    DXGI_ADAPTER_DESC description{};
    adapter->GetDesc(&description);
    shared_->adapterLuidLow = description.AdapterLuid.LowPart;
    shared_->adapterLuidHigh = description.AdapterLuid.HighPart;
    MemoryBarrier();
    shared_->magic = Magic(source_);
    status_ = std::wstring(L"等待 ") + SourceName(source_) + L" 首帧";
    return true;
}

void VFramePublisher::ReleaseSlots() {
    if (shared_) InterlockedExchange(&shared_->generation, 0);
    for (auto& slot : slots_) {
        slot.mutex = nullptr;
        slot.texture = nullptr;
        if (slot.handle) CloseHandle(slot.handle);
        slot.handle = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

bool VFramePublisher::CreateSlots(UINT width, UINT height) {
    ReleaseSlots();
    if (!width || !height) return false;
    if (++generation_ == 0) ++generation_;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    for (int index = 0; index < vf::kSlotCount; ++index) {
        auto& slot = slots_[index];
        HRESULT result = device_->CreateTexture2D(&description, nullptr, slot.texture.put());
        if (SUCCEEDED(result)) {
            slot.mutex = slot.texture.as<IDXGIKeyedMutex>();
            wchar_t name[vf::kNameLength]{};
            swprintf_s(name, L"Local\\GTAPortal.%s.%lu.%llu.%ld.%d",
                       SourceName(source_), GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()),
                       generation_, index);
            result = slot.texture.as<IDXGIResource1>()->CreateSharedHandle(
                nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                name, &slot.handle);
            if (SUCCEEDED(result)) wcscpy_s(shared_->slots[index].resourceName, name);
        }
        if (FAILED(result)) {
            SetError(L"V 共享纹理创建失败", result);
            ReleaseSlots();
            return false;
        }
        shared_->slots[index].state = vf::kFree;
        shared_->slots[index].width = width;
        shared_->slots[index].height = height;
        shared_->slots[index].format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    width_ = width;
    height_ = height;
    shared_->width = width;
    shared_->height = height;
    MemoryBarrier();
    InterlockedExchange(&shared_->generation, generation_);
    return true;
}

bool VFramePublisher::Publish(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                              int64_t compositorTime100ns) {
    if (!shared_ || !source) return false;
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.SampleDesc.Count != 1) {
        SetError(L"V 捕获纹理格式不支持", E_INVALIDARG);
        return false;
    }
    if (width_ != description.Width || height_ != description.Height) {
        if (!CreateSlots(description.Width, description.Height)) return false;
    }
    int chosen = -1;
    LONG chosenState = vf::kFree;
    uint64_t oldest = UINT64_MAX;
    for (int index = 0; index < vf::kSlotCount; ++index) {
        auto& slot = shared_->slots[index];
        const LONG state = InterlockedCompareExchange(&slot.state, 0, 0);
        if (state == vf::kFree) {
            chosen = index;
            chosenState = vf::kFree;
            break;
        }
        if (state == vf::kReady && slot.frameId < oldest) {
            chosen = index;
            chosenState = vf::kReady;
            oldest = slot.frameId;
        }
    }
    if (chosen < 0) {
        ++dropped_;
        InterlockedIncrement(&shared_->dropped);
        return false;
    }
    auto& metadata = shared_->slots[chosen];
    const LONG previous = InterlockedCompareExchange(&metadata.state, vf::kWriting, chosenState);
    if (previous != chosenState) {
        ++dropped_;
        InterlockedIncrement(&shared_->dropped);
        return false;
    }
    auto& slot = slots_[chosen];
    if (previous == vf::kReady) {
        const HRESULT reclaim = slot.mutex->AcquireSync(1, 0);
        if (reclaim == WAIT_TIMEOUT) {
            InterlockedExchange(&metadata.state, vf::kReady);
            ++dropped_;
            InterlockedIncrement(&shared_->dropped);
            return false;
        }
        if (reclaim != S_OK) {
            SetError(L"V 旧帧回收失败", reclaim);
            InterlockedExchange(&metadata.state, vf::kReady);
            return false;
        }
        slot.mutex->ReleaseSync(0);
        ++overwritten_;
        InterlockedIncrement(&shared_->overwritten);
    }
    const HRESULT acquired = slot.mutex->AcquireSync(0, 0);
    if (acquired != S_OK) {
        SetError(L"V 纹理加锁失败", acquired);
        InterlockedExchange(&metadata.state, vf::kFree);
        return false;
    }
    context->CopyResource(slot.texture.get(), source);
    const HRESULT released = slot.mutex->ReleaseSync(1);
    if (FAILED(released)) {
        SetError(L"V 纹理解锁失败", released);
        InterlockedExchange(&metadata.state, vf::kFree);
        return false;
    }
    context->Flush();
    metadata.frameId = ++nextFrameId_;
    metadata.compositorTime100ns = compositorTime100ns;
    MemoryBarrier();
    InterlockedExchange(&metadata.state, vf::kReady);
    ++produced_;
    InterlockedIncrement(&shared_->produced);
    status_ = std::wstring(SourceName(source_)) + L" GPU 帧发布正常";
    return true;
}

void VFramePublisher::Stop() {
    ReleaseSlots();
    if (shared_) UnmapViewOfFile(shared_);
    shared_ = nullptr;
    if (mapping_) CloseHandle(mapping_);
    mapping_ = nullptr;
    device_ = nullptr;
    status_ = L"等待 GTA V 窗口";
}
