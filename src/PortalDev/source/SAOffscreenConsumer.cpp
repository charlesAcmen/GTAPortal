#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "SAOffscreenConsumer.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "../../SAToVOffscreenFrames.h"

struct SAOffscreenConsumer::SharedFramesView : sa_to_v_offscreen::SharedFrames {};

namespace {

bool ProcessAlive(uint32_t processId) {
    if (!processId) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

std::wstring StageStatus(LONG stage, LONG error) {
    const wchar_t* label = L"未知阶段";
    switch (stage) {
    case sa_to_v_offscreen::kStarting: label = L"正在启动"; break;
    case sa_to_v_offscreen::kWaitingForCamera: label = L"等待启用 Portal 相机"; break;
    case sa_to_v_offscreen::kRendering: label = L"正在渲染首帧"; break;
    case sa_to_v_offscreen::kStreaming: label = L"已渲染，等待新帧"; break;
    case sa_to_v_offscreen::kRenderError: label = L"渲染失败"; break;
    case sa_to_v_offscreen::kReadbackError: label = L"画面读回失败"; break;
    case sa_to_v_offscreen::kMappingError: label = L"共享通道失败"; break;
    }
    std::wostringstream text;
    text << L"SA 第二相机：" << label;
    if (error) {
        text << L"（错误 0x" << std::uppercase << std::hex
             << static_cast<uint32_t>(error) << L"）";
    }
    return text.str();
}

} // namespace

void SAOffscreenConsumer::Start(ID3D11Device* device, ID3D11DeviceContext* context) {
    Stop();
    device_ = device;
    context_ = context;
}

void SAOffscreenConsumer::Disconnect() {
    if (pixels_) UnmapViewOfFile(pixels_);
    if (pixelMapping_) CloseHandle(pixelMapping_);
    if (controlView_) UnmapViewOfFile(controlView_);
    if (controlMapping_) CloseHandle(controlMapping_);
    pixels_ = nullptr;
    pixelMapping_ = nullptr;
    controlView_ = nullptr;
    controlMapping_ = nullptr;
    texture_ = nullptr;
    width_ = height_ = 0;
    generation_ = 0;
    lastFrameId_ = 0;
    lastFrameTickMs_ = 0;
    captureTime100ns_ = 0;
}

void SAOffscreenConsumer::Stop() {
    Disconnect();
    device_ = nullptr;
    context_ = nullptr;
    status_ = L"等待 SA 离屏第二相机";
}

bool SAOffscreenConsumer::Connect() {
    if (controlView_) return true;
    const DWORD now = GetTickCount();
    if (lastConnectAttempt_ && now - lastConnectAttempt_ < 500) return false;
    lastConnectAttempt_ = now;
    controlMapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
        sa_to_v_offscreen::kMappingName);
    if (!controlMapping_) {
        status_ = L"等待 SA 离屏第二相机控制通道";
        return false;
    }
    controlView_ = static_cast<SharedFramesView*>(MapViewOfFile(controlMapping_,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(sa_to_v_offscreen::SharedFrames)));
    if (!controlView_) {
        Disconnect();
        status_ = L"SA 离屏控制通道映射失败";
        return false;
    }
    if (controlView_->magic != sa_to_v_offscreen::kMagic ||
        controlView_->version != sa_to_v_offscreen::kVersion ||
        controlView_->size != sizeof(sa_to_v_offscreen::SharedFrames)) {
        Disconnect();
        status_ = L"SA 离屏控制通道版本不匹配";
        return false;
    }
    InterlockedExchange(&controlView_->consumerPid, GetCurrentProcessId());
    return true;
}

bool SAOffscreenConsumer::OpenPixels() {
    if (!controlView_) return false;
    const LONG generation = InterlockedCompareExchange(&controlView_->generation, 0, 0);
    if (pixels_ && generation == generation_) return true;
    if (pixels_) UnmapViewOfFile(pixels_);
    if (pixelMapping_) CloseHandle(pixelMapping_);
    pixels_ = nullptr;
    pixelMapping_ = nullptr;
    generation_ = generation;
    if (!generation || !controlView_->pixelMappingName[0]) return false;
    pixelMapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
        controlView_->pixelMappingName);
    if (!pixelMapping_) return false;
    const uint64_t totalBytes = controlView_->slotBytes * sa_to_v_offscreen::kSlotCount;
    pixels_ = MapViewOfFile(pixelMapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        static_cast<SIZE_T>(totalBytes));
    if (!pixels_) {
        CloseHandle(pixelMapping_);
        pixelMapping_ = nullptr;
        return false;
    }
    return true;
}

