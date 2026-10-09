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
    bool planeValid = false;
};

bool TryRegister(HMODULE module);
void SetPose(const Pose& pose);
void Unregister(HMODULE module);

} // namespace portal_v::compositor
