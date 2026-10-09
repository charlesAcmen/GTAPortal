#define WIN32_LEAN_AND_MEAN
#include "GameProcess.h"

#include <tlhelp32.h>

#include <array>

namespace game_process {

bool FindByExecutablePath(const std::filesystem::path& executable, DWORD* processId) {
    if (executable.empty()) return false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, executable.filename().c_str()) != 0) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            std::array<wchar_t, 32768> path{};
            DWORD length = static_cast<DWORD>(path.size());
            found = QueryFullProcessImageNameW(process, 0, path.data(), &length) &&
                    _wcsicmp(path.data(), executable.c_str()) == 0;
            CloseHandle(process);
            if (found) {
                if (processId) *processId = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

HWND FindVisibleWindow(DWORD processId) {
    struct Search { DWORD pid; HWND window; } search{processId, nullptr};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(parameter);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == search.pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
            search.window = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

} // namespace game_process
