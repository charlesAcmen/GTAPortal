#pragma once

#include <windows.h>

namespace portal { struct SharedFrames; }

namespace portal_dev::legacy::sa_gpu_preview {

// Preserved for mode-2 diagnosis of the archived SA-internal producer.
void Pump(portal::SharedFrames* frames, HWND previewWindow);
void Stop(portal::SharedFrames* frames);

} // namespace portal_dev::legacy::sa_gpu_preview
