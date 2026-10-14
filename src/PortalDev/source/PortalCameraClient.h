#pragma once

#include <windows.h>
#include <cstdint>

class PortalCameraClient {
public:
    struct Snapshot {
        DWORD processId{};
        uint64_t ageMs{};
        float playerPosition[3]{};
        float cameraPosition[3]{};
        float cameraForward[3]{};
        float cameraUp[3]{};
        float cameraFovDegrees{};
        bool playerValid{};
        bool cameraValid{};
        bool overrideActive{};
        uint32_t portalPlacementSequence{};
        float portalPlacement[4]{};
    };

    ~PortalCameraClient();
    bool Read(Snapshot& snapshot);
    bool Publish(bool enabled, bool showSaPortal, const float (&position)[3],
                 const float (&forward)[3], const float (&up)[3], float fovDegrees);
    void Disconnect();

private:
    bool Connect();
    HANDLE mapping_{};
    struct SharedStateView;
    SharedStateView* view_{};
};
