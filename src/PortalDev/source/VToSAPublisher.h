#pragma once
#include <d3d11.h>
#include <windows.h>
#include <winrt/base.h>
#include <array>
#include <cstdint>
#include <string>
#include "../../VToSAFrames.h"

class VToSAPublisher {
public:
    void Start(ID3D11Device*, ID3D11DeviceContext*);
    void Poll();
    void Publish(ID3D11Texture2D*);
    void Stop();
    std::wstring Status() const;
private:
    struct ReadbackSlot {
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::com_ptr<ID3D11Query> done;
        uint64_t frameId = 0, captureTime100ns = 0;
        bool pending = false;
    };
    bool CreateControlMapping();
    bool PrepareReadback(UINT, UINT);
    void ReleaseReadback();
    int ClaimMailboxSlot();
    void CompleteReadbacks();
    void Error(v_to_sa::Stage, HRESULT);
    HANDLE controlMapping_ = nullptr, pixelMapping_ = nullptr;
    v_to_sa::SharedFrames* shared_ = nullptr;
    void* pixels_ = nullptr;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    std::array<ReadbackSlot, v_to_sa::kSlotCount> readbacks_{};
    UINT width_ = 0, height_ = 0;
    LONG generationSeed_ = 0;
    uint64_t frameId_ = 0;
    unsigned nextReadback_ = 0;
};
