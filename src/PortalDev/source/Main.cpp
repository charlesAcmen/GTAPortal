#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "../../DevelopmentAnchors.h"
#include "../../PortalVisualConfig.h"
#include "../../PortalProtocol.h"
#include "../../PortalVProtocol.h"
#include "GpuCapture.h"
#include "GamePaths.h"
#include "GameProcess.h"
#include "PortalVClient.h"
#include "PortalCameraClient.h"

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"GTAPortalDevWindow";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kFrameTimer = 2;

enum ControlId : int {
    kBuild = 1001,
    kDeploy = 1002,
    kOpenLog = 1006,
    kSelectSa = 1011,
    kSelectGtaV = 1012,
    kDeployV = 1013,
    kOpenVLog = 1016,
    kTeleportV = 1019,
    kPortalCamera = 1020,
    kCaptureSaAnchor = 1021,
    kSaveCalibration = 1022,
    kShowVPortal = 1023,
    kShowSaPortal = 1024,
    kHotReloadVFx = 1025,
    kSaToVCamera = 1026,
    kSaPortalX = 1030,
    kSaPortalY = 1031,
    kSaPortalZ = 1032,
    kSaPortalYaw = 1033,
};

HINSTANCE g_instance{};
HWND g_window{};
HWND g_saStatus{};
HWND g_vStatus{};
HWND g_saHeading{};
HWND g_vHeading{};
HWND g_vSafety{};
HWND g_portalCameraMode{};
HWND g_saToVCameraMode{};
HWND g_showVPortalMode{};
HWND g_showSaPortalMode{};
HWND g_saPortalEdits[4]{};
HWND g_calibrationLabel{};
HFONT g_uiFont{};
HFONT g_monoFont{};
HFONT g_headingFont{};
fs::path g_repo;
fs::path g_dev;
fs::path g_sourceAsi;
fs::path g_deployedAsi;
fs::path g_disabledAsi;
fs::path g_log;
fs::path g_game;
std::wstring g_gameMd5;
bool g_saValid{};
fs::path g_gtaV;
std::wstring g_gtaVVersion;
bool g_gtaVValid{};
fs::path g_sourceVAsi;
PortalVClient g_vClient;
PortalVClient::Snapshot g_vSnapshot;
bool g_vConnected{};
PortalCameraClient g_portalCameraClient;
PortalCameraClient::Snapshot g_saCameraSnapshot;
bool g_saCameraConnected{};
uint32_t g_lastSaPortalPlacementSequence{};
bool g_portalCameraEnabled{};
bool g_saToVCameraEnabled{};
bool g_showVPortal{true};
bool g_showSaPortal{};
// The confirmed SA development TP is the portal ground contact. Store the
// camera-mapping origin at the center of the default 2.4 x 1.35 m aperture.
float g_saPortal[4]{portal_visual::gta_sa::groundX,
                    portal_visual::gta_sa::groundY,
                    portal_visual::gta_sa::groundZ + portal_visual::kCenterAboveGround,
                    portal_visual::gta_sa::yawDegrees};
HANDLE g_mapping{};
HANDLE g_supervisorMutex{};
portal::SharedFrames* g_frames{};
LARGE_INTEGER g_qpcFrequency{};
int64_t g_lastRefreshQpc{};
int g_scrollY{};
std::wstring g_saStatusText;
std::wstring g_vStatusText;
GpuCapture g_capture;
GpuCapture g_vCapture;
DWORD g_lastCaptureAttemptTick{};
DWORD g_lastVCaptureAttemptTick{};
uint64_t g_lastCaptureFrames{};
uint64_t g_lastVCaptureFrames{};
double g_captureFps{};
double g_vCaptureFps{};

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

bool IsGameRunning(DWORD* gamePid = nullptr) {
    return game_process::FindByExecutablePath(g_game, gamePid);
}

