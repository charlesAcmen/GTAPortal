#pragma once
#include <windows.h>
#include <cstdint>

// PortalDev publishes completed D3D11 readbacks into this latest-frame CPU
// mailbox. SA uploads the newest BGRA slot to its native D3D9 device.
namespace v_to_sa {
constexpr wchar_t kMappingName[] = L"Local\\GTAPortal.VToSA.Control.v2";
constexpr wchar_t kPixelMappingPrefix[] = L"Local\\GTAPortal.VToSA.Pixels.v2";
constexpr uint32_t kMagic = 0x32535647; // GVS2
constexpr uint32_t kVersion = 2;
constexpr int kSlotCount = 3;
constexpr uint32_t kBytesPerPixel = 4;
constexpr uint32_t kFormatBgra8 = 87;
constexpr uint32_t kMaxDimension = 8192;
constexpr size_t kPixelMappingNameChars = 96;

enum SlotState : LONG { kFree, kWriting, kReady, kReading };
enum Stage : LONG {
    kStarting, kReadbackReady, kWaitingForConsumer, kStreaming,
    kReadbackError, kMappingError, kUploadError, kDrawError,
};
#pragma pack(push, 8)
struct Slot {
    volatile LONG state;
    uint32_t reserved;
    uint64_t frameId;
    uint64_t readyTickMs;
    uint64_t captureTime100ns;
};
struct SharedFrames {
    uint32_t magic, version, size, ownerPid;
    volatile LONG generation, stage, lastError;
    volatile LONG produced, displayed, dropped, overwritten, readbackBusy, consumerPid;
    uint32_t width, height, rowBytes, format;
    uint64_t slotBytes;
    wchar_t pixelMappingName[kPixelMappingNameChars];
    Slot slots[kSlotCount];
};
#pragma pack(pop)
static_assert(sizeof(Slot) == 32);
inline uint8_t* SlotPixels(void* base, uint64_t bytes, int index) {
    return static_cast<uint8_t*>(base) + bytes * static_cast<uint64_t>(index);
}
} // namespace v_to_sa
