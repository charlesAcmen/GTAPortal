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
        float portalPlaneCenter[3]{};
        float portalPlaneYawDegrees = 0;
        bool portalPlaneValid = false;
        bool cameraValid = false;
        bool overlayEnabled = false;
        bool portalVisible = false;
        LONG teleportResult = 0;
        LONG teleportAppliedRequest = 0;
        LONG reloadFxApplied = 0;
        bool cameraOverrideActive=false;
    };

    ~PortalVClient();
    bool Read(Snapshot& snapshot);
    bool SetOverlayEnabled(bool enabled);
    bool SetPortalVisible(bool visible);
    bool RequestTeleport(const float (&position)[3], float headingDegrees);
    bool RequestFxReload();
    bool PublishCamera(bool enabled,const float (&position)[3],
                       const float (&rotation)[3],float fovDegrees);
    void Disconnect();

private:
    bool Connect();
    HANDLE mapping_ = nullptr;
    struct SharedStateView;
    SharedStateView* view_ = nullptr;
};