bool IsVRunning(DWORD* gamePid = nullptr) {
    return g_gtaVValid && game_process::FindByExecutablePath(
        g_gtaV / L"GTA5_Enhanced.exe", gamePid);
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
           game_paths::HashMd5(left) == game_paths::HashMd5(right);
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

void CaptureGameFrame() {
    DWORD pid = 0;
    const HWND gameWindow = IsGameRunning(&pid) ? game_process::FindVisibleWindow(pid) : nullptr;
    if (!gameWindow) {
        g_capture.Stop();
        return;
    }
    if (g_capture.Running() && g_capture.GameWindow() != gameWindow) g_capture.Stop();
    if (!g_capture.Running()) {
        const DWORD now = GetTickCount();
        if (g_lastCaptureAttemptTick && now - g_lastCaptureAttemptTick < 2000) return;
        g_lastCaptureAttemptTick = now;
        // Transport stays active, but PortalDev does not create a preview swap chain by default.
        if (!g_capture.Start(gameWindow, nullptr, GpuCapture::Publication::Sa)) return;
    }
    g_capture.Pump();
}

void CaptureVFrame() {
    if (!g_gtaVValid) {
        g_vCapture.Stop();
        return;
    }
    if (g_vCapture.Running()) {
        if (IsWindow(g_vCapture.GameWindow())) {
            g_vCapture.Pump();
            return;
        }
        g_vCapture.Stop();
    }
    const DWORD now = GetTickCount();
    if (g_lastVCaptureAttemptTick && now - g_lastVCaptureAttemptTick < 500) return;
    g_lastVCaptureAttemptTick = now;
    DWORD pid = 0;
    const HWND gameWindow = IsVRunning(&pid) ? game_process::FindVisibleWindow(pid) : nullptr;
    if (gameWindow && g_vCapture.Start(gameWindow, nullptr, GpuCapture::Publication::V)) g_vCapture.Pump();
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

void LoadCalibration() {
    std::ifstream file(g_dev / L"PortalCalibration.txt");
    if (!file) return;
    std::string line;
    std::getline(file, line);
    std::istringstream input(line);
    int version = 0;
    float value[4]{};
    if (input >> version && version == 3 &&
        input >> value[0] >> value[1] >> value[2] >> value[3]) {
        std::copy(std::begin(value), std::end(value), std::begin(g_saPortal));
        return;
    }
    std::istringstream versionTwo(line);
    if (versionTwo >> version && version == 2 &&
        versionTwo >> value[0] >> value[1] >> value[2] >> value[3]) {
        g_saPortal[0] = value[0];
        g_saPortal[1] = value[1];
        g_saPortal[2] = value[2] + portal_visual::kCenterAboveGround;
        g_saPortal[3] = value[3];
        return;
    }
    std::istringstream legacy(line);
    if (legacy >> value[0] >> value[1] >> value[2] >> value[3]) {
        g_saPortal[0] = value[0];
        g_saPortal[1] = value[1];
        g_saPortal[2] = value[2] - 1.10f + portal_visual::kCenterAboveGround;
        g_saPortal[3] = value[3];
    }
}

void UpdateCalibrationEdits() {
    for (int index = 0; index < 4; ++index) {
        if (!g_saPortalEdits[index]) continue;
        wchar_t text[32]{};
        swprintf_s(text, L"%.6f", g_saPortal[index]);
        SetWindowTextW(g_saPortalEdits[index], text);
    }
}

bool ReadCalibrationEdits() {
    float values[4]{};
    for (int index = 0; index < 4; ++index) {
        wchar_t text[64]{};
        GetWindowTextW(g_saPortalEdits[index], text, static_cast<int>(std::size(text)));
        wchar_t* end = nullptr;
        values[index] = std::wcstof(text, &end);
        if (end == text || *end != L'\0' || !std::isfinite(values[index])) return false;
    }
    std::copy(std::begin(values), std::end(values), std::begin(g_saPortal));
    return true;
}

bool SaveCalibration() {
    if (!ReadCalibrationEdits()) return false;
    std::ofstream file(g_dev / L"PortalCalibration.txt", std::ios::trunc);
    if (!file) return false;
    file << "3 " << std::fixed << std::setprecision(6)
         << g_saPortal[0] << ' ' << g_saPortal[1] << ' '
         << g_saPortal[2] << ' '
         << g_saPortal[3] << '\n';
    return file.good();
}

struct Vec3f { float x, y, z; };
Vec3f Add(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3f Scale(Vec3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3f Unit(Vec3f value) {
    const float length = std::sqrt(Dot(value, value));
    return length > 0.0001f ? Scale(value, 1.0f / length) : Vec3f{};
}

void PortalBasis(float yawDegrees, Vec3f& right, Vec3f& forward, Vec3f& up) {
    const float yaw = yawDegrees * 0.01745329251994329577f;
    right = {std::cos(yaw), std::sin(yaw), 0.0f};
    forward = {-std::sin(yaw), std::cos(yaw), 0.0f};
    up = {0.0f, 0.0f, 1.0f};
}

Vec3f MapBasisVector(Vec3f value, Vec3f sourceRight, Vec3f sourceForward, Vec3f sourceUp,
                     Vec3f targetRight, Vec3f targetForward, Vec3f targetUp) {
    return Add(Add(Scale(targetRight, Dot(value, sourceRight)),
                   Scale(targetForward, Dot(value, sourceForward))),
               Scale(targetUp, Dot(value, sourceUp)));
}

void UpdatePortalCamera() {
    g_saCameraConnected = g_portalCameraClient.Read(g_saCameraSnapshot);
    if (g_saCameraConnected && g_saCameraSnapshot.portalPlacementSequence &&
        g_saCameraSnapshot.portalPlacementSequence != g_lastSaPortalPlacementSequence) {
        g_lastSaPortalPlacementSequence = g_saCameraSnapshot.portalPlacementSequence;
        g_saPortal[0] = g_saCameraSnapshot.portalPlacement[0];
        g_saPortal[1] = g_saCameraSnapshot.portalPlacement[1];
        g_saPortal[2] = g_saCameraSnapshot.portalPlacement[2];
        g_saPortal[3] = g_saCameraSnapshot.portalPlacement[3];
        UpdateCalibrationEdits();
    }
    PortalVClient::Snapshot liveV{};
    const bool vReady = g_vClient.Read(liveV) && liveV.cameraValid;
    if (vReady) {
        g_vSnapshot = liveV;
        g_vConnected = true;
        if (liveV.portalVisible != g_showVPortal) g_vClient.SetPortalVisible(g_showVPortal);
    }
    const float zero[3]{};
    if (g_saToVCameraEnabled && g_saCameraConnected &&
        g_saCameraSnapshot.cameraValid && vReady && liveV.portalPlaneValid) {
        Vec3f saRight{},saPortalForward{},saUp{};
        Vec3f vRight{},vPortalForward{},vUp{};
        PortalBasis(g_saPortal[3],saRight,saPortalForward,saUp);
        PortalBasis(liveV.portalPlaneYawDegrees,vRight,vPortalForward,vUp);
        const Vec3f saLook=Unit({g_saCameraSnapshot.cameraForward[0],
                                g_saCameraSnapshot.cameraForward[1],
                                g_saCameraSnapshot.cameraForward[2]});
        const Vec3f vLook=Unit(MapBasisVector(saLook,saRight,saPortalForward,saUp,
                                               vRight,vPortalForward,vUp));
        constexpr float radiansToDegrees=57.295779513082320876f;
        const float rotation[3]{
            std::asin(std::clamp(vLook.z,-1.0f,1.0f))*radiansToDegrees,
            0.0f,
            std::atan2(-vLook.x,vLook.y)*radiansToDegrees};
        const float position[3]{
            liveV.portalPlaneCenter[0],liveV.portalPlaneCenter[1],
            liveV.portalPlaneCenter[2]+portal_visual::kGtaVPortalCameraHeightOffset};
        const float horizontalFov=std::clamp(g_saCameraSnapshot.cameraFovDegrees,5.0f,170.0f);
        const float horizontalRadians=horizontalFov/radiansToDegrees;
        const float verticalFov=2.0f*std::atan(std::tan(horizontalRadians*0.5f)/(16.0f/9.0f)) *
                                radiansToDegrees;
        g_vClient.PublishCamera(true,position,rotation,verticalFov);
    } else if (vReady) {
        g_vClient.PublishCamera(false,zero,zero,70.0f);
    }

    if (!g_portalCameraEnabled || !vReady || !liveV.portalPlaneValid) {
        if (g_saCameraConnected) {
            g_portalCameraClient.Publish(false, g_showSaPortal, zero, zero, zero, 70.0f);
        }
        return;
    }

    const float vYaw = liveV.portalPlaneYawDegrees;
    Vec3f vRight{}, vForward{}, vUp{};
    Vec3f saRight{}, saForward{}, saUp{};
    PortalBasis(vYaw, vRight, vForward, vUp);
    PortalBasis(g_saPortal[3],saRight,saForward,saUp);

    const Vec3f saCenter{g_saPortal[0], g_saPortal[1], g_saPortal[2]};
    // The SA portal camera is a fixed viewpoint at the circle center. GTA V
    // translation never moves it; only final rendered look direction/FOV map across.
    const Vec3f targetPosition=saCenter;

    const float yaw = liveV.renderedCameraRotation[2] * 0.01745329251994329577f;
    const float pitch = liveV.renderedCameraRotation[0] * 0.01745329251994329577f;
    const Vec3f cameraForward{-std::sin(yaw) * std::cos(pitch),
                              std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
    const Vec3f cameraRight{std::cos(yaw), std::sin(yaw), 0.0f};
    const Vec3f cameraUp{cameraRight.y * cameraForward.z,
                         -cameraRight.x * cameraForward.z,
                          cameraRight.x * cameraForward.y - cameraRight.y * cameraForward.x};
    const Vec3f targetForward = Unit(MapBasisVector(cameraForward, vRight, vForward, vUp,
                                                     saRight, saForward, saUp));
    const Vec3f targetUp = Unit(MapBasisVector(cameraUp, vRight, vForward, vUp,
                                                saRight, saForward, saUp));
    const float position[3]{targetPosition.x, targetPosition.y, targetPosition.z};
    const float forward[3]{targetForward.x, targetForward.y, targetForward.z};
    const float up[3]{targetUp.x, targetUp.y, targetUp.z};
    g_portalCameraClient.Publish(
        true, g_showSaPortal, position, forward, up, liveV.renderedFovDegrees);
}

void Refresh() {
    if (!g_gtaV.empty()) g_gtaVValid = game_paths::ValidateGtaVDirectory(g_gtaV, g_gtaVVersion);
    const int64_t now = Qpc();
    if (g_lastRefreshQpc) {
        const double seconds = static_cast<double>(now - g_lastRefreshQpc) / g_qpcFrequency.QuadPart;
        if (seconds > 0) {
            g_captureFps = (g_capture.Frames() - g_lastCaptureFrames) / seconds;
            g_vCaptureFps = (g_vCapture.Frames() - g_lastVCaptureFrames) / seconds;
        }
    }
    g_lastRefreshQpc = now;
    g_lastCaptureFrames = g_capture.Frames();
    g_lastVCaptureFrames = g_vCapture.Frames();
    const bool running = IsGameRunning();
    DWORD vPid = 0;
    const bool vRunning = IsVRunning(&vPid);
    g_vConnected = g_vClient.Read(g_vSnapshot) && vRunning && g_vSnapshot.processId == vPid;
    if (!g_vConnected) g_vClient.Disconnect();
    std::wstring vHookVersion;
    const bool vHookMatches = g_gtaVValid &&
        game_paths::ValidateGtaVHookVersion(g_gtaV, g_gtaVVersion, vHookVersion);
    const bool sourceReady = fs::exists(g_sourceAsi);
    const bool deployed = fs::exists(g_deployedAsi);
    const bool matching = sourceReady && deployed && SameFile(g_sourceAsi, g_deployedAsi);
    const std::string logBytes = ReadTailBytes(g_log);

    std::wostringstream saStatus;
    saStatus << L"版本：" << (g_saValid ? L"✓ HOODLUM 1.0 US" : L"✗ 未识别")
           << L"    MD5: " << (g_gameMd5.empty() ? L"读取失败" : g_gameMd5) << L"\r\n"
           << L"目录：" << g_dev.wstring() << L"\r\n"
           << L"ASI 构建：" << (sourceReady ? L"✓ " : L"✗ ")
           << (sourceReady ? SizeText(g_sourceAsi) : L"尚未生成")
           << L"    dev 部署：" << (matching ? L"✓ 已同步" : deployed ? L"△ 版本可能不同" : L"✗ 未部署")
           << L"    游戏：" << (running ? L"● 正在运行" : L"○ 未运行") << L"\r\n"
           << L"注入日志：" << (logBytes.find("PortalSA loaded") != std::string::npos ? L"✓ 已加载" : L"等待")
           << L"    渲染 Hook：" << (logBytes.find("Render output hook installed") != std::string::npos ? L"✓ 已安装" : L"等待")
           << L"    状态流：" << (logBytes.find("state tick_ms=") != std::string::npos ? L"✓ 有数据" : L"等待")
           << L"    SA → V 传输：" << g_capture.Status() << L"\r\n"
           << L"捕获帧数：" << g_capture.Frames()
           << L"    捕获速度：" << std::fixed << std::setprecision(1) << g_captureFps << L" FPS"
           << L"    画面：" << g_capture.Width() << L" × " << g_capture.Height() << L"\r\n"
           << L"窗口帧龄：最新 " << g_capture.LastAgeMs() << L" ms / P95 "
           << g_capture.AgeP95Ms() << L" ms    主动跳过旧帧：" << g_capture.DroppedFrames() << L"\r\n"
           << L"SA 共享帧：发布 " << g_capture.PublishedFrames()
           << L" / 覆盖旧帧 " << g_capture.OverwrittenFrames()
           << L" / 跳过 " << g_capture.PublishDrops() << L"\r\n"
           << L"Portal 相机控制：" << (g_saCameraConnected ? L"已连接" : L"等待 SA")
           << L" / " << (g_saCameraSnapshot.overrideActive ? L"SA 主相机同步中" : L"目标未启用")
           << L"；V 门 " << (g_showVPortal ? L"显示" : L"隐藏")
           << L" / SA 门 " << (g_showSaPortal ? L"显示" : L"隐藏") << L"\r\n"
           << g_vCapture.SaConsumerStatus() << L"；SA 前台按 F9 放置圆形传送门";
    UpdateText(g_saStatus, g_saStatusText, saStatus.str(), false);

    std::wostringstream vStatus;
    vStatus << L"版本：" << (g_gtaVValid ? L"✓ " + g_gtaVVersion : L"✗ 未选择或无效")
           << L"    游戏：" << (vRunning ? L"● 运行中" : L"○ 未运行") << L"\r\n"
           << L"目录：" << (g_gtaV.empty() ? L"未选择" : g_gtaV.wstring()) << L"\r\n"
           << L"Script Hook V：" << (vHookVersion.empty() ? L"未安装/无法读取版本" : vHookVersion)
           << L"    文件版本："
           << (vHookMatches ? L"一致" : g_vConnected ? L"不同，但 ASI 正在运行" : L"不同，需实测")
           << L"\r\n"
           << L"插件：" << (fs::exists(g_gtaV / L"PortalV.asi") ? L"已部署" : L"未部署")
           << L"    状态：" << (g_vConnected ? L"已连接" : L"未连接") << L"\r\n";
    if (g_vConnected) {
        vStatus << L"相机：(" << g_vSnapshot.cameraPosition[0] << L", "
               << g_vSnapshot.cameraPosition[1] << L", " << g_vSnapshot.cameraPosition[2]
               << L") FOV " << g_vSnapshot.fovDegrees << L"  帧 " << g_vSnapshot.frameId
               << L"  状态年龄 " << g_vSnapshot.ageMs << L" ms\r\n";
        vStatus << L"最终渲染相机：("
                << g_vSnapshot.renderedCameraPosition[0] << L", "
                << g_vSnapshot.renderedCameraPosition[1] << L", "
                << g_vSnapshot.renderedCameraPosition[2] << L") FOV "
                << g_vSnapshot.renderedFovDegrees << L"\r\n";
        vStatus << L"SA → V 相机："
                << (g_vSnapshot.cameraOverrideActive ? L"正在控制" : L"未启用")
                << L"\r\n";
        if (g_vSnapshot.portalPlaneValid) {
            vStatus << L"V 门锚点：("
                    << g_vSnapshot.portalPlaneCenter[0] << L", "
                    << g_vSnapshot.portalPlaneCenter[1] << L", "
                    << g_vSnapshot.portalPlaneCenter[2] << L") yaw "
                    << g_vSnapshot.portalPlaneYawDegrees << L"°\r\n";
        } else {
            vStatus << L"V 门锚点：等待 GTA V 前台按 F9\r\n";
        }
    }
    const wchar_t* teleportStatus = L"未请求";
    switch (g_vSnapshot.teleportResult) {
    case portal_v::kTeleportPending: teleportStatus = L"正在加载碰撞"; break;
    case portal_v::kTeleportSuccess: teleportStatus = L"成功并找到地面"; break;
    case portal_v::kTeleportRejectedOnline: teleportStatus = L"拒绝：检测到 Online"; break;
    case portal_v::kTeleportPlayerUnavailable: teleportStatus = L"等待 Story Mode 玩家"; break;
    case portal_v::kTeleportGroundUnavailable: teleportStatus = L"已到配置高度，但未确认地面"; break;
    default: break;
    }
    vStatus << L"测试 HUD：" << (g_vConnected && g_vSnapshot.overlayEnabled ? L"开启" : L"关闭/未连接") << L"\r\n"
           << L"开发 TP：" << teleportStatus << L"\r\n"
           << L"SA 矩形：" << (fs::exists(g_gtaV / L"reshade-shaders" / L"Shaders" / L"PortalSA.fx") ? L"着色器已部署" : L"着色器未部署")
           << L"；ReShade：" << (fs::exists(g_gtaV / L"ReShade64.asi") || fs::exists(g_gtaV / L"dxgi.dll") ? L"检测到运行库" : L"未检测到运行库")
            << L"；F9 从 TP 重设，ReShade 中实时微调并 Lock In\r\n"
           << L"V → SA 传输：" << g_vCapture.Status()
           << L"    " << g_vCapture.Width() << L" × " << g_vCapture.Height()
           << L"    " << std::fixed << std::setprecision(1) << g_vCaptureFps << L" FPS\r\n"
           << L"仅停用 PortalV.asi 不会移除 Script Hook V 或 ReShade，不能用于切换 Online。";
    UpdateText(g_vStatus, g_vStatusText, vStatus.str(), false);

}

bool Deploy() {
    if (IsGameRunning()) {
        ShowError(L"无法部署", L"请先关闭 GTA San Andreas，再替换 PortalSA.asi。");
        return false;
    }
    if (!g_saValid) {
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

void DeployV() {
    std::wstring version;
    if (!game_paths::ValidateGtaVDirectory(g_gtaV, version)) {
        ShowError(L"V 版本检查失败", L"请选择有效的 GTA V Enhanced 游戏根目录。");
        return;
    }
    if (IsVRunning()) {
        ShowError(L"请先关闭 GTA V", L"游戏运行时不能替换 PortalV.asi。");
        return;
    }
    if (!fs::exists(g_sourceVAsi)) {
        ShowError(L"尚未构建", L"找不到 PortalV.asi，请先重新构建。");
        return;
    }
    if (!fs::exists(g_gtaV / L"ScriptHookV.dll") || !fs::exists(g_gtaV / L"dinput8.dll")) {
        ShowError(L"缺少运行库", L"游戏根目录需要 ScriptHookV.dll 和 ASI loader (dinput8.dll)。");
        return;
    }
    const fs::path shaderSource = g_repo / L"src" / L"PortalV" / L"shaders" / L"PortalSA.fx";
    if (!fs::exists(shaderSource)) {
        ShowError(L"缺少着色器", L"找不到 src/PortalV/shaders/PortalSA.fx。");
        return;
    }
    const fs::path shaderDirectory = g_gtaV / L"reshade-shaders" / L"Shaders";
    std::error_code directoryError;
    fs::create_directories(shaderDirectory, directoryError);
    if (directoryError || !CopyFileW(shaderSource.c_str(), (shaderDirectory / L"PortalSA.fx").c_str(), FALSE)) {
        ShowError(L"V 着色器部署失败", WinError());
        return;
    }
    // Compatibility is reported in the status panel; allow a manual Story Mode probe.
    if (!CopyFileW(g_sourceVAsi.c_str(), (g_gtaV / L"PortalV.asi").c_str(), FALSE)) {
        ShowError(L"V 部署失败", WinError());
        return;
    }
    Refresh();
}

void HotReloadVFx(){
    if(!g_gtaVValid||!IsVRunning()){
        ShowError(L"V 尚未运行",L"请先进入已部署新版 PortalV.asi 的 GTA V Story Mode。");return;
    }
    const fs::path source=g_repo/L"src"/L"PortalV"/L"shaders"/L"PortalSA.fx";
    const fs::path target=g_gtaV/L"reshade-shaders"/L"Shaders"/L"PortalSA.fx";
    if(!fs::exists(source)||!CopyFileW(source.c_str(),target.c_str(),FALSE)){
        ShowError(L"FX 热更新失败",WinError());return;
    }
    if(!g_vClient.RequestFxReload()){
        ShowError(L"V 插件版本过旧",L"请关闭 GTA V，部署一次新版 PortalV.asi；以后改 FX 就不必重启。");return;
    }
}

void DisableV() {
    if (!g_gtaVValid || IsVRunning()) {
        ShowError(L"无法停用 V 插件", L"请确认 GTA V 目录有效，并先关闭游戏。");
        return;
    }
    const fs::path asi = g_gtaV / L"PortalV.asi";
    if (!fs::exists(asi)) return;
    if (!MoveFileExW(asi.c_str(), (g_gtaV / L"PortalV.asi.disabled").c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ShowError(L"V 停用失败", WinError());
    }
    Refresh();
}

void SetSaDirectory(const fs::path& directory) {
    g_dev = directory;
    g_deployedAsi = g_dev / L"PortalSA.asi";
    g_disabledAsi = g_dev / L"PortalSA.asi.disabled";
    g_log = g_dev / L"portal.log";
    g_game = g_dev / L"gta-sa.EXE";
    g_saValid = game_paths::ValidateSaDirectory(g_dev, g_gameMd5);
}

void SelectSaDirectory() {
    if (IsGameRunning()) {
        ShowError(L"请先关闭 SA", L"游戏运行时不能切换当前 SA 目录。");
        return;
    }
    const auto selected = game_paths::ChooseDirectory(g_window, g_dev, L"选择 GTA San Andreas 1.0 US 游戏目录");
    if (!selected) return;
    const fs::path clean = g_repo / L"game" / L"clean" / L"GTA San Andreas";
    std::error_code pathError;
    if (fs::exists(clean) && fs::equivalent(*selected, clean, pathError)) {
        ShowError(L"不能选择 clean 游戏", L"仓库内 game/clean 保留为原始副本。请选择 dev 目录或另一个开发副本。");
        return;
    }
    std::wstring md5;
    if (!game_paths::ValidateSaDirectory(*selected, md5)) {
        ShowError(L"不是支持的 SA 目录", L"需要 GTA SA 1.0 US HOODLUM 的 gta-sa.EXE。检测到的 MD5：" +
                  (md5.empty() ? L"读取失败" : md5));
        return;
    }
    g_capture.Stop();
    SetSaDirectory(*selected);
    LoadCalibration();
    UpdateCalibrationEdits();
    if (!game_paths::SaveDirectory(L"SaDirectory", g_dev)) {
        ShowError(L"保存路径失败", L"本次已切换 SA 目录，但无法保存到当前用户的设置。请检查注册表写入权限。");
    }
    Refresh();
}

void SelectGtaVDirectory() {
    const auto selected = game_paths::ChooseDirectory(g_window, g_gtaV, L"选择 GTA V Enhanced 游戏目录");
    if (!selected) return;
    std::wstring version;
    if (!game_paths::ValidateGtaVDirectory(*selected, version)) {
        ShowError(L"不是 GTA V Enhanced 目录",
                  L"目录中需要有效的 64 位 GTA5_Enhanced.exe，且版本信息应标明 Rockstar Games。请选游戏根目录。");
        return;
    }
    g_gtaV = *selected;
    g_gtaVVersion = version;
    g_gtaVValid = true;
    if (!game_paths::SaveDirectory(L"GtaVEnhancedDirectory", g_gtaV)) {
        ShowError(L"保存路径失败", L"本次已选择 GTA V 目录，但无法保存到当前用户的设置。请检查注册表写入权限。");
    }
    Refresh();
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
    const int contentWidth = clientWidth - padding * 2;
    const int headingHeight = 24;
    const int statusHeight = 165;
    int y = padding;
    const int commonY = y; y += buttonHeight + gap;
    const int saHeadingY = y; y += headingHeight + 4;
    const int saActionsY = y; y += buttonHeight + gap;
    const int saStatusY = y; y += statusHeight + gap;
    const int vHeadingY = y; y += headingHeight + 4;
    const int vSafetyY = y; y += 36 + gap;
    const int vActionsY = y; y += buttonHeight + gap;
    const int calibrationLabelY = y; y += headingHeight + 4;
    const int calibrationEditY = y; y += buttonHeight + gap;
    const int calibrationActionY = y; y += buttonHeight + gap;
    const int reverseCameraY = y; y += buttonHeight + gap;
    const int portalVisibilityY = y; y += buttonHeight + gap;
    const int vStatusY = y; y += statusHeight + gap;
    const int contentHeight = y + padding;
    g_scrollY = std::clamp(g_scrollY, 0, std::max(0, contentHeight - clientHeight));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMax = contentHeight - 1;
    scroll.nPage = static_cast<UINT>(clientHeight);
    scroll.nPos = g_scrollY;
    SetScrollInfo(window, SB_VERT, &scroll, TRUE);

    const auto place = [&](HWND control, int x, int top, int width, int height) {
        MoveWindow(control, x, top - g_scrollY, width, height, TRUE);
    };
    const auto button = [&](int id, int x, int top, int width) {
        place(GetDlgItem(window, id), x, top, width, buttonHeight);
    };
    button(kBuild, padding, commonY, 96);

    place(g_saHeading, padding, saHeadingY, contentWidth, headingHeight);
    int x = padding;
    for (const auto [id, width] : std::array<std::pair<int, int>, 3>{{
             {kSelectSa, 160}, {kDeploy, 100}, {kOpenLog, 110}}}) {
        button(id, x, saActionsY, width); x += width + gap;
    }
    place(g_saStatus, padding, saStatusY, contentWidth, statusHeight);

    place(g_vHeading, padding, vHeadingY, contentWidth, headingHeight);
    place(g_vSafety, padding, vSafetyY, contentWidth, 36);
    button(kSelectGtaV, padding, vActionsY, 190);
    button(kDeployV, padding + 190 + gap, vActionsY, 116);
    button(kOpenVLog, padding + 190 + gap + 116 + gap, vActionsY, 110);
    button(kTeleportV, padding + 190 + gap + 116 + gap + 110 + gap, vActionsY, 170);
    button(kHotReloadVFx,padding+190+gap+116+gap+110+gap+170+gap,vActionsY,110);
    place(g_calibrationLabel, padding, calibrationLabelY, contentWidth, headingHeight);
    const int calibrationEditWidth = (contentWidth - gap * 3) / 4;
    for (int index = 0; index < 4; ++index) {
        place(g_saPortalEdits[index], padding + index * (calibrationEditWidth + gap),
              calibrationEditY, calibrationEditWidth, buttonHeight);
    }
    place(g_portalCameraMode, padding, calibrationActionY, 260, buttonHeight);
    button(kCaptureSaAnchor, padding + 260 + gap, calibrationActionY, 180);
    button(kSaveCalibration, padding + 260 + gap + 180 + gap, calibrationActionY, 160);
    place(g_saToVCameraMode, padding, reverseCameraY, 260, buttonHeight);
    place(g_showVPortalMode, padding, portalVisibilityY, 250, buttonHeight);
    place(g_showSaPortalMode, padding + 250 + gap, portalVisibilityY, 250, buttonHeight);
    place(g_vStatus, padding, vStatusY, contentWidth, statusHeight);
}

HWND AddButton(HWND parent, int id, const wchar_t* text) {
    HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0, 0, 1, 1, parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  g_instance, nullptr);
    SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    return button;
}

HWND AddHeading(HWND parent, const wchar_t* text) {
    HWND heading = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                   0, 0, 1, 1, parent, nullptr, g_instance, nullptr);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(g_headingFont), TRUE);
    return heading;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        AddButton(window, kBuild, L"重新构建");
        AddButton(window, kDeploy, L"部署 ASI");
        AddButton(window, kOpenLog, L"打开日志");
        AddButton(window, kSelectSa, L"选择 SA 目录…");
        AddButton(window, kSelectGtaV, L"选择 GTA V 目录…");
        AddButton(window, kDeployV, L"部署 V + FX");
        AddButton(window, kOpenVLog, L"打开 V 日志");
        AddButton(window, kTeleportV, L"TP 到 V 葛洛夫街");
        AddButton(window,kHotReloadVFx,L"热更新 FX");
        AddButton(window, kCaptureSaAnchor, L"取当前 SA 人物位置");
        AddButton(window, kSaveCalibration, L"保存 SA 出口参数");
        g_saHeading = AddHeading(window, L"GTA San Andreas · 开发副本");
        g_vHeading = AddHeading(window, L"GTA V Enhanced · 故事模式开发副本");
        g_vSafety = CreateWindowExW(0, L"STATIC",
                                   L"仅停用 V 插件不会清理 Script Hook V / ReShade；勿用此目录进入 Online。",
                                   WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window, nullptr, g_instance, nullptr);
        SendMessageW(g_vSafety, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        g_calibrationLabel = CreateWindowExW(0, L"STATIC",
            L"Portal Camera 校准 · SA 出口 X / Y / Z / Yaw°（SA 主相机，WGC 实时预览）",
            WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window, nullptr, g_instance, nullptr);
        SendMessageW(g_calibrationLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        for (int index = 0; index < 4; ++index) {
            g_saPortalEdits[index] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                0, 0, 1, 1, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaPortalX + index)),
                g_instance, nullptr);
            SendMessageW(g_saPortalEdits[index], WM_SETFONT,
                         reinterpret_cast<WPARAM>(g_monoFont), TRUE);
        }
        UpdateCalibrationEdits();
        g_portalCameraMode = CreateWindowExW(0, L"BUTTON", L"启用 V → SA Portal 相机",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPortalCamera)),
            g_instance, nullptr);
        SendMessageW(g_portalCameraMode, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        g_saToVCameraMode = CreateWindowExW(0, L"BUTTON", L"启用 SA → V Portal 相机",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaToVCamera)),
            g_instance, nullptr);
        SendMessageW(g_saToVCameraMode, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        g_showVPortalMode = CreateWindowExW(0, L"BUTTON", L"显示 GTA V 中的 SA 门",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShowVPortal)),
            g_instance, nullptr);
        SendMessageW(g_showVPortalMode, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        SendMessageW(g_showVPortalMode, BM_SETCHECK, BST_CHECKED, 0);
        g_showSaPortalMode = CreateWindowExW(0, L"BUTTON", L"显示 SA 中的 GTA V 门",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShowSaPortal)),
            g_instance, nullptr);
        SendMessageW(g_showSaPortalMode, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        SendMessageW(g_showSaPortalMode, BM_SETCHECK, BST_UNCHECKED, 0);
        g_saStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                         ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                     0, 0, 1, 1, window, nullptr, g_instance, nullptr);
        g_vStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                    0, 0, 1, 1, window, nullptr, g_instance, nullptr);
        SendMessageW(g_saStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        SendMessageW(g_vStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        SetTimer(window, kRefreshTimer, 1000, nullptr);
        SetTimer(window, kFrameTimer, 15, nullptr);
        Refresh();
        return 0;
    case WM_SIZE:
        Layout(window);
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {760, 650};
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
        if (wParam == kFrameTimer) {
            UpdatePortalCamera();
            CaptureGameFrame();
            CaptureVFrame();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kBuild: StartBuild(); break;
        case kDeploy: Deploy(); break;
        case kOpenLog: OpenPath(g_log, L"portal.log 尚未生成。"); break;
        case kSelectSa: SelectSaDirectory(); break;
        case kSelectGtaV: SelectGtaVDirectory(); break;
        case kDeployV: DeployV(); break;
        case kHotReloadVFx: HotReloadVFx(); break;
        case kOpenVLog: OpenPath(g_gtaV / L"PortalV.log", L"PortalV.log 尚未生成。"); break;
        case kTeleportV: {
            // User-confirmed GTA V Grove Street development spawn, separate from F9's portal plane.
            const float groveStreet[3]{development_anchors::gta_v::x,
                                       development_anchors::gta_v::y,
                                       development_anchors::gta_v::z};
            if (!g_vClient.RequestTeleport(
                    groveStreet, development_anchors::gta_v::headingDegrees)) {
                ShowError(L"V 尚未连接", L"请先进入已部署 PortalV.asi 的 GTA V Enhanced 故事模式。");
            }
            Refresh();
            break;
        }
        case kPortalCamera:
            g_portalCameraEnabled =
                SendMessageW(g_portalCameraMode, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (g_portalCameraEnabled && !ReadCalibrationEdits()) {
                g_portalCameraEnabled = false;
                SendMessageW(g_portalCameraMode, BM_SETCHECK, BST_UNCHECKED, 0);
                ShowError(L"SA 出口参数无效", L"请为 X、Y、Z 和 Yaw 输入有限数字。");
            }
            if(g_portalCameraEnabled){
                g_saToVCameraEnabled=false;
                SendMessageW(g_saToVCameraMode,BM_SETCHECK,BST_UNCHECKED,0);
                const float zero[3]{};g_vClient.PublishCamera(false,zero,zero,70.0f);
            }
            UpdatePortalCamera();
            Refresh();
            break;
        case kSaToVCamera:
            g_saToVCameraEnabled=
                SendMessageW(g_saToVCameraMode,BM_GETCHECK,0,0)==BST_CHECKED;
            if(g_saToVCameraEnabled){
                g_portalCameraEnabled=false;
                SendMessageW(g_portalCameraMode,BM_SETCHECK,BST_UNCHECKED,0);
            }
            UpdatePortalCamera();
            Refresh();
            break;
        case kShowVPortal:
            g_showVPortal =
                SendMessageW(g_showVPortalMode, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_vClient.SetPortalVisible(g_showVPortal);
            Refresh();
            break;
        case kShowSaPortal:
            g_showSaPortal =
                SendMessageW(g_showSaPortalMode, BM_GETCHECK, 0, 0) == BST_CHECKED;
            UpdatePortalCamera();
            Refresh();
            break;
        case kCaptureSaAnchor:
            if (!g_portalCameraClient.Read(g_saCameraSnapshot) ||
                !g_saCameraSnapshot.playerValid) {
                ShowError(L"SA 尚未连接", L"请先进入 SA gameplay；建议在指定位置按 F8 写入日志标记。");
                break;
            }
            g_saPortal[0] = g_saCameraSnapshot.playerPosition[0];
            g_saPortal[1] = g_saCameraSnapshot.playerPosition[1];
            g_saPortal[2] = g_saCameraSnapshot.playerPosition[2] +
                portal_visual::kCenterAboveGround;
            UpdateCalibrationEdits();
            break;
        case kSaveCalibration:
            if (!SaveCalibration()) {
                ShowError(L"保存失败", L"请检查四个数字以及 dev 游戏目录的写入权限。");
            }
            break;
        case kSaPortalX:
        case kSaPortalY:
        case kSaPortalZ:
        case kSaPortalYaw:
            if (HIWORD(wParam) == EN_CHANGE && g_portalCameraEnabled && ReadCalibrationEdits())
                UpdatePortalCamera();
            break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, kRefreshTimer);
        KillTimer(window, kFrameTimer);
        g_capture.Stop();
        g_vCapture.Stop();
        if (g_saCameraConnected) {
            const float zero[3]{};
            g_portalCameraClient.Publish(false, g_showSaPortal, zero, zero, zero, 70.0f);
        }
        if(g_vConnected){
            const float zero[3]{};g_vClient.PublishCamera(false,zero,zero,70.0f);
        }
        g_portalCameraClient.Disconnect();
        g_vClient.Disconnect();
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

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t* commandLine, int showCommand) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    g_instance = instance;
    SetProcessDPIAware();
    g_repo = game_paths::FindRepoRoot();
    if (g_repo.empty()) {
        MessageBoxW(nullptr, L"无法定位 GTAPortal 仓库。请保留 PortalDev.exe 在仓库的构建目录中。",
                    L"GTAPortal Dev", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_sourceAsi = g_repo / L"src" / L"PortalSA" / L"bin" / L"GTA-SA" / L"Release" / L"PortalSA.asi";
    g_sourceVAsi = g_repo / L"src" / L"PortalV" / L"bin" / L"x64" / L"Release" / L"PortalV.asi";
    const fs::path savedSa = game_paths::LoadSavedDirectory(L"SaDirectory");
    SetSaDirectory(savedSa.empty() ? g_repo / L"game" / L"dev" / L"GTA San Andreas" : savedSa);
    LoadCalibration();
    g_gtaV = game_paths::LoadSavedDirectory(L"GtaVEnhancedDirectory");
    if (!g_gtaV.empty()) g_gtaVValid = game_paths::ValidateGtaVDirectory(g_gtaV, g_gtaVVersion);
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
    g_headingFont = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Segoe UI");

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

    g_window = CreateWindowExW(0, kWindowClass, L"GTAPortal Dev",
                               WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 900, 760,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        return 1;
    }
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);
    if (commandLine && wcsstr(commandLine, L"--auto-run-sa")) {
        const HINSTANCE launched = ShellExecuteW(g_window, L"open", g_game.c_str(), nullptr,
                                                  g_game.parent_path().c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) <= 32) {
            ShowError(L"自动启动 SA 失败", WinError());
        }
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DeleteObject(g_uiFont);
    DeleteObject(g_monoFont);
    DeleteObject(g_headingFont);
    return static_cast<int>(message.wParam);
}
