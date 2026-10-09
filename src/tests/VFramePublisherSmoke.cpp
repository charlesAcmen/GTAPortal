#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <winrt/base.h>

#include <array>
#include <cstdio>

#include "../PortalDev/source/VFramePublisher.h"
#include "../PortalVFrames.h"

int wmain() {
    winrt::com_ptr<ID3D11Device> writerDevice;
    winrt::com_ptr<ID3D11DeviceContext> writerContext;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                  nullptr, 0, D3D11_SDK_VERSION, writerDevice.put(),
                                  nullptr, writerContext.put()))) return 1;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 16;
    description.Height = 16;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    std::array<uint32_t, 16 * 16> pixels{};
    pixels.fill(0xFF11AA77);
    D3D11_SUBRESOURCE_DATA data{pixels.data(), 16 * 4, 0};
    winrt::com_ptr<ID3D11Texture2D> source;
    if (FAILED(writerDevice->CreateTexture2D(&description, &data, source.put()))) return 2;

    VFramePublisher publisher;
    if (!publisher.Start(writerDevice.get())) return 3;
    if (!publisher.Publish(writerContext.get(), source.get(), 123456)) return 4;

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                      portal_v_frames::kMappingName);
    if (!mapping) return 5;
    auto* frames = static_cast<portal_v_frames::SharedFrames*>(MapViewOfFile(
        mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(portal_v_frames::SharedFrames)));
    if (!frames) return 6;
    int ready = -1;
    for (int index = 0; index < portal_v_frames::kSlotCount; ++index) {
        if (InterlockedCompareExchange(&frames->slots[index].state,
                                       portal_v_frames::kReading,
                                       portal_v_frames::kReady) == portal_v_frames::kReady) {
            ready = index;
            break;
        }
    }
    if (ready < 0 || frames->magic != portal_v_frames::kMagic) return 7;

    winrt::com_ptr<ID3D11Device> readerDevice;
    winrt::com_ptr<ID3D11DeviceContext> readerContext;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                  nullptr, 0, D3D11_SDK_VERSION, readerDevice.put(),
                                  nullptr, readerContext.put()))) return 8;
    auto readerDevice1 = readerDevice.as<ID3D11Device1>();
    winrt::com_ptr<ID3D11Texture2D> sharedTexture;
    if (FAILED(readerDevice1->OpenSharedResourceByName(
            frames->slots[ready].resourceName, DXGI_SHARED_RESOURCE_READ,
            __uuidof(ID3D11Texture2D), sharedTexture.put_void()))) return 9;
    auto mutex = sharedTexture.as<IDXGIKeyedMutex>();
    if (mutex->AcquireSync(1, 1000) != S_OK) return 10;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(readerDevice->CreateTexture2D(&description, nullptr, staging.put()))) return 11;
    readerContext->CopyResource(staging.get(), sharedTexture.get());
    if (FAILED(mutex->ReleaseSync(0))) return 12;
    readerContext->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(readerContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return 13;
    const uint32_t color = *static_cast<const uint32_t*>(mapped.pData);
    readerContext->Unmap(staging.get(), 0);
    InterlockedExchange(&frames->slots[ready].state, portal_v_frames::kFree);
    if (color != 0xFF11AA77) return 14;

    for (int index = 0; index < 10; ++index) publisher.Publish(writerContext.get(), source.get(), 123456 + index);
    if (publisher.Produced() < 2 || publisher.Overwritten() == 0) return 15;
    std::printf("V shared texture smoke passed: produced=%llu overwritten=%llu\n",
                static_cast<unsigned long long>(publisher.Produced()),
                static_cast<unsigned long long>(publisher.Overwritten()));
    UnmapViewOfFile(frames);
    CloseHandle(mapping);
    return 0;
}
