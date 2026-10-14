#pragma once

#include <windows.h>

#include <cstdint>

// PortalSA publishes the independently rendered portal camera into this
// latest-frame CPU mailbox. PortalDev uploads the newest BGRA frame to D3D11
// and forwards it through the existing SAFrames GPU channel consumed by V.
namespace sa_to_v_offscreen {

constexpr wchar_t kMappingName[] = L"Local\\GTAPortal.SAOffscreen.Control.v1";
constexpr wchar_t kPixelMappingPrefix[] = L"Local\\GTAPortal.SAOffscreen.Pixels.v1";
constexpr uint32_t kMagic = 0x314F5347; // GSO1
constexpr uint32_t kVersion = 1;
constexpr int kSlotCount = 3;
constexpr uint32_t kBytesPerPixel = 4;
constexpr uint32_t kFormatBgra8 = 87;
constexpr uint32_t kMaxDimension = 4096;
constexpr size_t kPixelMappingNameChars = 96;

enum SlotState : LONG { kFree, kWriting, kReady, kReading };
enum Stage : LONG {
    kStarting,
    kWaitingForCamera,
    kRendering,
    kStreaming,
    kRenderError,
    kReadbackError,
    kMappingError,
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
    volatile LONG produced, consumed, dropped, overwritten, consumerPid;
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

} // namespace sa_to_v_offscreen
