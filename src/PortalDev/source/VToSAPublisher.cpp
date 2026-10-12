#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "VToSAPublisher.h"
#include <cstring>
#include <limits>
#include <sstream>

namespace {
uint64_t NowFileTime() {
    FILETIME value{}; GetSystemTimeAsFileTime(&value);
    ULARGE_INTEGER joined{}; joined.LowPart = value.dwLowDateTime; joined.HighPart = value.dwHighDateTime;
    return joined.QuadPart;
}
}

void VToSAPublisher::Start(ID3D11Device* device, ID3D11DeviceContext* context) {
    Stop(); device_.copy_from(device); context_.copy_from(context); CreateControlMapping();
}

bool VToSAPublisher::CreateControlMapping() {
    controlMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(v_to_sa::SharedFrames), v_to_sa::kMappingName);
    if (!controlMapping_) return false;
    shared_ = static_cast<v_to_sa::SharedFrames*>(MapViewOfFile(controlMapping_,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(v_to_sa::SharedFrames)));
    if (!shared_) { CloseHandle(controlMapping_); controlMapping_ = nullptr; return false; }
    ZeroMemory(shared_, sizeof(*shared_));
    shared_->version = v_to_sa::kVersion; shared_->size = sizeof(*shared_);
    shared_->ownerPid = GetCurrentProcessId(); shared_->format = v_to_sa::kFormatBgra8;
    generationSeed_ = static_cast<LONG>(GetTickCount() | 1);
    shared_->stage = v_to_sa::kStarting; MemoryBarrier(); shared_->magic = v_to_sa::kMagic;
    return true;
}

void VToSAPublisher::ReleaseReadback() {
    if (shared_) InterlockedExchange(&shared_->generation, 0);
    if (context_) {
        context_->Flush();
        const DWORD start = GetTickCount();
        for (auto& slot : readbacks_) while (slot.pending && slot.done &&
            GetTickCount() - start < 200 && context_->GetData(slot.done.get(), nullptr, 0,
            D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE) Sleep(1);
    }
    for (auto& slot : readbacks_) slot = {};
    if (pixels_) UnmapViewOfFile(pixels_);
    if (pixelMapping_) CloseHandle(pixelMapping_);
    pixels_ = nullptr; pixelMapping_ = nullptr; width_ = height_ = 0; nextReadback_ = 0;
}

void VToSAPublisher::Stop() {
    ReleaseReadback();
    if (shared_) { InterlockedExchange(&shared_->stage, v_to_sa::kStarting); UnmapViewOfFile(shared_); }
    if (controlMapping_) CloseHandle(controlMapping_);
    shared_ = nullptr; controlMapping_ = nullptr; context_ = nullptr; device_ = nullptr;
}

void VToSAPublisher::Error(v_to_sa::Stage stage, HRESULT hr) {
    if (shared_) { InterlockedExchange(&shared_->lastError, hr); InterlockedExchange(&shared_->stage, stage); }
}

bool VToSAPublisher::PrepareReadback(UINT width, UINT height) {
    if (pixels_ && width_ == width && height_ == height) return true;
    if (!shared_ || !device_ || !width || !height || width > v_to_sa::kMaxDimension ||
        height > v_to_sa::kMaxDimension) { Error(v_to_sa::kReadbackError, E_INVALIDARG); return false; }
    ReleaseReadback();
    const uint64_t rowBytes = static_cast<uint64_t>(width) * v_to_sa::kBytesPerPixel;
    const uint64_t slotBytes = rowBytes * height;
    const uint64_t totalBytes = slotBytes * v_to_sa::kSlotCount;
    if (!slotBytes || totalBytes > std::numeric_limits<SIZE_T>::max()) {
        Error(v_to_sa::kMappingError, E_OUTOFMEMORY); return false;
    }
    const LONG generation = ++generationSeed_;
    wchar_t name[v_to_sa::kPixelMappingNameChars]{};
    swprintf_s(name, L"%s.%lu.%ld", v_to_sa::kPixelMappingPrefix, GetCurrentProcessId(), generation);
    pixelMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(totalBytes >> 32), static_cast<DWORD>(totalBytes), name);
    if (pixelMapping_) pixels_ = MapViewOfFile(pixelMapping_, FILE_MAP_READ | FILE_MAP_WRITE,
        0, 0, static_cast<SIZE_T>(totalBytes));
    if (!pixels_) { Error(v_to_sa::kMappingError, HRESULT_FROM_WIN32(GetLastError())); return false; }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    for (auto& slot : readbacks_) {
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, slot.staging.put());
        if (SUCCEEDED(hr)) hr = device_->CreateQuery(&query, slot.done.put());
        if (FAILED(hr)) { Error(v_to_sa::kReadbackError, hr); ReleaseReadback(); return false; }
    }
    width_ = width; height_ = height;
    ZeroMemory(pixels_, static_cast<SIZE_T>(totalBytes));
    for (auto& slot : shared_->slots) { slot = {}; slot.state = v_to_sa::kFree; }
    shared_->width = width; shared_->height = height; shared_->rowBytes = static_cast<uint32_t>(rowBytes);
    shared_->slotBytes = slotBytes; shared_->format = v_to_sa::kFormatBgra8;
    wcscpy_s(shared_->pixelMappingName, name);
    InterlockedExchange(&shared_->lastError, S_OK);
    InterlockedExchange(&shared_->stage, v_to_sa::kReadbackReady);
    MemoryBarrier(); InterlockedExchange(&shared_->generation, generation);
    return true;
}

