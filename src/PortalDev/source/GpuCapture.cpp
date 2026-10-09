#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "GpuCapture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>

#include <sstream>

namespace {
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

std::wstring ErrorText(HRESULT error) {
    std::wostringstream text;
    text << L"GPU 捕获失败 0x" << std::hex << static_cast<unsigned>(error);
    return text.str();
}
}

struct GpuCapture::Impl {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IDXGISwapChain1> swap;
    IDirect3DDevice winrtDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    SizeInt32 poolSize{};
    UINT swapWidth = 0;
    UINT swapHeight = 0;
    HWND preview = nullptr;

    void EnsureSwap(UINT width, UINT height) {
        if (swap && swapWidth == width && swapHeight == height) return;
        if (swap) {
            winrt::check_hresult(swap->ResizeBuffers(0, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
        } else {
            auto dxgi = device.as<IDXGIDevice>();
            winrt::com_ptr<IDXGIAdapter> adapter;
            winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
            winrt::com_ptr<IDXGIFactory2> factory;
            winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = width;
            description.Height = height;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Scaling = DXGI_SCALING_STRETCH;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            winrt::check_hresult(factory->CreateSwapChainForHwnd(device.get(), preview,
                &description, nullptr, nullptr, swap.put()));
        }
        swapWidth = width;
        swapHeight = height;
    }
};

bool GpuCapture::Start(HWND gameWindow, HWND previewWindow) {
    Stop();
    try {
        if (!GraphicsCaptureSession::IsSupported()) {
            status_ = L"系统不支持 GPU 窗口捕获";
            return false;
        }
        impl_ = new Impl;
        impl_->preview = previewWindow;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            impl_->device.put(), nullptr, impl_->context.put()));
        auto dxgi = impl_->device.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        impl_->winrtDevice = inspectable.as<IDirect3DDevice>();

        auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForWindow(gameWindow,
            winrt::guid_of<GraphicsCaptureItem>(),
            reinterpret_cast<void**>(winrt::put_abi(impl_->item))));
        impl_->poolSize = impl_->item.Size();
        impl_->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(impl_->winrtDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, impl_->poolSize);
        impl_->session = impl_->pool.CreateCaptureSession(impl_->item);
        impl_->session.StartCapture();
        gameWindow_ = gameWindow;
        status_ = L"GPU 捕获已连接，等待首帧";
        return true;
    } catch (const winrt::hresult_error& error) {
        Stop();
        status_ = ErrorText(error.code());
        return false;
    }
}

bool GpuCapture::Pump() {
    if (!impl_ || !gameWindow_) return false;
    try {
        auto frame = impl_->pool.TryGetNextFrame();
        if (!frame) return false;
        while (auto newer = impl_->pool.TryGetNextFrame()) frame = newer;
        const auto size = frame.ContentSize();
        if (size.Width != impl_->poolSize.Width || size.Height != impl_->poolSize.Height) {
            frame.Close();
            impl_->pool.Recreate(impl_->winrtDevice,
                DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            impl_->poolSize = size;
            return false;
        }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        impl_->EnsureSwap(description.Width, description.Height);
        winrt::com_ptr<ID3D11Texture2D> backBuffer;
        winrt::check_hresult(impl_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()));
        impl_->context->CopyResource(backBuffer.get(), texture.get());
        backBuffer = nullptr;
        winrt::check_hresult(impl_->swap->Present(0, 0));
        width_ = description.Width;
        height_ = description.Height;
        ++frames_;
        status_ = L"GPU 窗口捕获正常";
        return true;
    } catch (const winrt::hresult_error& error) {
        Stop();
        status_ = ErrorText(error.code());
        return false;
    }
}

void GpuCapture::Stop() {
    gameWindow_ = nullptr;
    status_ = L"等待 dev 游戏窗口";
    if (!impl_) return;
    if (impl_->session) impl_->session.Close();
    if (impl_->pool) impl_->pool.Close();
    delete impl_;
    impl_ = nullptr;
}