bool SAOffscreenConsumer::EnsureTexture(uint32_t width, uint32_t height) {
    if (texture_ && width_ == width && height_ == height) return true;
    texture_ = nullptr;
    width_ = height_ = 0;
    if (!device_ || !width || !height || width > sa_to_v_offscreen::kMaxDimension ||
        height > sa_to_v_offscreen::kMaxDimension) return false;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&description, nullptr, texture_.put()))) return false;
    width_ = width;
    height_ = height;
    return true;
}

bool SAOffscreenConsumer::Poll() {
    if (!device_ || !context_ || !Connect()) return false;
    if (controlView_->magic != sa_to_v_offscreen::kMagic) {
        status_ = L"SA 第二相机控制通道无效";
        return false;
    }
    if (!ProcessAlive(controlView_->ownerPid)) {
        Disconnect();
        status_ = L"SA 未运行，等待重新连接";
        return false;
    }
    if (!OpenPixels()) {
        status_ = StageStatus(
            InterlockedCompareExchange(&controlView_->stage, 0, 0),
            InterlockedCompareExchange(&controlView_->lastError, 0, 0));
        return false;
    }
    const uint32_t width = controlView_->width;
    const uint32_t height = controlView_->height;
    const uint32_t rowBytes = controlView_->rowBytes;
    if (controlView_->format != sa_to_v_offscreen::kFormatBgra8 ||
        rowBytes != width * sa_to_v_offscreen::kBytesPerPixel ||
        controlView_->slotBytes < static_cast<uint64_t>(rowBytes) * height ||
        !EnsureTexture(width, height)) {
        status_ = L"SA 第二相机帧描述无效";
        return false;
    }

    int newest = -1;
    uint64_t newestId = lastFrameId_;
    for (int index = 0; index < sa_to_v_offscreen::kSlotCount; ++index) {
        auto& slot = controlView_->slots[index];
        if (InterlockedCompareExchange(&slot.state, 0, 0) == sa_to_v_offscreen::kReady &&
            slot.frameId > newestId) {
            newest = index;
            newestId = slot.frameId;
        }
    }
    if (newest < 0 || InterlockedCompareExchange(&controlView_->slots[newest].state,
            sa_to_v_offscreen::kReading, sa_to_v_offscreen::kReady) !=
        sa_to_v_offscreen::kReady) {
        if (!Live()) {
            status_ = StageStatus(
                InterlockedCompareExchange(&controlView_->stage, 0, 0),
                InterlockedCompareExchange(&controlView_->lastError, 0, 0));
        }
        return false;
    }

    auto& slot = controlView_->slots[newest];
    const auto* source = sa_to_v_offscreen::SlotPixels(pixels_, controlView_->slotBytes, newest);
    context_->UpdateSubresource(texture_.get(), 0, nullptr, source, rowBytes, 0);
    lastFrameId_ = slot.frameId;
    lastFrameTickMs_ = GetTickCount64();
    captureTime100ns_ = slot.captureTime100ns;
    MemoryBarrier();
    InterlockedExchange(&slot.state, sa_to_v_offscreen::kFree);
    InterlockedIncrement(&controlView_->consumed);

    // Older ready frames are never useful in a latest-frame portal stream.
    for (int index = 0; index < sa_to_v_offscreen::kSlotCount; ++index) {
        if (index != newest && controlView_->slots[index].frameId < lastFrameId_)
            InterlockedCompareExchange(&controlView_->slots[index].state,
                sa_to_v_offscreen::kFree, sa_to_v_offscreen::kReady);
    }
    std::wostringstream text;
    text << L"SA 离屏第二相机 " << width << L'×' << height
         << L"，帧 " << lastFrameId_;
    status_ = text.str();
    return true;
}

bool SAOffscreenConsumer::Live() const {
    const uint64_t now = GetTickCount64();
    return texture_ && lastFrameTickMs_ && now >= lastFrameTickMs_ &&
           now - lastFrameTickMs_ <= 500;
}
