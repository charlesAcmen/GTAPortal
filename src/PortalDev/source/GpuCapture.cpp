#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "GpuCapture.h"
#include "SAFramePublisher.h"
#include "VToSAPublisher.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>

#include <sstream>
#include <algorithm>
#include <cmath>
#include <vector>

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
    winrt::com_ptr<ID3D11Texture2D> clientTexture;
    UINT clientWidth = 0;
    UINT clientHeight = 0;
    HWND preview = nullptr;
    HWND gameWindow = nullptr;
    LARGE_INTEGER qpcFrequency{};
    GpuCapture::Publication publication = GpuCapture::Publication::None;
    SAFramePublisher publisher;
    VToSAPublisher saConsumer;
    std::wstring cropStatus = L"等待客户区裁剪";

    ID3D11Texture2D* CropToClient(ID3D11Texture2D* source, D3D11_TEXTURE2D_DESC& description) {
        // PortalDev has no DPI-awareness manifest. Without overriding the calling
        // thread here, User32 may return virtualized coordinates while WGC/DWM use
        // physical pixels, which makes the bounds check silently reject cropping.
        const DPI_AWARENESS_CONTEXT previousDpi = SetThreadDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        RECT client{};
        POINT clientOrigin{};
        const bool hasClient = gameWindow && GetClientRect(gameWindow, &client) &&
            ClientToScreen(gameWindow, &clientOrigin);
        if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
        if (!hasClient) {
            cropStatus = L"客户区裁剪失败：无法读取窗口坐标";
            return source;
        }
        const LONG width = client.right - client.left;
        const LONG height = client.bottom - client.top;
        if (width <= 0 || height <= 0) {
            cropStatus = L"客户区裁剪失败：客户区尺寸无效";
            return source;
        }

        RECT windowBounds{};
        RECT extendedBounds{};
        const DPI_AWARENESS_CONTEXT previousBoundsDpi = SetThreadDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const bool hasWindowBounds = GetWindowRect(gameWindow, &windowBounds) != FALSE;
        const bool hasExtendedBounds = SUCCEEDED(DwmGetWindowAttribute(
            gameWindow, DWMWA_EXTENDED_FRAME_BOUNDS, &extendedBounds, sizeof(extendedBounds)));
        if (previousBoundsDpi) SetThreadDpiAwarenessContext(previousBoundsDpi);
        if (!hasWindowBounds && !hasExtendedBounds) {
            cropStatus = L"客户区裁剪失败：无法读取窗口边界";
            return source;
        }

        auto sizeError = [&description](const RECT& bounds) {
            return std::abs((bounds.right - bounds.left) - static_cast<LONG>(description.Width)) +
                std::abs((bounds.bottom - bounds.top) - static_cast<LONG>(description.Height));
        };
        RECT captureBounds = hasExtendedBounds ? extendedBounds : windowBounds;
        if (hasWindowBounds && (!hasExtendedBounds || sizeError(windowBounds) < sizeError(extendedBounds)))
            captureBounds = windowBounds;

        const LONG captureWidth = captureBounds.right - captureBounds.left;
        const LONG captureHeight = captureBounds.bottom - captureBounds.top;
        if (captureWidth <= 0 || captureHeight <= 0) {
            cropStatus = L"客户区裁剪失败：捕获边界尺寸无效";
            return source;
        }

        // WGC texture pixels are physical, but its content size can differ by a
        // pixel or by a DPI scale from the bounds reported by older windows.
        const double scaleX = static_cast<double>(description.Width) / captureWidth;
        const double scaleY = static_cast<double>(description.Height) / captureHeight;
        const LONG left = static_cast<LONG>(std::llround((clientOrigin.x - captureBounds.left) * scaleX));
        const LONG top = static_cast<LONG>(std::llround((clientOrigin.y - captureBounds.top) * scaleY));
        const LONG right = static_cast<LONG>(std::llround(
            (clientOrigin.x - captureBounds.left + width) * scaleX));
        const LONG bottom = static_cast<LONG>(std::llround(
            (clientOrigin.y - captureBounds.top + height) * scaleY));
        const LONG croppedWidth = right - left;
        const LONG croppedHeight = bottom - top;
        if (left < 0 || top < 0 || right > static_cast<LONG>(description.Width) ||
            bottom > static_cast<LONG>(description.Height) || croppedWidth <= 0 || croppedHeight <= 0) {
            std::wostringstream status;
            status << L"客户区裁剪失败：纹理 " << description.Width << L'x' << description.Height
                   << L"，边界 " << captureWidth << L'x' << captureHeight
                   << L"，裁剪框 " << left << L',' << top << L'-' << right << L',' << bottom;
            cropStatus = status.str();
            return source;
        }
        if (left == 0 && top == 0 && croppedWidth == static_cast<LONG>(description.Width) &&
            croppedHeight == static_cast<LONG>(description.Height)) {
            cropStatus = L"客户区与捕获纹理相同，无需裁剪";
            return source;
        }

        if (!clientTexture || clientWidth != static_cast<UINT>(croppedWidth) ||
            clientHeight != static_cast<UINT>(croppedHeight)) {
            D3D11_TEXTURE2D_DESC cropped = description;
            cropped.Width = croppedWidth;
            cropped.Height = croppedHeight;
            cropped.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            cropped.CPUAccessFlags = 0;
            cropped.MiscFlags = 0;
            cropped.Usage = D3D11_USAGE_DEFAULT;
            clientTexture = nullptr;
            winrt::check_hresult(device->CreateTexture2D(&cropped, nullptr, clientTexture.put()));
            clientWidth = cropped.Width;
            clientHeight = cropped.Height;
        }
        D3D11_BOX box{};
        box.left = left;
        box.top = top;
        box.front = 0;
        box.right = right;
        box.bottom = bottom;
        box.back = 1;
        context->CopySubresourceRegion(clientTexture.get(), 0, 0, 0, 0, source, 0, &box);
        description.Width = clientWidth;
        description.Height = clientHeight;
        std::wostringstream status;
        status << L"GPU 客户区裁剪正常 " << description.Width << L'x' << description.Height
               << L"（偏移 " << left << L',' << top << L"）";
        cropStatus = status.str();
        return clientTexture.get();
    }

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

