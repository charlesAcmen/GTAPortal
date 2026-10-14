#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>

class SAOffscreenConsumer {
public:
    void Start(ID3D11Device* device, ID3D11DeviceContext* context);
    void Stop();
    bool Poll();

    bool Live() const;
    ID3D11Texture2D* Texture() const { return texture_.get(); }
    uint64_t CaptureTime100ns() const { return captureTime100ns_; }
    const std::wstring& Status() const { return status_; }

private:
    bool Connect();
    bool OpenPixels();
    bool EnsureTexture(uint32_t width, uint32_t height);
    void Disconnect();

    ID3D11Device* device_{};
    ID3D11DeviceContext* context_{};
    HANDLE controlMapping_{};
    HANDLE pixelMapping_{};
    struct SharedFramesView;
    SharedFramesView* controlView_{};
    void* pixels_{};
    winrt::com_ptr<ID3D11Texture2D> texture_;
    uint32_t width_{};
    uint32_t height_{};
    LONG generation_{};
    DWORD lastConnectAttempt_{};
    uint64_t lastFrameId_{};
    uint64_t lastFrameTickMs_{};
    uint64_t captureTime100ns_{};
    std::wstring status_ = L"等待 SA 离屏第二相机";
};
