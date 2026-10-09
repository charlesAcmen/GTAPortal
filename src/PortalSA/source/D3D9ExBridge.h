#pragma once

namespace portal_sa {
bool InstallD3D9ExBridge(void (*trace)(const char*));
bool CreatedExDevice();
}
