#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "VToSAPublisher.h"

#include <d3dcompiler.h>
#include <dxgi.h>
#include <limits>
#include <sstream>

namespace {
constexpr char kShader[] = R"(
Texture2D image : register(t0);
SamplerState linearSampler : register(s0);
struct VertexOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VertexOut VS(uint id : SV_VertexID) {
    VertexOut result;
    float2 uv = float2((id << 1) & 2, id & 2);
    result.uv = uv;
    result.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    return result;
}
float4 PS(VertexOut input) : SV_Target { return image.Sample(linearSampler, input.uv); }
)";
}

void VToSAPublisher::Start(ID3D11Device* device, ID3D11DeviceContext* context) {
    Stop();
    device_.copy_from(device);
    context_.copy_from(context);
}

void VToSAPublisher::ReleaseSlots() {
    if (context_) {
        context_->Flush();
        const DWORD start = GetTickCount();
        for (auto& slot : slots_) {
            while (slot.pending && GetTickCount() - start < 200 &&
                context_->GetData(slot.done.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE) Sleep(1);
        }
    }
    for (auto& slot : slots_) slot = {};
    generation_ = 0;
}

void VToSAPublisher::Stop() {
    const LONG openedGeneration = generation_;
    if (context_) context_->Flush();
    ReleaseSlots();
    if (shared_) {
        if (openedGeneration == shared_->generation) {
            for (auto& slot : shared_->slots)
                InterlockedCompareExchange(&slot.state, v_to_sa::kFree, v_to_sa::kWriting);
        }
        InterlockedExchange(&shared_->readerGeneration, 0);
        InterlockedExchange(&shared_->readerPid, 0);
    }
    sourceView_ = nullptr;
    sourceCopy_ = nullptr;
    vertexShader_ = nullptr;
    pixelShader_ = nullptr;
    sampler_ = nullptr;
    if (shared_) UnmapViewOfFile(shared_);
    if (mapping_) CloseHandle(mapping_);
    shared_ = nullptr;
    mapping_ = nullptr;
    context_ = nullptr;
    device_ = nullptr;
    sourceWidth_ = sourceHeight_ = 0;
}

void VToSAPublisher::Error(v_to_sa::Stage stage, HRESULT hr) {
    if (shared_) {
        InterlockedExchange(&shared_->lastError, hr);
        InterlockedExchange(&shared_->stage, stage);
    }
}

bool VToSAPublisher::Attach() {
    if (shared_) {
        bool ownerAlive = false;
        if (shared_->ownerPid) {
            HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, shared_->ownerPid);
            ownerAlive = owner && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT;
            if (owner) CloseHandle(owner);
        }
        if (ownerAlive && shared_->magic == v_to_sa::kMagic &&
            shared_->version == v_to_sa::kVersion && shared_->size == sizeof(*shared_)) return true;
        ReleaseSlots();
        InterlockedExchange(&shared_->readerGeneration, 0);
        InterlockedExchange(&shared_->readerPid, 0);
        UnmapViewOfFile(shared_);
        CloseHandle(mapping_);
        shared_ = nullptr;
        mapping_ = nullptr;
    }
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, v_to_sa::kMappingName);
    if (!mapping_) return false;
    shared_ = static_cast<v_to_sa::SharedFrames*>(MapViewOfFile(mapping_,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(v_to_sa::SharedFrames)));
    bool ownerAlive = false;
    if (shared_ && shared_->ownerPid) {
        HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, shared_->ownerPid);
        ownerAlive = owner && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT;
        if (owner) CloseHandle(owner);
    }
    if (!shared_ || !ownerAlive || shared_->magic != v_to_sa::kMagic ||
        shared_->version != v_to_sa::kVersion || shared_->size != sizeof(*shared_)) {
        if (shared_) UnmapViewOfFile(shared_);
        CloseHandle(mapping_);
        shared_ = nullptr;
        mapping_ = nullptr;
        return false;
    }
    return true;
}

