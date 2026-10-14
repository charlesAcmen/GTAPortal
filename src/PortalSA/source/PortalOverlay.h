#pragma once

struct IDirect3DDevice9;

namespace portal_sa::portal_overlay {
void Render(IDirect3DDevice9* device);
void BeforeReset();
void AfterReset();
void Shutdown();
}
