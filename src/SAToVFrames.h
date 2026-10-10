#pragma once

#include <windows.h>
#include <cstdint>

namespace sa_to_v_frames {

constexpr wchar_t kMappingName[] = L"Local\\GTAPortal.SAFrames.v1";
constexpr uint32_t kMagic = 0x31465347; // GSF1
constexpr uint32_t kVersion = 1;
constexpr int kSlotCount = 3;
constexpr int kNameLength = 96;

enum SlotState : LONG {
    kFree = 0,
    kWriting = 1,
    kReady = 2,
    kReading = 3,
};

struct FrameSlot {
    volatile LONG state;
    uint32_t reserved;
    uint64_t frameId;
    int64_t compositorTime100ns;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t reserved2;
    wchar_t resourceName[kNameLength];
};

struct SharedFrames {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t producerPid;
    volatile LONG generation;
    uint32_t adapterLuidLow;
    int32_t adapterLuidHigh;
    volatile LONG produced;
    volatile LONG overwritten;
    volatile LONG dropped;
    volatile LONG errors;
    uint32_t width;
    uint32_t height;
    FrameSlot slots[kSlotCount];
};

static_assert(sizeof(FrameSlot) == 232);
static_assert(sizeof(SharedFrames) == 752);

} // namespace sa_to_v_frames
