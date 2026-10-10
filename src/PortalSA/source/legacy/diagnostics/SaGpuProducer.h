#pragma once

struct IDirect3DDevice9;

namespace portal_sa::legacy::sa_gpu_producer {

using LogFunction = void (*)(const char*);

void Configure(bool enabled, LogFunction log);
void BeforeReset(IDirect3DDevice9* device);
void BeforeShowRaster(IDirect3DDevice9* device);
void AfterShowRaster();

} // namespace portal_sa::legacy::sa_gpu_producer
