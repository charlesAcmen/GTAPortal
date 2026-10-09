#pragma once

#include <windows.h>
#include <filesystem>

namespace game_process {

bool FindByExecutablePath(const std::filesystem::path& executable, DWORD* processId = nullptr);
HWND FindVisibleWindow(DWORD processId);

} // namespace game_process
