#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <d3d9.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "../../PortalProtocol.h"
#include "GpuCapture.h"

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"GTAPortalDevWindow";
constexpr wchar_t kExpectedMd5[] = L"170B3A9108687B26DA2D8901C6948A18";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kFrameTimer = 2;

enum ControlId : int {
    kBuild = 1001,
    kDeploy = 1002,
    kLaunch = 1003,
    kDeployLaunch = 1004,
    kRefresh = 1005,
    kOpenLog = 1006,
    kDisable = 1009,
    kGpuMode = 1010,
};

HINSTANCE g_instance{};
HWND g_window{};
HWND g_status{};
HWND g_logView{};
HWND g_preview{};
HWND g_gpuMode{};
HFONT g_uiFont{};
HFONT g_monoFont{};
fs::path g_repo;
fs::path g_dev;
fs::path g_sourceAsi;
fs::path g_deployedAsi;
fs::path g_disabledAsi;
fs::path g_log;
fs::path g_game;
std::wstring g_gameMd5;
HANDLE g_mapping{};
HANDLE g_supervisorMutex{};
portal::SharedFrames* g_frames{};
IDirect3D9Ex* g_d3d{};
IDirect3DDevice9Ex* g_device{};
IDirect3DQuery9* g_readQuery{};
std::array<IDirect3DTexture9*, portal::kSlotCount> g_sharedTextures{};
std::array<uint64_t, portal::kSlotCount> g_openHandles{};
LONG g_openGeneration{};
int g_readingSlot = -1;
uint64_t g_lastFrameId{};
uint64_t g_received{};
double g_lastAgeMs{};
double g_ageP95Ms{};
double g_lastGpuReadyMs{};
double g_lastSubmitMs{};
bool g_lastCameraValid{};
std::array<float, 3> g_lastCameraPosition{};
float g_lastFov{};
uint32_t g_lastStateAgeMs{};
std::wstring g_gpuStatus = L"等待 SA 生产者";
LARGE_INTEGER g_qpcFrequency{};
int64_t g_lastRefreshQpc{};
LONG g_lastProduced{};
uint64_t g_lastReceived{};
double g_producerFps{};
double g_consumerFps{};
std::array<double, 600> g_ageSamples{};
size_t g_ageCount{};
size_t g_ageNext{};
int g_scrollY{};
std::wstring g_statusText;
std::wstring g_logText;
GpuCapture g_capture;
bool g_captureEnabled = true;
DWORD g_lastCaptureAttemptTick{};
uint64_t g_lastCaptureFrames{};
double g_captureFps{};

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

bool IsGameRunning(DWORD* gamePid = nullptr) {
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
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                if (process) {
                    std::array<wchar_t, 32768> path{};
                    DWORD length = static_cast<DWORD>(path.size());
                    found = QueryFullProcessImageNameW(process, 0, path.data(), &length) &&
                        _wcsicmp(path.data(), g_game.c_str()) == 0;
                    CloseHandle(process);
                    if (found) {
                        if (gamePid) *gamePid = entry.th32ProcessID;
                        break;
                    }
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

HWND FindGameWindow(DWORD gamePid) {
    struct Search { DWORD pid; HWND window; } search{gamePid, nullptr};
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

bool StartSupervisor() {
    g_supervisorMutex = CreateMutexW(nullptr, TRUE, L"Local\\GTAPortal.Supervisor.v1");
    if (!g_supervisorMutex) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_supervisorMutex);
        g_supervisorMutex = nullptr;
        return false;
    }
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   sizeof(portal::SharedFrames), portal::kMappingName);
    if (!g_mapping) return false;
    const bool existing = GetLastError() == ERROR_ALREADY_EXISTS;
    g_frames = static_cast<portal::SharedFrames*>(MapViewOfFile(
        g_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(portal::SharedFrames)));
    if (!g_frames) return false;
    if (!existing || g_frames->magic != portal::kMagic ||
        g_frames->version != portal::kVersion || g_frames->size != sizeof(portal::SharedFrames)) {
        ZeroMemory(g_frames, sizeof(*g_frames));
        g_frames->size = sizeof(*g_frames);
        g_frames->version = portal::kVersion;
        MemoryBarrier();
        g_frames->magic = portal::kMagic;
    }
    InterlockedExchange(&g_frames->bridgeEnabled, 0);
    QueryPerformanceFrequency(&g_qpcFrequency);
    return true;
}

