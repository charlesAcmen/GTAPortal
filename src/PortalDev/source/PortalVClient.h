#pragma once

#include <windows.h>
#include <cstdint>

class PortalVClient {
public:
    struct Snapshot {
        DWORD processId = 0;
        uint64_t ageMs = 0;
        uint64_t frameId = 0;
        float cameraPosition[3]{};
        float cameraRotation[3]{};
        float fovDegrees = 0;
        float renderedCameraPosition[3]{};
        float renderedCameraRotation[3]{};
        float renderedFovDegrees = 0;
        bool cameraValid = false;
        bool overlayEnabled = false;
        LONG teleportResult = 0;
        LONG teleportAppliedRequest = 0;
    };

    ~PortalVClient();
    bool Read(Snapshot& snapshot);
    bool SetOverlayEnabled(bool enabled);
    bool RequestTeleport(const float (&position)[3], float headingDegrees);
    void Disconnect();

private:
    bool Connect();
    HANDLE mapping_ = nullptr;
    struct SharedStateView;
    SharedStateView* view_ = nullptr;
};