int VToSAPublisher::ClaimMailboxSlot() {
    int oldest = -1; uint64_t tick = std::numeric_limits<uint64_t>::max();
    for (int i = 0; i < v_to_sa::kSlotCount; ++i) {
        auto& slot = shared_->slots[i]; const LONG state = InterlockedCompareExchange(&slot.state, 0, 0);
        if (state == v_to_sa::kFree && InterlockedCompareExchange(&slot.state,
            v_to_sa::kWriting, v_to_sa::kFree) == v_to_sa::kFree) return i;
        if (state == v_to_sa::kReady && slot.readyTickMs < tick) { oldest = i; tick = slot.readyTickMs; }
    }
    if (oldest >= 0 && InterlockedCompareExchange(&shared_->slots[oldest].state,
        v_to_sa::kWriting, v_to_sa::kReady) == v_to_sa::kReady) {
        InterlockedIncrement(&shared_->overwritten); return oldest;
    }
    return -1;
}

void VToSAPublisher::CompleteReadbacks() {
    if (!shared_ || !pixels_ || !context_) return;
    LONG busy = 0;
    for (auto& readback : readbacks_) {
        if (!readback.pending) continue;
        ++busy;
        const HRESULT ready = context_->GetData(readback.done.get(), nullptr, 0,
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (ready == S_FALSE) continue;
        readback.pending = false; --busy;
        if (FAILED(ready)) { Error(v_to_sa::kReadbackError, ready); continue; }
        const int mailbox = ClaimMailboxSlot();
        if (mailbox < 0) { InterlockedIncrement(&shared_->dropped); continue; }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context_->Map(readback.staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) { InterlockedExchange(&shared_->slots[mailbox].state, v_to_sa::kFree);
            Error(v_to_sa::kReadbackError, hr); continue; }
        auto* destination = v_to_sa::SlotPixels(pixels_, shared_->slotBytes, mailbox);
        for (UINT row = 0; row < height_; ++row) std::memcpy(
            destination + static_cast<size_t>(row) * shared_->rowBytes,
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
            shared_->rowBytes);
        context_->Unmap(readback.staging.get(), 0);
        auto& metadata = shared_->slots[mailbox];
        metadata.frameId = readback.frameId; metadata.captureTime100ns = readback.captureTime100ns;
        metadata.readyTickMs = GetTickCount64(); MemoryBarrier();
        InterlockedExchange(&metadata.state, v_to_sa::kReady);
        InterlockedIncrement(&shared_->produced);
        InterlockedExchange(&shared_->stage, shared_->consumerPid ? v_to_sa::kStreaming : v_to_sa::kWaitingForConsumer);
    }
    InterlockedExchange(&shared_->readbackBusy, busy);
}

void VToSAPublisher::Poll() { CompleteReadbacks(); }

void VToSAPublisher::Publish(ID3D11Texture2D* source) {
    CompleteReadbacks();
    if (!shared_ || !source || !context_) return;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1 ||
        !PrepareReadback(desc.Width, desc.Height)) return;
    for (unsigned offset = 0; offset < readbacks_.size(); ++offset) {
        const unsigned index = (nextReadback_ + offset) % readbacks_.size(); auto& slot = readbacks_[index];
        if (slot.pending) continue;
        context_->CopyResource(slot.staging.get(), source); context_->End(slot.done.get());
        slot.frameId = ++frameId_; slot.captureTime100ns = NowFileTime(); slot.pending = true;
        // Submit the DMA batch without waiting. DONOTFLUSH polling alone is not
        // guaranteed to push a partially filled immediate-context command buffer.
        context_->Flush();
        nextReadback_ = (index + 1) % readbacks_.size(); return;
    }
    InterlockedIncrement(&shared_->dropped);
}

std::wstring VToSAPublisher::Status() const {
    if (!shared_) return L"V→SA 控制区创建失败";
    std::wostringstream text; text << L"V→SA：";
    switch (shared_->stage) {
    case v_to_sa::kStarting: text << L"等待 V 画面"; break;
    case v_to_sa::kReadbackReady: text << L"异步读回已就绪"; break;
    case v_to_sa::kWaitingForConsumer: text << L"等待 SA 消费端"; break;
    case v_to_sa::kStreaming: text << L"最新帧传输中"; break;
    case v_to_sa::kReadbackError: text << L"GPU 异步读回失败"; break;
    case v_to_sa::kMappingError: text << L"共享内存创建失败"; break;
    case v_to_sa::kUploadError: text << L"SA 上传失败"; break;
    case v_to_sa::kDrawError: text << L"SA 绘制失败"; break;
    default: text << L"未知阶段"; break;
    }
    if (shared_->width) text << L"  " << shared_->width << L"×" << shared_->height;
    text << L"  发布 " << shared_->produced << L" / 显示 " << shared_->displayed
        << L" / 覆盖 " << shared_->overwritten << L" / 跳过 " << shared_->dropped
        << L" / DMA中 " << shared_->readbackBusy;
    if (shared_->lastError) text << L"  HRESULT 0x" << std::hex << static_cast<unsigned>(shared_->lastError);
    return text.str();
}