int64_t Qpc() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

void ReleaseSharedTextures() {
    for (int index = 0; index < portal::kSlotCount; ++index) {
        if (g_sharedTextures[index]) g_sharedTextures[index]->Release();
        g_sharedTextures[index] = nullptr;
        g_openHandles[index] = 0;
    }
}

bool StartPreviewDevice() {
    if (g_device) return true;
    if (!g_d3d && FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d))) {
        g_gpuStatus = L"D3D9Ex 初始化失败";
        return false;
    }
    UINT adapter = D3DADAPTER_DEFAULT;
    bool matched = false;
    for (UINT index = 0; index < g_d3d->GetAdapterCount(); ++index) {
        LUID luid{};
        if (SUCCEEDED(g_d3d->GetAdapterLUID(index, &luid)) &&
            luid.LowPart == g_frames->adapterLuidLow && luid.HighPart == g_frames->adapterLuidHigh) {
            adapter = index;
            matched = true;
            break;
        }
    }
    if (!matched) {
        g_gpuStatus = L"未找到与 SA 相同的显卡适配器";
        return false;
    }
    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth = 640;
    params.BackBufferHeight = 360;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    params.MultiSampleType = D3DMULTISAMPLE_NONE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = g_preview;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT result = g_d3d->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, g_preview,
                                            D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                            &params, nullptr, &g_device);
    if (SUCCEEDED(result)) result = g_device->CreateQuery(D3DQUERYTYPE_EVENT, &g_readQuery);
    if (FAILED(result)) {
        if (g_device) { g_device->Release(); g_device = nullptr; }
        std::wostringstream message;
        message << L"预览设备创建失败 0x" << std::hex << static_cast<unsigned>(result);
        g_gpuStatus = message.str();
        return false;
    }
    g_gpuStatus = L"GPU 预览设备已连接";
    return true;
}

