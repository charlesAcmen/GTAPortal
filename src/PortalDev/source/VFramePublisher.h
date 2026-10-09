#pragma once

#include <windows.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <string>

#include "../../PortalVFrames.h"

class VFramePublisher {
public:
    VFramePublisher() = default;
    ~VFramePublisher();
    VFramePublisher(const VFramePublisher&) = delete;
    VFramePublisher& operator=(const VFramePublisher&) = delete;

    bool Start(ID3D11Device* device);
    bool Publish(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                 int64_t compositorTime100ns);
    void Stop();
    const std::wstring& Status() const { return status_; }
    uint64_t Produced() const { return produced_; }
    uint64_t Overwritten() const { return overwritten_; }
    uint64_t Dropped() const { return dropped_; }

private:
    struct TextureSlot {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<IDXGIKeyedMutex> mutex;
        HANDLE handle = nullptr;
    };

    bool CreateSlots(UINT width, UINT height);
    void ReleaseSlots();
    void SetError(const wchar_t* action, HRESULT result);

    winrt::com_ptr<ID3D11Device> device_;
    HANDLE mapping_ = nullptr;
    portal_v_frames::SharedFrames* shared_ = nullptr;
    std::array<TextureSlot, portal_v_frames::kSlotCount> slots_{};
    UINT width_ = 0;
    UINT height_ = 0;
    LONG generation_ = 0;
    uint64_t nextFrameId_ = 0;
    uint64_t produced_ = 0;
    uint64_t overwritten_ = 0;
    uint64_t dropped_ = 0;
    std::wstring status_ = L"等待 GTA V 窗口";
};
