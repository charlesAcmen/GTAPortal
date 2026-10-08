#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"GTAPortalDevWindow";
constexpr wchar_t kExpectedMd5[] = L"170B3A9108687B26DA2D8901C6948A18";
constexpr UINT_PTR kRefreshTimer = 1;

enum ControlId : int {
    kBuild = 1001,
    kDeploy = 1002,
    kLaunch = 1003,
    kDeployLaunch = 1004,
    kRefresh = 1005,
    kOpenLog = 1006,
    kOpenFrame = 1007,
    kOpenDev = 1008,
    kDisable = 1009,
};

HINSTANCE g_instance{};
HWND g_window{};
HWND g_status{};
HWND g_logView{};
HFONT g_uiFont{};
HFONT g_monoFont{};
fs::path g_repo;
fs::path g_dev;
fs::path g_sourceAsi;
fs::path g_deployedAsi;
fs::path g_disabledAsi;
fs::path g_log;
fs::path g_frame;
fs::path g_game;
std::wstring g_gameMd5;

std::wstring WinError(DWORD code = GetLastError()) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = buffer ? buffer : L"未知错误";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

void ShowError(const std::wstring& title, const std::wstring& message) {
    MessageBoxW(g_window, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

fs::path FindRepoRoot() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) {
        return {};
    }
    fs::path cursor = fs::path(path.data()).parent_path();
    for (int depth = 0; depth < 10; ++depth) {
        if (fs::exists(cursor / L"src" / L"PortalSA" / L"PortalSA.vcxproj")) {
            return cursor;
        }
        if (!cursor.has_parent_path() || cursor.parent_path() == cursor) {
            break;
        }
        cursor = cursor.parent_path();
    }
    return {};
}