bool VToSAPublisher::OpenSlots() {
    if (!shared_ || shared_->generation == 0) return false;
    if (shared_->stage != v_to_sa::kTexturesReady && shared_->stage != v_to_sa::kOpenError &&
        shared_->stage != v_to_sa::kAdapterError) return false;
    const DWORD now = GetTickCount();
    if (lastOpenAttempt_ && now - lastOpenAttempt_ < 2000) return false;
    lastOpenAttempt_ = now;
    ReleaseSlots();
    for (const auto& slot : shared_->slots) if (!slot.sharedHandle) return false;
    auto dxgi = device_.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    HRESULT hr = dxgi->GetAdapter(adapter.put());
    if (SUCCEEDED(hr)) hr = adapter->GetDesc(&adapterDesc);
    if (FAILED(hr)) { Error(v_to_sa::kAdapterError, hr); return false; }
    if ((shared_->adapterLuidLow || shared_->adapterLuidHigh) &&
        (shared_->adapterLuidLow != adapterDesc.AdapterLuid.LowPart ||
         shared_->adapterLuidHigh != adapterDesc.AdapterLuid.HighPart)) {
        Error(v_to_sa::kAdapterError, DXGI_ERROR_UNSUPPORTED);
        return false;
    }
    for (int i = 0; i < v_to_sa::kSlotCount; ++i) {
        const auto& metadata = shared_->slots[i];
        if (!metadata.sharedHandle) { Error(v_to_sa::kOpenError, E_HANDLE); ReleaseSlots(); return false; }
        hr = device_->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(metadata.sharedHandle)),
            __uuidof(ID3D11Texture2D), slots_[i].texture.put_void());
        D3D11_TEXTURE2D_DESC desc{};
        if (SUCCEEDED(hr)) slots_[i].texture->GetDesc(&desc);
        if (SUCCEEDED(hr) && (desc.Width != v_to_sa::kWidth || desc.Height != v_to_sa::kHeight ||
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)) hr = E_INVALIDARG;
        if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(slots_[i].texture.get(), nullptr,
            slots_[i].target.put());
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        if (SUCCEEDED(hr)) hr = device_->CreateQuery(&query, slots_[i].done.put());
        if (FAILED(hr)) { Error(v_to_sa::kOpenError, hr); ReleaseSlots(); return false; }
    }
    // A previous capture session may have stopped while a slot was marked Writing.
    for (auto& slot : shared_->slots)
        InterlockedCompareExchange(&slot.state, v_to_sa::kFree, v_to_sa::kWriting);
    generation_ = shared_->generation;
    InterlockedExchange(&shared_->readerPid, GetCurrentProcessId());
    InterlockedExchange(&shared_->readerGeneration, generation_);
    InterlockedExchange(&shared_->lastError, S_OK);
    InterlockedExchange(&shared_->stage, v_to_sa::kOpenedByTool);
    return true;
}

bool VToSAPublisher::MakeShaders() {
    if (vertexShader_ && pixelShader_) return true;
    winrt::com_ptr<ID3DBlob> vertex, pixel;
    HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr,
        "VS", "vs_4_0", 0, 0, vertex.put(), nullptr);
    if (SUCCEEDED(hr)) hr = D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr,
        "PS", "ps_4_0", 0, 0, pixel.put(), nullptr);
    if (SUCCEEDED(hr)) hr = device_->CreateVertexShader(vertex->GetBufferPointer(),
        vertex->GetBufferSize(), nullptr, vertexShader_.put());
    if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(pixel->GetBufferPointer(),
        pixel->GetBufferSize(), nullptr, pixelShader_.put());
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = device_->CreateSamplerState(&sampler, sampler_.put());
    if (FAILED(hr)) { Error(v_to_sa::kShaderError, hr); return false; }
    return true;
}

bool VToSAPublisher::PrepareSource(ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1) {
        Error(v_to_sa::kShaderError, E_INVALIDARG);
        return false;
    }
    if (!sourceCopy_ || sourceWidth_ != desc.Width || sourceHeight_ != desc.Height) {
        sourceView_ = nullptr;
        sourceCopy_ = nullptr;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, sourceCopy_.put());
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(sourceCopy_.get(), nullptr,
            sourceView_.put());
        if (FAILED(hr)) { Error(v_to_sa::kShaderError, hr); return false; }
        sourceWidth_ = desc.Width;
        sourceHeight_ = desc.Height;
    }
    context_->CopyResource(sourceCopy_.get(), source);
    return true;
}

