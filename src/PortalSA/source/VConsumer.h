#pragma once

struct IDirect3DDevice9;

namespace portal_sa::v_consumer {
struct VisualTuning {
    float radius;
    float centerAboveGround;
    bool followingPlayer;
};
void OnGameProcess(void (*log)(const char*));
void OnRenderScene(IDirect3DDevice9* device, void (*log)(const char*));
void BeforeReset();
VisualTuning GetVisualTuning();
void SetVisualTuning(float radius, float centerAboveGround);
void SaveVisualTuning();
bool ConsumeOverlayOpenRequest();
}
