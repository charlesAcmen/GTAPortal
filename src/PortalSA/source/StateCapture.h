#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>

namespace portal_sa::state {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct State {
    DWORD tick = 0;
    bool playerValid = false;
    bool cameraValid = false;
    bool velocityFromVehicle = false;
    Vec3 playerPosition;
    Vec3 playerVelocity;
    float playerHeading = 0.0f;
    Vec3 cameraPosition;
    Vec3 cameraForward;
    Vec3 cameraUp;
    float fovDegrees = 0.0f;
    float nearClip = 0.0f;
    float farClip = 0.0f;
    float viewWindowX = 0.0f;
    float viewWindowY = 0.0f;
    unsigned cameraIndex = 0;
};

extern State g_state;
extern std::array<float, 16> g_viewMatrix;
extern std::array<float, 16> g_projectionMatrix;
extern uint64_t g_renderMatrixSerial;

void OnGameProcess(void (*log)(const std::string&));
void OnRenderScene();

} // namespace portal_sa::state