void ConsumeGpuFrame() {
    if (!g_frames || !g_frames->producerPid) return;
    const LONG generation = InterlockedCompareExchange(&g_frames->generation, 0, 0);
    if (!generation) return;
    if (g_readingSlot >= 0) {
        const HRESULT done = g_readQuery->GetData(nullptr, 0, 0);
        if (done == S_FALSE) return;
        InterlockedCompareExchange(&g_frames->slots[g_readingSlot].state,
                                   portal::kFree, portal::kReading);
        g_readingSlot = -1;
        if (FAILED(done)) {
            g_gpuStatus = L"GPU 消费完成查询失败";
            return;
        }
        ++g_received;
    }
    if (g_openGeneration != generation) {
        ReleaseSharedTextures();
        g_openGeneration = generation;
        g_lastFrameId = 0;
        g_ageCount = 0;
        g_ageNext = 0;
        g_ageP95Ms = 0;
    }
    if (!StartPreviewDevice()) return;

    int newest = -1;
    for (int index = 0; index < portal::kSlotCount; ++index) {
        auto& slot = g_frames->slots[index];
        if (InterlockedCompareExchange(&slot.state, 0, 0) == portal::kReady &&
            slot.frameId > g_lastFrameId &&
            (newest < 0 || slot.frameId > g_frames->slots[newest].frameId)) newest = index;
    }
    if (newest < 0) return;
    auto& slot = g_frames->slots[newest];
    if (InterlockedCompareExchange(&slot.state, portal::kReading, portal::kReady) != portal::kReady) return;
    if (g_openHandles[newest] != slot.sharedHandle) {
        if (g_sharedTextures[newest]) g_sharedTextures[newest]->Release();
        g_sharedTextures[newest] = nullptr;
        HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(slot.sharedHandle));
        const HRESULT opened = g_device->CreateTexture(slot.width, slot.height, 1,
            D3DUSAGE_RENDERTARGET, static_cast<D3DFORMAT>(slot.format), D3DPOOL_DEFAULT,
            &g_sharedTextures[newest], &handle);
        if (FAILED(opened)) {
            std::wostringstream message;
            message << L"共享纹理打开失败 0x" << std::hex << static_cast<unsigned>(opened);
            g_gpuStatus = message.str();
            InterlockedExchange(&slot.state, portal::kFree);
            return;
        }
        g_openHandles[newest] = slot.sharedHandle;
    }

    IDirect3DSurface9* source = nullptr;
    IDirect3DSurface9* target = nullptr;
    HRESULT result = g_sharedTextures[newest]->GetSurfaceLevel(0, &source);
    if (SUCCEEDED(result)) result = g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &target);
    if (SUCCEEDED(result)) result = g_device->StretchRect(source, nullptr, target, nullptr, D3DTEXF_LINEAR);
    if (source) source->Release();
    if (target) target->Release();
    if (SUCCEEDED(result)) result = g_readQuery->Issue(D3DISSUE_END);
    if (SUCCEEDED(result)) {
        g_readingSlot = newest;
        RECT destination{};
        GetClientRect(g_preview, &destination);
        result = g_device->PresentEx(nullptr, &destination, nullptr, nullptr, 0);
    }
    if (FAILED(result)) {
        if (g_readingSlot < 0) InterlockedExchange(&slot.state, portal::kFree);
        std::wostringstream message;
        message << L"GPU 预览失败 0x" << std::hex << static_cast<unsigned>(result);
        g_gpuStatus = message.str();
        return;
    }
    const int64_t now = Qpc();
    g_lastFrameId = slot.frameId;
    g_lastAgeMs = 1000.0 * static_cast<double>(now - slot.captureQpc) / g_qpcFrequency.QuadPart;
    g_ageSamples[g_ageNext] = g_lastAgeMs;
    g_ageNext = (g_ageNext + 1) % g_ageSamples.size();
    if (g_ageCount < g_ageSamples.size()) ++g_ageCount;
    g_lastGpuReadyMs = 1000.0 * static_cast<double>(slot.readyQpc - slot.captureQpc) / g_qpcFrequency.QuadPart;
    g_lastSubmitMs = slot.submitUs / 1000.0;
    g_lastCameraValid = slot.cameraValid != 0;
    g_lastCameraPosition[0] = slot.cameraPosition[0];
    g_lastCameraPosition[1] = slot.cameraPosition[1];
    g_lastCameraPosition[2] = slot.cameraPosition[2];
    g_lastFov = slot.fovDegrees;
    g_lastStateAgeMs = slot.stateAgeMs;
    g_gpuStatus = L"GPU 帧链路正常";
}

void CaptureGameFrame() {
    if (!g_captureEnabled) {
        g_capture.Stop();
        return;
    }
    DWORD pid = 0;
    const HWND gameWindow = IsGameRunning(&pid) ? FindGameWindow(pid) : nullptr;
    if (!gameWindow) {
        g_capture.Stop();
        return;
    }
    if (g_capture.Running() && g_capture.GameWindow() != gameWindow) g_capture.Stop();
    if (!g_capture.Running()) {
        const DWORD now = GetTickCount();
        if (g_lastCaptureAttemptTick && now - g_lastCaptureAttemptTick < 2000) return;
        g_lastCaptureAttemptTick = now;
        if (!g_capture.Start(gameWindow, g_preview)) return;
    }
    g_capture.Pump();
}

