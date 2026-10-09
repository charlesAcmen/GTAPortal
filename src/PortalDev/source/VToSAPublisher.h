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
    void Start(ID3D11Device* device, ID3D11DeviceContext* context);
    void Poll();
    void Publish(ID3D11Texture2D* source);
    void Stop();
    std::wstring Status() const;

private:
    struct Slot {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11RenderTargetView> target;
        winrt::com_ptr<ID3D11Query> done;
        bool pending = false;
    };
    bool Attach();
    bool OpenSlots();
    bool MakeShaders();
    bool PrepareSource(ID3D11Texture2D* source);
    void ReleaseSlots();
    void Error(v_to_sa::Stage stage, HRESULT hr);

    HANDLE mapping_ = nullptr;
    v_to_sa::SharedFrames* shared_ = nullptr;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    std::array<Slot, v_to_sa::kSlotCount> slots_{};
    winrt::com_ptr<ID3D11VertexShader> vertexShader_;
    winrt::com_ptr<ID3D11PixelShader> pixelShader_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11Texture2D> sourceCopy_;
    winrt::com_ptr<ID3D11ShaderResourceView> sourceView_;
    UINT sourceWidth_ = 0;
    UINT sourceHeight_ = 0;
    LONG generation_ = 0;
    uint64_t frameId_ = 0;
    DWORD lastOpenAttempt_ = 0;
};
