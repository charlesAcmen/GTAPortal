#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static int Failure(int step, HRESULT hr) {
    std::printf("bridge step %d failed: 0x%08X\n", step, static_cast<unsigned>(hr));
    return step;
}

int main() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Failure(1, hr);
    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(0, &adapter);
    if (FAILED(hr)) return Failure(2, hr);

    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> context;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &d11, nullptr, &context);
    if (FAILED(hr)) return Failure(3, hr);
    ComPtr<ID3D12Device> d12;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
    if (FAILED(hr)) return Failure(4, hr);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 16;
    desc.Height = 16;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                 D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> shared;
    hr = d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                      IID_PPV_ARGS(&shared));
    if (FAILED(hr)) return Failure(5, hr);
    HANDLE textureHandle = nullptr;
    hr = d12->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr, &textureHandle);
    if (FAILED(hr)) return Failure(6, hr);
    ComPtr<ID3D11Device1> d11_1;
    hr = d11.As(&d11_1);
    if (FAILED(hr)) return Failure(7, hr);
    ComPtr<ID3D11Texture2D> opened;
    hr = d11_1->OpenSharedResource1(textureHandle, IID_PPV_ARGS(&opened));
    CloseHandle(textureHandle);
    if (FAILED(hr)) return Failure(8, hr);

    std::array<unsigned, 256> pixels{};
    pixels.fill(0xFF2468AC);
    D3D11_TEXTURE2D_DESC sourceDesc{};
    sourceDesc.Width = 16;
    sourceDesc.Height = 16;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA sourceData{pixels.data(), 16 * 4, 0};
    ComPtr<ID3D11Texture2D> source;
    hr = d11->CreateTexture2D(&sourceDesc, &sourceData, &source);
    if (FAILED(hr)) return Failure(9, hr);

    ComPtr<ID3D12Fence> fence;
    hr = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return Failure(10, hr);
    HANDLE fenceHandle = nullptr;
    hr = d12->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (FAILED(hr)) return Failure(11, hr);
    ComPtr<ID3D11Device5> d11_5;
    hr = d11.As(&d11_5);
    if (FAILED(hr)) return Failure(12, hr);
    ComPtr<ID3D11Fence> d11Fence;
    hr = d11_5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&d11Fence));
    CloseHandle(fenceHandle);
    if (FAILED(hr)) return Failure(13, hr);
    ComPtr<ID3D11DeviceContext4> context4;
    hr = context.As(&context4);
    if (FAILED(hr)) return Failure(14, hr);
    context->CopyResource(opened.Get(), source.Get());
    hr = context4->Signal(d11Fence.Get(), 1);
    if (FAILED(hr)) return Failure(15, hr);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ComPtr<ID3D12CommandQueue> queue;
    hr = d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return Failure(16, hr);
    hr = queue->Wait(fence.Get(), 1);
    if (FAILED(hr)) return Failure(17, hr);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    d12->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = bytes;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    hr = d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&readback));
    if (FAILED(hr)) return Failure(18, hr);
    ComPtr<ID3D12CommandAllocator> allocator;
    hr = d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) return Failure(19, hr);
    ComPtr<ID3D12GraphicsCommandList> list;
    hr = d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                nullptr, IID_PPV_ARGS(&list));
    if (FAILED(hr)) return Failure(20, hr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = shared.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = shared.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    hr = list->Close();
    if (FAILED(hr)) return Failure(21, hr);
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    hr = queue->Signal(fence.Get(), 2);
    if (FAILED(hr)) return Failure(22, hr);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = fence->SetEventOnCompletion(2, done);
    if (FAILED(hr) || WaitForSingleObject(done, 5000) != WAIT_OBJECT_0) return Failure(23, hr);
    CloseHandle(done);
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    hr = readback->Map(0, &range, &mapped);
    if (FAILED(hr)) return Failure(24, hr);
    const unsigned color = *static_cast<const unsigned*>(mapped);
    readback->Unmap(0, nullptr);
    std::printf("D3D11 -> D3D12 GPU bridge color: %08X\n", color);
    return color == 0xFF2468AC ? 0 : 25;
}
