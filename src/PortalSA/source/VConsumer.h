#pragma once

struct IDirect3DDevice9;

namespace portal_sa::v_consumer {
void OnGameProcess(void (*log)(const char*));
void OnRenderScene(IDirect3DDevice9* device, void (*log)(const char*));
void BeforeReset();
}
