#pragma once

#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>

namespace game_paths {

std::filesystem::path FindRepoRoot();
std::optional<std::filesystem::path> ChooseDirectory(HWND owner, const std::filesystem::path& initial,
                                                      const wchar_t* title);
std::filesystem::path LoadSavedDirectory(const wchar_t* valueName);
bool SaveDirectory(const wchar_t* valueName, const std::filesystem::path& directory);
std::wstring HashMd5(const std::filesystem::path& file);
bool ValidateSaDirectory(const std::filesystem::path& directory, std::wstring& md5);
bool ValidateGtaVDirectory(const std::filesystem::path& directory, std::wstring& version);
bool ValidateGtaVHookVersion(const std::filesystem::path& directory,
                             const std::wstring& gameVersion, std::wstring& hookVersion);

} // namespace game_paths