std::wstring HashMd5(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD hashBytes = 0;
    DWORD resultBytes = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
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

    object.resize(objectBytes);
    digest.resize(hashBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    std::array<char, 1 << 16> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                           static_cast<ULONG>(count), 0) < 0) {
            result.clear();
            break;
        }
    }

    if (input.eof() && BCryptFinishHash(hash, digest.data(), hashBytes, 0) >= 0) {
        std::wostringstream text;
        text << std::uppercase << std::hex << std::setfill(L'0');
        for (const unsigned char byte : digest) {
            text << std::setw(2) << static_cast<unsigned>(byte);
        }
        result = text.str();
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

bool IsGameRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"gta-sa.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::string ReadTailBytes(const fs::path& path, size_t maximum = 96 * 1024) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const auto end = input.tellg();
    const auto size = static_cast<size_t>(std::max<std::streamoff>(0, end));
    const auto start = size > maximum ? size - maximum : 0;
    input.seekg(static_cast<std::streamoff>(start));
    std::string bytes(size - start, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (start > 0) {
        const auto newline = bytes.find('\n');
        if (newline != std::string::npos) bytes.erase(0, newline + 1);
    }
    return bytes;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (!length) return {};
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::wstring SizeText(const fs::path& path) {
    std::error_code error;
    const auto bytes = fs::file_size(path, error);
    if (error) return L"不可读";
    std::wostringstream text;
    text << bytes << L" bytes";
    return text.str();
}

bool SameFile(const fs::path& left, const fs::path& right) {
    std::error_code errorLeft;
    std::error_code errorRight;
    const auto leftSize = fs::file_size(left, errorLeft);
    const auto rightSize = fs::file_size(right, errorRight);
    return !errorLeft && !errorRight && leftSize == rightSize &&
           HashMd5(left) == HashMd5(right);
}

void Refresh() {
    const bool running = IsGameRunning();
    const bool sourceReady = fs::exists(g_sourceAsi);
    const bool deployed = fs::exists(g_deployedAsi);
    const bool matching = sourceReady && deployed && SameFile(g_sourceAsi, g_deployedAsi);
    const bool logReady = fs::exists(g_log);
    const bool frameReady = fs::exists(g_frame);
    const std::string logBytes = ReadTailBytes(g_log);

    std::wostringstream status;
    status << L"游戏版本：" << (g_gameMd5 == kExpectedMd5 ? L"✓ HOODLUM 1.0 US" : L"✗ 未识别")
           << L"    MD5: " << (g_gameMd5.empty() ? L"读取失败" : g_gameMd5) << L"\r\n"
           << L"ASI 构建：" << (sourceReady ? L"✓ " : L"✗ ")
           << (sourceReady ? SizeText(g_sourceAsi) : L"尚未生成")
           << L"    dev 部署：" << (matching ? L"✓ 已同步" : deployed ? L"△ 版本可能不同" : L"✗ 未部署")
           << L"    游戏：" << (running ? L"● 正在运行" : L"○ 未运行") << L"\r\n"
           << L"注入日志：" << (logBytes.find("PortalSA loaded") != std::string::npos ? L"✓ 已加载" : L"等待")
           << L"    渲染 Hook：" << (logBytes.find("Render output hook installed") != std::string::npos ? L"✓ 已安装" : L"等待")
           << L"    状态流：" << (logBytes.find("state tick_ms=") != std::string::npos ? L"✓ 有数据" : L"等待")
           << L"    抓帧：" << (logBytes.find("frame tick_ms=") != std::string::npos ? L"✓ 有记录" : L"等待")
           << L"\r\n截图文件：" << (frameReady ? L"✓ " + SizeText(g_frame) : L"尚未生成")
           << L"\r\n仓库：" << g_repo.wstring();
    SetWindowTextW(g_status, status.str().c_str());

    const std::wstring wideLog = logReady ? Widen(logBytes) : L"portal.log 尚未生成。部署并启动游戏后，此处会自动刷新。";
    const int firstVisible = static_cast<int>(SendMessageW(g_logView, EM_GETFIRSTVISIBLELINE, 0, 0));
    const int lineCount = static_cast<int>(SendMessageW(g_logView, EM_GETLINECOUNT, 0, 0));
    const bool followTail = firstVisible >= std::max(0, lineCount - 15);
    SetWindowTextW(g_logView, wideLog.c_str());
    if (followTail) {
        SendMessageW(g_logView, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(g_logView, EM_SCROLLCARET, 0, 0);
    }
}

bool Deploy() {
    if (IsGameRunning()) {
        ShowError(L"无法部署", L"请先关闭 GTA San Andreas，再替换 PortalSA.asi。");
        return false;
    }
    if (!fs::exists(g_sourceAsi)) {
        ShowError(L"尚未构建", L"找不到编译产物。请先点击“重新构建”。");
        return false;
    }
    if (!CopyFileW(g_sourceAsi.c_str(), g_deployedAsi.c_str(), FALSE)) {
        ShowError(L"部署失败", WinError());
        return false;
    }
    Refresh();
    return true;
}

void DisableAsi() {
    if (IsGameRunning()) {
        ShowError(L"无法停用", L"请先关闭 GTA San Andreas，再停用 PortalSA.asi。");
        return;
    }
    if (!fs::exists(g_deployedAsi)) {
        MessageBoxW(g_window, L"dev 目录中没有启用的 PortalSA.asi。", L"GTAPortal Dev",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!MoveFileExW(g_deployedAsi.c_str(), g_disabledAsi.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ShowError(L"停用失败", WinError());
        return;
    }
    Refresh();
}

bool LaunchGame() {
    if (IsGameRunning()) {
        MessageBoxW(g_window, L"GTA San Andreas 已经在运行。", L"GTAPortal Dev", MB_OK | MB_ICONINFORMATION);
        return true;
    }
    if (g_gameMd5 != kExpectedMd5) {
        ShowError(L"版本检查失败", L"dev 目录中的 gta-sa.EXE 不是已确认的 HOODLUM 1.0 US，已阻止启动。");
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(g_game.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr,
                        g_dev.c_str(), &startup, &process)) {
        ShowError(L"启动失败", WinError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Refresh();
    return true;
}

void OpenPath(const fs::path& path, const wchar_t* missingMessage) {
    if (!fs::exists(path)) {
        ShowError(L"文件不存在", missingMessage);
        return;
    }
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"open", path.c_str(), nullptr,
                                                 path.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
        ShowError(L"打开失败", WinError());
    }
}

void StartBuild() {
    const fs::path script = g_repo / L"src" / L"Build-Dev.ps1";
    const std::wstring parameters = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\"";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"open", L"powershell.exe",
                                                 parameters.c_str(), g_repo.c_str(),
                                                 SW_SHOWNORMAL)) <= 32) {
        ShowError(L"无法启动构建", WinError());
    }
}

void Layout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int padding = 12;
    const int buttonHeight = 30;
    const int gap = 8;
    int x = padding;
    const int y = padding;
    const std::array<std::pair<int, int>, 9> buttons{{
        {kBuild, 96}, {kDeploy, 86}, {kLaunch, 86}, {kDeployLaunch, 126},
        {kDisable, 86}, {kRefresh, 76}, {kOpenLog, 86}, {kOpenFrame, 86}, {kOpenDev, 96},
    }};
    for (const auto [id, width] : buttons) {
        HWND control = GetDlgItem(window, id);
        MoveWindow(control, x, y, width, buttonHeight, TRUE);
        x += width + gap;
    }
    const int statusY = y + buttonHeight + 10;
    const int statusHeight = 104;
    const int clientWidth = static_cast<int>(client.right - client.left);
    const int clientHeight = static_cast<int>(client.bottom - client.top);
    MoveWindow(g_status, padding, statusY, clientWidth - padding * 2, statusHeight, TRUE);
    const int logY = statusY + statusHeight + 6;
    MoveWindow(g_logView, padding, logY, clientWidth - padding * 2,
               std::max(80, clientHeight - logY - padding), TRUE);
}

HWND AddButton(HWND parent, int id, const wchar_t* text) {
    HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0, 0, 1, 1, parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  g_instance, nullptr);
    SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    return button;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        AddButton(window, kBuild, L"重新构建");
        AddButton(window, kDeploy, L"部署 ASI");
        AddButton(window, kLaunch, L"启动游戏");
        AddButton(window, kDeployLaunch, L"部署并启动");
        AddButton(window, kDisable, L"停用 ASI");
        AddButton(window, kRefresh, L"刷新");
        AddButton(window, kOpenLog, L"打开日志");
        AddButton(window, kOpenFrame, L"打开抓帧");
        AddButton(window, kOpenDev, L"打开 dev");
        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                   0, 0, 1, 1, window, nullptr, g_instance, nullptr);
        g_logView = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                    0, 0, 1, 1, window, nullptr, g_instance, nullptr);
        SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        SendMessageW(g_logView, WM_SETFONT, reinterpret_cast<WPARAM>(g_monoFont), TRUE);
        SetTimer(window, kRefreshTimer, 1000, nullptr);
        Refresh();
        return 0;
    case WM_SIZE:
        Layout(window);
        return 0;
    case WM_TIMER:
        if (wParam == kRefreshTimer) Refresh();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kBuild: StartBuild(); break;
        case kDeploy: Deploy(); break;
        case kLaunch: LaunchGame(); break;
        case kDeployLaunch: if (Deploy()) LaunchGame(); break;
        case kDisable: DisableAsi(); break;
        case kRefresh: Refresh(); break;
        case kOpenLog: OpenPath(g_log, L"portal.log 尚未生成。"); break;
        case kOpenFrame: OpenPath(g_frame, L"portal-frame.bmp 尚未生成。"); break;
        case kOpenDev: OpenPath(g_dev, L"dev 游戏目录不存在。"); break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, kRefreshTimer);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int showCommand) {
    g_instance = instance;
    SetProcessDPIAware();
    g_repo = FindRepoRoot();
    if (g_repo.empty()) {
        MessageBoxW(nullptr, L"无法定位 GTAPortal 仓库。请保留 PortalDev.exe 在仓库的构建目录中。",
                    L"GTAPortal Dev", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_dev = g_repo / L"game" / L"dev" / L"GTA San Andreas";
    g_sourceAsi = g_repo / L"src" / L"PortalSA" / L"bin" / L"GTA-SA" / L"Release" / L"PortalSA.asi";
    g_deployedAsi = g_dev / L"PortalSA.asi";
    g_disabledAsi = g_dev / L"PortalSA.asi.disabled";
    g_log = g_dev / L"portal.log";
    g_frame = g_dev / L"portal-frame.bmp";
    g_game = g_dev / L"gta-sa.EXE";
    g_gameMd5 = fs::exists(g_game) ? HashMd5(g_game) : L"";

    g_uiFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    g_monoFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH, L"Consolas");

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    g_window = CreateWindowExW(0, kWindowClass, L"GTAPortal Dev — GTA SA Milestone 1",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 680,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        return 1;
    }
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DeleteObject(g_uiFont);
    DeleteObject(g_monoFont);
    return static_cast<int>(message.wParam);
}