void UpdateText(HWND control, std::wstring& previous, const std::wstring& next, bool followTail) {
    if (previous == next) return;
    const int firstVisible = static_cast<int>(SendMessageW(control, EM_GETFIRSTVISIBLELINE, 0, 0));
    const int lineCount = static_cast<int>(SendMessageW(control, EM_GETLINECOUNT, 0, 0));
    const bool atTail = followTail && firstVisible >= std::max(0, lineCount - 15);
    SendMessageW(control, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(control, next.c_str());
    if (atTail) {
        SendMessageW(control, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(control, EM_SCROLLCARET, 0, 0);
    } else {
        SendMessageW(control, EM_LINESCROLL, 0, firstVisible);
    }
    SendMessageW(control, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(control, nullptr, FALSE);
    previous = next;
}

void Refresh() {
    if (g_ageCount) {
        std::vector<double> ages(g_ageSamples.begin(), g_ageSamples.begin() + g_ageCount);
        const size_t index = (95 * g_ageCount + 99) / 100 - 1;
        std::nth_element(ages.begin(), ages.begin() + index, ages.end());
        g_ageP95Ms = ages[index];
    }
    const int64_t now = Qpc();
    if (g_lastRefreshQpc) {
        const double seconds = static_cast<double>(now - g_lastRefreshQpc) / g_qpcFrequency.QuadPart;
        if (seconds > 0) {
            g_captureFps = (g_capture.Frames() - g_lastCaptureFrames) / seconds;
        }
    }
    g_lastRefreshQpc = now;
    g_lastProduced = g_frames->produced;
    g_lastReceived = g_received;
    g_lastCaptureFrames = g_capture.Frames();
    const bool running = IsGameRunning();
    const bool sourceReady = fs::exists(g_sourceAsi);
    const bool deployed = fs::exists(g_deployedAsi);
    const bool matching = sourceReady && deployed && SameFile(g_sourceAsi, g_deployedAsi);
    const bool logReady = fs::exists(g_log);
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
           << L"    GPU 画面：" << (g_captureEnabled ? g_capture.Status() : L"已关闭") << L"\r\n"
           << L"捕获帧数：" << g_capture.Frames()
           << L"    捕获速度：" << std::fixed << std::setprecision(1) << g_captureFps << L" FPS"
           << L"    画面：" << g_capture.Width() << L" × " << g_capture.Height() << L"\r\n"
           << L"相机元数据：见下方 PortalSA 日志"
           << L"\r\n仓库：" << g_repo.wstring();
    UpdateText(g_status, g_statusText, status.str(), false);

    const std::wstring wideLog = logReady ? Widen(logBytes) : L"portal.log 尚未生成。部署 ASI 并手动启动 dev 游戏后，此处会自动刷新。";
    UpdateText(g_logView, g_logText, wideLog, true);
}

bool Deploy() {
    if (IsGameRunning()) {
        ShowError(L"无法部署", L"请先关闭 GTA San Andreas，再替换 PortalSA.asi。");
        return false;
    }
    if (g_gameMd5 != kExpectedMd5) {
        ShowError(L"版本检查失败", L"dev 目录中的 gta-sa.EXE 不是已确认的 HOODLUM 1.0 US，已阻止部署。");
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
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    const int statusHeight = 194;
    const int statusY = padding + 2 * (buttonHeight + gap) + 2;
    const int contentWidth = clientWidth - padding * 2;
    const int previewHeight = contentWidth * 9 / 16;
    const int contentHeight = statusY + statusHeight + 6 + previewHeight + 8 + 200 + padding;
    g_scrollY = std::clamp(g_scrollY, 0, std::max(0, contentHeight - clientHeight));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMax = contentHeight - 1;
    scroll.nPage = static_cast<UINT>(clientHeight);
    scroll.nPos = g_scrollY;
    SetScrollInfo(window, SB_VERT, &scroll, TRUE);

    int x = padding;
    for (const auto [id, width] : std::array<std::pair<int, int>, 4>{{
             {kBuild, 96}, {kDeploy, 86}, {kDeployLaunch, 146}, {kLaunch, 118}}}) {
        MoveWindow(GetDlgItem(window, id), x, padding - g_scrollY, width, buttonHeight, TRUE);
        x += width + gap;
    }
    x = padding;
    for (const auto [id, width] : std::array<std::pair<int, int>, 3>{{
             {kDisable, 86}, {kRefresh, 76}, {kOpenLog, 86}}}) {
        MoveWindow(GetDlgItem(window, id), x, padding + buttonHeight + gap - g_scrollY,
                   width, buttonHeight, TRUE);
        x += width + gap;
    }
    MoveWindow(g_gpuMode, x + 8, padding + buttonHeight + gap - g_scrollY,
               300, buttonHeight, TRUE);
    MoveWindow(g_status, padding, statusY - g_scrollY, contentWidth, statusHeight, TRUE);
    const int previewY = statusY + statusHeight + 6;
    MoveWindow(g_preview, padding, previewY - g_scrollY, contentWidth, previewHeight, TRUE);
    const int logY = previewY + previewHeight + 8;
    MoveWindow(g_logView, padding, logY - g_scrollY, contentWidth, 200, TRUE);
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
        AddButton(window, kLaunch, L"打开游戏目录");
        AddButton(window, kDeployLaunch, L"部署并打开目录");
        AddButton(window, kDisable, L"停用 ASI");
        AddButton(window, kRefresh, L"刷新");
        AddButton(window, kOpenLog, L"打开日志");
        g_gpuMode = CreateWindowExW(0, L"BUTTON", L"GPU 窗口捕获（运行中可切换）",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    0, 0, 1, 1, window,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGpuMode)),
                                    g_instance, nullptr);
        SendMessageW(g_gpuMode, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        SendMessageW(g_gpuMode, BM_SETCHECK, g_captureEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                   0, 0, 1, 1, window, nullptr, g_instance, nullptr);
        g_logView = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                    0, 0, 1, 1, window, nullptr, g_instance, nullptr);
        g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"等待共享画面",
                                    WS_CHILD | WS_VISIBLE, 0, 0, 640, 360,
                                    window, nullptr, g_instance, nullptr);
        SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        SendMessageW(g_logView, WM_SETFONT, reinterpret_cast<WPARAM>(g_monoFont), TRUE);
        SetTimer(window, kRefreshTimer, 1000, nullptr);
        SetTimer(window, kFrameTimer, 15, nullptr);
        Refresh();
        return 0;
    case WM_SIZE:
        Layout(window);
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {620, 500};
        return 0;
    case WM_VSCROLL: {
        if (lParam) return 0;
        SCROLLINFO scroll{sizeof(scroll), SIF_ALL};
        GetScrollInfo(window, SB_VERT, &scroll);
        int next = g_scrollY;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: next -= 40; break;
        case SB_LINEDOWN: next += 40; break;
        case SB_PAGEUP: next -= static_cast<int>(scroll.nPage); break;
        case SB_PAGEDOWN: next += static_cast<int>(scroll.nPage); break;
        case SB_THUMBTRACK: next = scroll.nTrackPos; break;
        default: return 0;
        }
        g_scrollY = std::clamp(next, 0, std::max(0, scroll.nMax + 1 - static_cast<int>(scroll.nPage)));
        Layout(window);
        return 0;
    }
    case WM_MOUSEWHEEL:
        g_scrollY = std::max(0, g_scrollY - GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 80);
        Layout(window);
        return 0;
    case WM_TIMER:
        if (wParam == kRefreshTimer) Refresh();
        if (wParam == kFrameTimer) CaptureGameFrame();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kBuild: StartBuild(); break;
        case kDeploy: Deploy(); break;
        case kLaunch: OpenPath(g_dev, L"dev 游戏目录不存在。"); break;
        case kDeployLaunch: if (Deploy()) OpenPath(g_dev, L"dev 游戏目录不存在。"); break;
        case kDisable: DisableAsi(); break;
        case kRefresh: Refresh(); break;
        case kOpenLog: OpenPath(g_log, L"portal.log 尚未生成。"); break;
        case kGpuMode:
            g_captureEnabled = SendMessageW(g_gpuMode, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!g_captureEnabled) g_capture.Stop();
            Refresh();
            break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, kRefreshTimer);
        KillTimer(window, kFrameTimer);
        g_capture.Stop();
        ReleaseSharedTextures();
        if (g_readQuery) g_readQuery->Release();
        if (g_device) g_device->Release();
        if (g_d3d) g_d3d->Release();
        if (g_frames && g_readingSlot >= 0) {
            InterlockedCompareExchange(&g_frames->slots[g_readingSlot].state,
                                       portal::kFree, portal::kReading);
        }
        if (g_frames) UnmapViewOfFile(g_frames);
        if (g_mapping) CloseHandle(g_mapping);
        if (g_supervisorMutex) {
            ReleaseMutex(g_supervisorMutex);
            CloseHandle(g_supervisorMutex);
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int showCommand) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
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
    g_game = g_dev / L"gta-sa.EXE";
    g_gameMd5 = fs::exists(g_game) ? HashMd5(g_game) : L"";
    if (!StartSupervisor()) {
        MessageBoxW(nullptr, L"无法建立共享帧控制区，或 PortalDev 已在运行。", L"GTAPortal Dev", MB_OK | MB_ICONERROR);
        return 1;
    }

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

    g_window = CreateWindowExW(0, kWindowClass, L"GTAPortal Dev — GPU Supervisor",
                               WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 980, 860,
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
