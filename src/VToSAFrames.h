#pragma once

#include <windows.h>
#include <cstdint>

// The x86 game owns the textures; the x64 tool opens their legacy D3D9 handles.
namespace v_to_sa {

constexpr wchar_t kMappingName[] = L"Local\\GTAPortal.VToSA.v1";
constexpr uint32_t kMagic = 0x31535647; // GVS1
constexpr uint32_t kVersion = 1;
constexpr int kSlotCount = 3;
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kFormat = 32; // D3DFMT_A8B8G8R8 / DXGI_FORMAT_R8G8B8A8_UNORM

enum SlotState : LONG { kFree, kWriting, kReady, kReading };
enum Stage : LONG {
    kStarting, kTexturesReady, kOpenedByTool, kStreaming,
    kTextureError, kOpenError, kAdapterError, kShaderError, kDrawError,
};

#pragma pack(push, 8)
struct Slot {
    volatile LONG state;
    uint32_t reserved;
    uint64_t sharedHandle;
    uint64_t frameId;
    uint64_t readyTickMs;
};

struct SharedFrames {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t ownerPid;
    volatile LONG generation;
    volatile LONG stage;
    volatile LONG lastError;
    volatile LONG produced;
    volatile LONG displayed;
    volatile LONG dropped;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t reserved;
    uint32_t adapterLuidLow;
    int32_t adapterLuidHigh;
    volatile LONG readerPid;
    volatile LONG readerGeneration;
    Slot slots[kSlotCount];
};
#pragma pack(pop)

static_assert(sizeof(Slot) == 32);
static_assert(sizeof(SharedFrames) == 168);

} // namespace v_to_sa