void VToSAPublisher::Poll() {
    if (!device_ || !Attach()) return;
    if (generation_ != shared_->generation) {
        ReleaseSlots();
        InterlockedExchange(&shared_->readerGeneration, 0);
    }
    if (!generation_) return;
    for (int i = 0; i < v_to_sa::kSlotCount; ++i) {
        auto& gpu = slots_[i];
        if (!gpu.pending) continue;
        const HRESULT hr = context_->GetData(gpu.done.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) continue;
        gpu.pending = false;
        if (FAILED(hr)) { Error(v_to_sa::kShaderError, hr); InterlockedExchange(&shared_->slots[i].state, v_to_sa::kFree); continue; }
        shared_->slots[i].readyTickMs = GetTickCount64();
        MemoryBarrier();
        InterlockedExchange(&shared_->slots[i].state, v_to_sa::kReady);
        InterlockedIncrement(&shared_->produced);
        InterlockedExchange(&shared_->stage, v_to_sa::kStreaming);
    }
}

void VToSAPublisher::Publish(ID3D11Texture2D* source) {
    Poll();
    if (!shared_ || !source) return;
    if (!generation_ && !OpenSlots()) return;
    if (!MakeShaders() || !PrepareSource(source)) return;
    int chosen = -1;
    LONG previous = v_to_sa::kFree;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (int i = 0; i < v_to_sa::kSlotCount; ++i) {
        auto& slot = shared_->slots[i];
        const LONG state = InterlockedCompareExchange(&slot.state, 0, 0);
        if (state == v_to_sa::kFree) { chosen = i; previous = state; break; }
        if (state == v_to_sa::kReady && slot.readyTickMs < oldest) {
            chosen = i; previous = state; oldest = slot.readyTickMs;
        }
    }
    if (chosen < 0 || InterlockedCompareExchange(&shared_->slots[chosen].state,
        v_to_sa::kWriting, previous) != previous) {
        InterlockedIncrement(&shared_->dropped);
        return;
    }
    auto& gpu = slots_[chosen];
    ID3D11RenderTargetView* target = gpu.target.get();
    ID3D11ShaderResourceView* sourceView = sourceView_.get();
    ID3D11SamplerState* sampler = sampler_.get();
    D3D11_VIEWPORT viewport{0, 0, static_cast<FLOAT>(v_to_sa::kWidth),
        static_cast<FLOAT>(v_to_sa::kHeight), 0, 1};
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, &sourceView);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* noSource = nullptr;
    ID3D11RenderTargetView* noTarget = nullptr;
    context_->PSSetShaderResources(0, 1, &noSource);
    context_->OMSetRenderTargets(1, &noTarget, nullptr);
    context_->End(gpu.done.get());
    context_->Flush();
    shared_->slots[chosen].frameId = ++frameId_;
    gpu.pending = true;
}

std::wstring VToSAPublisher::Status() const {
    if (!shared_) return L"等待 SA 消费端";
    std::wostringstream text;
    text << L"SA 消费端：";
    switch (shared_->stage) {
    case v_to_sa::kStarting: text << L"等待共享纹理"; break;
    case v_to_sa::kTexturesReady: text << L"共享纹理已创建"; break;
    case v_to_sa::kOpenedByTool: text << L"已打开纹理，等待帧"; break;
    case v_to_sa::kStreaming: text << L"GPU 帧传输中"; break;
    case v_to_sa::kTextureError: text << L"SA 创建纹理失败"; break;
    case v_to_sa::kOpenError: text << L"跨进程打开纹理失败"; break;
    case v_to_sa::kAdapterError: text << L"显卡不匹配/查询失败"; break;
    case v_to_sa::kShaderError: text << L"GPU 转换失败"; break;
    case v_to_sa::kDrawError: text << L"SA 世界矩形绘制失败"; break;
    default: text << L"未知阶段"; break;
    }
    text << L"  写入 " << shared_->produced << L" / 绘制 " << shared_->displayed
         << L" / 跳过 " << shared_->dropped;
    if (shared_->lastError) text << L"  HRESULT 0x" << std::hex << static_cast<unsigned>(shared_->lastError);
    return text.str();
}
