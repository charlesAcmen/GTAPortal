#pragma once

#include <windows.h>
#include <cstdint>

// The same layout is mapped by the x86 ASI and the x64 supervisor.
namespace portal {

constexpr wchar_t kMappingName[] = L"Local\\GTAPortal.Frame.v1";
constexpr uint32_t kMagic = 0x31505447; // GTP1
constexpr uint32_t kVersion = 1;
constexpr int kSlotCount = 3;

enum SlotState : LONG {
    kFree = 0,
    kWriting = 1,
    kReady = 2,
    kReading = 3,
};

#pragma pack(push, 8)
struct FrameSlot {
    volatile LONG state;
    uint32_t reserved;
    uint64_t sharedHandle;
    uint64_t frameId;
    int64_t captureQpc;
    int64_t readyQpc;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t cameraValid;
    uint32_t submitUs;
    uint32_t stateAgeMs;
    float cameraPosition[3];
    float cameraForward[3];
    float cameraUp[3];
    float fovDegrees;
    float nearClip;
    float farClip;
    float viewWindowX;
    float viewWindowY;
    float view[16];
    float projection[16];
};

struct SharedFrames {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    volatile LONG producerPid;
    volatile LONG generation;
    uint32_t adapterLuidLow;
    int32_t adapterLuidHigh;
    uint32_t reserved;
    volatile LONG produced;
    volatile LONG dropped;
    volatile LONG errors;
    volatile LONG bridgeEnabled;
    FrameSlot slots[kSlotCount];
};
#pragma pack(pop)

static_assert(sizeof(LONG) == 4);
static_assert(sizeof(FrameSlot) == 248);
static_assert(sizeof(SharedFrames) == 792);

} // namespace portal
