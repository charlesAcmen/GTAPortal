#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "../PortalDev/source/SAOffscreenConsumer.h"
#include "../SAToVOffscreenFrames.h"

int wmain() {
    constexpr uint32_t width = 41, height = 23;
    constexpr uint32_t color = 0xFF6A3CB5;
    constexpr uint64_t slotBytes = static_cast<uint64_t>(width) * height * 4;
    HANDLE control = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(sa_to_v_offscreen::SharedFrames), sa_to_v_offscreen::kMappingName);
    if (!control) return 1;
    auto* shared = static_cast<sa_to_v_offscreen::SharedFrames*>(MapViewOfFile(control,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(sa_to_v_offscreen::SharedFrames)));
    if (!shared) return 2;
    ZeroMemory(shared, sizeof(sa_to_v_offscreen::SharedFrames));
    shared->version = sa_to_v_offscreen::kVersion;
    shared->size = sizeof(sa_to_v_offscreen::SharedFrames);
    shared->ownerPid = GetCurrentProcessId();
    shared->generation = 1;
    shared->width = width;
    shared->height = height;
    shared->rowBytes = width * 4;
    shared->format = sa_to_v_offscreen::kFormatBgra8;
    shared->slotBytes = slotBytes;
    swprintf_s(shared->pixelMappingName, L"%s.%lu.test",
        sa_to_v_offscreen::kPixelMappingPrefix, GetCurrentProcessId());
    MemoryBarrier();
    shared->magic = sa_to_v_offscreen::kMagic;

    HANDLE pixelsMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, static_cast<DWORD>(slotBytes * sa_to_v_offscreen::kSlotCount),
        shared->pixelMappingName);
    if (!pixelsMapping) return 3;
    void* pixels = MapViewOfFile(pixelsMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        static_cast<SIZE_T>(slotBytes * sa_to_v_offscreen::kSlotCount));
    if (!pixels) return 4;
    auto* destination = reinterpret_cast<uint32_t*>(pixels);
    std::fill(destination, destination + width * height, color);
    shared->slots[0].frameId = 7;
    shared->slots[0].readyTickMs = GetTickCount64();
    shared->slots[0].captureTime100ns = 1234567;
    MemoryBarrier();
    InterlockedExchange(&shared->slots[0].state, sa_to_v_offscreen::kReady);

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()))) return 5;
    SAOffscreenConsumer consumer;
    consumer.Start(device.get(), context.get());
    if (!consumer.Poll() || !consumer.Live() || !consumer.Texture() ||
        consumer.CaptureTime100ns() != 1234567) return 6;

    D3D11_TEXTURE2D_DESC description{};
    consumer.Texture()->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&description, nullptr, staging.put()))) return 7;
    context->CopyResource(staging.get(), consumer.Texture());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return 8;
    const uint32_t copied = *static_cast<const uint32_t*>(mapped.pData);
    context->Unmap(staging.get(), 0);
    if (copied != color || shared->consumed != 1 || shared->slots[0].state !=
        sa_to_v_offscreen::kFree) return 9;
    std::printf("SA offscreen CPU bridge smoke passed: %ux%u frame=%llu color=%08X\n",
        width, height, static_cast<unsigned long long>(7), copied);
    consumer.Stop();
    UnmapViewOfFile(pixels);
    CloseHandle(pixelsMapping);
    UnmapViewOfFile(shared);
    CloseHandle(control);
    return 0;
}
