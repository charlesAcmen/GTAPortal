#pragma once

#include <windows.h>
#include <cstdint>

namespace portal_v {

constexpr wchar_t kMappingName[] = L"Local\\GTAPortal.VState.v1";
constexpr uint32_t kMagic = 0x31565447; // GTV1
constexpr uint32_t kVersion = 1;

// The ASI owns this mapping. The tool reads a latest-only snapshot and writes
// one atomic overlay command; no frame images or pointers cross this boundary.
struct SharedState {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t processId;
    volatile LONG sequence;
    volatile LONG overlayEnabled;
    uint64_t heartbeatTickMs;
    uint64_t frameId;
    float cameraPosition[3];
    float cameraRotation[3];
    float fovDegrees;
    uint32_t cameraValid;
};

static_assert(sizeof(SharedState) == 72);

} // namespace portal_v
