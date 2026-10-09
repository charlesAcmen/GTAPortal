#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winver.h>
#include <shobjidl.h>

#include "GamePaths.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace game_paths {
namespace {
constexpr wchar_t kSettingsKey[] = L"Software\\GTAPortal\\PortalDev";
constexpr wchar_t kSaMd5[] = L"170B3A9108687B26DA2D8901C6948A18";
}

fs::path FindRepoRoot() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return {};
    fs::path cursor = fs::path(path.data()).parent_path();
    for (int depth = 0; depth < 10; ++depth) {
        if (fs::exists(cursor / L"src" / L"PortalSA" / L"PortalSA.vcxproj")) return cursor;
        if (!cursor.has_parent_path() || cursor.parent_path() == cursor) break;
        cursor = cursor.parent_path();
    }
    return {};
}

std::optional<fs::path> ChooseDirectory(HWND owner, const fs::path& initial, const wchar_t* title) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    std::error_code directoryError;
    if (!initial.empty() && fs::is_directory(initial, directoryError)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                 IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    std::optional<fs::path> selected;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                selected = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return selected;
}

fs::path LoadSavedDirectory(const wchar_t* valueName) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, valueName, RRF_RT_REG_SZ,
                     nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, valueName, RRF_RT_REG_SZ,
                     nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    value.resize(wcsnlen_s(value.c_str(), value.size()));
    return fs::path(value);
}

bool SaveDirectory(const wchar_t* valueName, const fs::path& directory) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const std::wstring value = directory.wstring();
    const LONG result = RegSetValueExW(key, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::wstring HashMd5(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD hashBytes = 0;
    DWORD resultBytes = 0;
    std::wstring result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                          &resultBytes, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes),
                          &resultBytes, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<unsigned char> object(objectBytes);
    std::vector<unsigned char> digest(hashBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    std::array<char, 1 << 16> buffer{};
    bool valid = static_cast<bool>(input);
    while (valid && input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(count), 0) < 0) valid = false;
    }
    if (valid && input.eof() && BCryptFinishHash(hash, digest.data(), hashBytes, 0) >= 0) {
        std::wostringstream text;
        text << std::uppercase << std::hex << std::setfill(L'0');
        for (const unsigned char byte : digest) text << std::setw(2) << static_cast<unsigned>(byte);
        result = text.str();
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

bool ValidateSaDirectory(const fs::path& directory, std::wstring& md5) {
    const fs::path executable = directory / L"gta-sa.EXE";
    std::error_code error;
    md5 = fs::is_regular_file(executable, error) ? HashMd5(executable) : L"";
    return md5 == kSaMd5;
}

bool ValidateGtaVDirectory(const fs::path& directory, std::wstring& version) {
    version.clear();
    const fs::path executable = directory / L"GTA5_Enhanced.exe";
    std::error_code error;
    if (!fs::is_regular_file(executable, error)) return false;
    DWORD binaryType = 0;
    if (!GetBinaryTypeW(executable.c_str(), &binaryType) || binaryType != SCS_64BIT_BINARY) return false;
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
    if (!bytes) return false;
    std::vector<BYTE> info(bytes);
    if (!GetFileVersionInfoW(executable.c_str(), 0, bytes, info.data())) return false;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) ||
        length < sizeof(VS_FIXEDFILEINFO) || fixed->dwSignature != 0xFEEF04BD) return false;
    struct Language { WORD language; WORD codePage; }* language = nullptr;
    if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation",
                         reinterpret_cast<void**>(&language), &length) || length < sizeof(Language)) return false;
    const auto readString = [&](const wchar_t* name) -> std::wstring {
        wchar_t key[128]{};
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", language->language,
                   language->codePage, name);
        wchar_t* value = nullptr;
        UINT valueLength = 0;
        if (!VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&value), &valueLength) ||
            !value || !valueLength) return {};
        return value;
    };
    if (readString(L"CompanyName") != L"Rockstar Games" ||
        readString(L"ProductName") != L"Grand Theft Auto V" ||
        readString(L"OriginalFilename") != L"GTA5_Enhanced.exe") return false;
    std::wostringstream text;
    text << HIWORD(fixed->dwFileVersionMS) << L'.' << LOWORD(fixed->dwFileVersionMS)
         << L'.' << HIWORD(fixed->dwFileVersionLS) << L'.' << LOWORD(fixed->dwFileVersionLS);
    version = text.str();
    return true;
}

bool ValidateGtaVHookVersion(const fs::path& directory,
                             const std::wstring& gameVersion, std::wstring& hookVersion) {
    hookVersion.clear();
    const fs::path dll = directory / L"ScriptHookV.dll";
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(dll.c_str(), &ignored);
    if (!bytes) return false;
    std::vector<BYTE> info(bytes);
    if (!GetFileVersionInfoW(dll.c_str(), 0, bytes, info.data())) return false;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) ||
        length < sizeof(VS_FIXEDFILEINFO) || fixed->dwSignature != 0xFEEF04BD) return false;
    std::wostringstream text;
    text << HIWORD(fixed->dwFileVersionMS) << L'.' << LOWORD(fixed->dwFileVersionMS)
         << L'.' << HIWORD(fixed->dwFileVersionLS) << L'.' << LOWORD(fixed->dwFileVersionLS);
    hookVersion = text.str();
    const auto gameMinor = gameVersion.find(L'.', gameVersion.find(L'.') + 1);
    const auto hookMinor = hookVersion.find(L'.', hookVersion.find(L'.') + 1);
    return gameMinor != std::wstring::npos && hookMinor != std::wstring::npos &&
           gameVersion.substr(gameMinor) == hookVersion.substr(hookMinor);
}

} // namespace game_paths
