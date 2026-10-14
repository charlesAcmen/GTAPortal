#pragma once

#include <d3d9.h>

namespace portal_sa::second_camera {

void OnRenderMirror(IDirect3DDevice9* device, void (*log)(const char*));
void BeforeReset();
void Shutdown();

} // namespace portal_sa::second_camera
