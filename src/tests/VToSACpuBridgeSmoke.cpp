#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <winrt/base.h>
#include <array>
#include <cstdio>
#include "../PortalDev/source/VToSAPublisher.h"
#include "../VToSAFrames.h"

int wmain() {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()))) return 1;
    constexpr UINT width = 37, height = 19;
    constexpr uint32_t color = 0xFF3579BD;
    std::array<uint32_t, width * height> pixels{}; pixels.fill(color);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{pixels.data(),width*4,0};
    winrt::com_ptr<ID3D11Texture2D> source;
    if(FAILED(device->CreateTexture2D(&desc,&data,source.put())))return 2;
    VToSAPublisher publisher;publisher.Start(device.get(),context.get());publisher.Publish(source.get());
    HANDLE control=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,v_to_sa::kMappingName);
    if(!control)return 3;
    auto* shared=static_cast<v_to_sa::SharedFrames*>(MapViewOfFile(control,
        FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(v_to_sa::SharedFrames)));
    if(!shared)return 4;
    const DWORD start=GetTickCount();
    while(!shared->produced&&GetTickCount()-start<2000){publisher.Poll();Sleep(1);}
    if(!shared->produced||shared->width!=width||shared->height!=height||shared->rowBytes!=width*4)return 5;
    int newest=-1;
    for(int i=0;i<v_to_sa::kSlotCount;++i)if(InterlockedCompareExchange(&shared->slots[i].state,
        v_to_sa::kReading,v_to_sa::kReady)==v_to_sa::kReady){newest=i;break;}
    if(newest<0)return 6;
    HANDLE pixelMap=OpenFileMappingW(FILE_MAP_READ,FALSE,shared->pixelMappingName);
    if(!pixelMap)return 7;
    void* view=MapViewOfFile(pixelMap,FILE_MAP_READ,0,0,
        static_cast<SIZE_T>(shared->slotBytes*v_to_sa::kSlotCount));
    if(!view)return 8;
    const uint32_t copied=*reinterpret_cast<const uint32_t*>(v_to_sa::SlotPixels(view,shared->slotBytes,newest));
    InterlockedExchange(&shared->slots[newest].state,v_to_sa::kFree);
    if(copied!=color)return 9;
    for(int i=0;i<8;++i){publisher.Publish(source.get());Sleep(2);publisher.Poll();}
    if(shared->produced<2||shared->overwritten==0)return 10;
    std::printf("V->SA CPU mailbox smoke passed: %ux%u produced=%ld overwritten=%ld dropped=%ld\n",
        shared->width,shared->height,shared->produced,shared->overwritten,shared->dropped);
    UnmapViewOfFile(view);CloseHandle(pixelMap);UnmapViewOfFile(shared);CloseHandle(control);return 0;
}
