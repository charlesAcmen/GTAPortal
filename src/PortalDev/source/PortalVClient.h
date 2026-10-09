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
        bool cameraValid = false;
        bool overlayEnabled = false;
    };

    ~PortalVClient();
    bool Read(Snapshot& snapshot);
    bool SetOverlayEnabled(bool enabled);
    void Disconnect();

private:
    bool Connect();
    HANDLE mapping_ = nullptr;
    struct SharedStateView;
    SharedStateView* view_ = nullptr;
};
