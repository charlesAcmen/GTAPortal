#pragma once

#include <windows.h>

namespace portal_sa {
bool InstallD3D9ExBridge(void (*trace)(const char*));
bool CreatedExDevice();
LONG BridgeMode();
}
