#pragma once

#include <windows.h>

namespace portal_v::compositor {

struct Pose {
    float camera[3]{};
    float rotation[3]{};
    float fov = 60.0f;
    float nearClip = 0.15f;
    float farClip = 10000.0f;
    float planeCenter[3]{};
    float planeYaw = 0.0f;
    float planeSize[2]{2.4f, 1.35f};
    float planeCrop[4]{0.0f, 0.0f, 1.0f, 1.0f};
    float cornerRadius = 0.16f;
    float edgeFeather = 0.008f;
    int planeShape = 1;
    bool planeValid = false;
};

bool TryRegister(HMODULE module);
void SetPose(const Pose& pose);
void OpenCalibrationOverlay();
void ReloadEffects();
void Unregister(HMODULE module);

} // namespace portal_v::compositor