bool GpuCapture::Start(HWND gameWindow, HWND previewWindow, Publication publication) {
    Stop();
    try {
        if (!GraphicsCaptureSession::IsSupported()) {
            status_ = L"系统不支持 GPU 窗口捕获";
            return false;
        }
        impl_ = new Impl();
        impl_->preview = previewWindow;
        impl_->gameWindow = gameWindow;
        impl_->publication = publication;
        QueryPerformanceFrequency(&impl_->qpcFrequency);
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            impl_->device.put(), nullptr, impl_->context.put()));
        if (publication == Publication::V) impl_->saConsumer.Start(impl_->device.get(), impl_->context.get());
        if (publication == Publication::Sa && !impl_->publisher.Start(impl_->device.get())) {
            const std::wstring error = impl_->publisher.Status();
            Stop();
            status_ = error;
            return false;
        }
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
        if (impl_->publication == Publication::V) impl_->saConsumer.Poll();
        auto frame = impl_->pool.TryGetNextFrame();
        if (!frame) return false;
        while (auto newer = impl_->pool.TryGetNextFrame()) {
            frame = newer;
            ++droppedFrames_;
        }
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
        ID3D11Texture2D* content = impl_->CropToClient(texture.get(), description);
        if (impl_->publication == Publication::Sa)
            impl_->publisher.Publish(impl_->context.get(), content,
                                     frame.SystemRelativeTime().count());
        if (impl_->publication == Publication::V) impl_->saConsumer.Publish(content);
        if (impl_->preview) {
            impl_->EnsureSwap(description.Width, description.Height);
            winrt::com_ptr<ID3D11Texture2D> backBuffer;
            winrt::check_hresult(impl_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()));
            impl_->context->CopyResource(backBuffer.get(), content);
            backBuffer = nullptr;
            winrt::check_hresult(impl_->swap->Present(0, 0));
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        // WinRT TimeSpan stores QPC-relative time in 100 ns units.
        const int64_t captured100ns = frame.SystemRelativeTime().count();
        const double now100ns = impl_->qpcFrequency.QuadPart > 0
            ? static_cast<double>(now.QuadPart) * 10000000.0 / impl_->qpcFrequency.QuadPart : 0;
        if (captured100ns > 0 && now100ns >= captured100ns &&
            now100ns - captured100ns < 100000000.0) {
            lastAgeMs_ = (now100ns - captured100ns) / 10000.0;
            ageSamples_[ageNext_] = lastAgeMs_;
            ageNext_ = (ageNext_ + 1) % ageSamples_.size();
            if (ageCount_ < ageSamples_.size()) ++ageCount_;
        }
        width_ = description.Width;
        height_ = description.Height;
        ++frames_;
        status_ = impl_->publication == Publication::Sa
            ? impl_->publisher.Status() + L"；WGC 主相机；" + impl_->cropStatus
            : impl_->cropStatus;
        return true;
    } catch (const winrt::hresult_error& error) {
        Stop();
        status_ = ErrorText(error.code());
        return false;
    }
}

double GpuCapture::AgeP95Ms() const {
    if (!ageCount_) return 0;
    std::vector<double> ages(ageSamples_.begin(), ageSamples_.begin() + ageCount_);
    const size_t index = (95 * ageCount_ + 99) / 100 - 1;
    std::nth_element(ages.begin(), ages.begin() + index, ages.end());
    return ages[index];
}

uint64_t GpuCapture::PublishedFrames() const {
    return impl_ ? impl_->publisher.Produced() : 0;
}

uint64_t GpuCapture::OverwrittenFrames() const {
    return impl_ ? impl_->publisher.Overwritten() : 0;
}

uint64_t GpuCapture::PublishDrops() const {
    return impl_ ? impl_->publisher.Dropped() : 0;
}

std::wstring GpuCapture::PublishStatus() const {
    return impl_ ? impl_->publisher.Status() : L"等待 GTA V 窗口";
}

std::wstring GpuCapture::SaConsumerStatus() const {
    return impl_ ? impl_->saConsumer.Status() : L"等待 V 窗口生产者";
}

void GpuCapture::Stop() {
    gameWindow_ = nullptr;
    status_ = L"等待游戏窗口";
    ageCount_ = 0;
    ageNext_ = 0;
    lastAgeMs_ = 0;
    if (!impl_) return;
    if (impl_->session) impl_->session.Close();
    if (impl_->pool) impl_->pool.Close();
    impl_->saConsumer.Stop();
    delete impl_;
    impl_ = nullptr;
}
