#include <d3d9.h>
#include <windows.h>

#include <array>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>

#include <plugin.h>
#include <CCamera.h>
#include <CCutsceneMgr.h>
#include <CLoadingScreen.h>
#include <CMenuManager.h>
#include <CPlayerPed.h>
#include <common.h>
#include <safetyhook.hpp>

namespace {

constexpr DWORD kStateLogIntervalMs = 100;
constexpr DWORD kFrameIntervalMs = 2000;
// GTA SA 1.0 US HOODLUM; plugin-sdk uses this address in RenderWare.cpp.
constexpr uintptr_t kShowRasterAddress = 0x7EE370;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 Copy(const CVector& value) {
    return {value.x, value.y, value.z};
}

struct State {
    DWORD tick = 0;
    bool playerValid = false;
    bool cameraValid = false;
    bool velocityFromVehicle = false;
    Vec3 playerPosition;
    Vec3 playerVelocity;
    float playerHeading = 0.0f;
    Vec3 cameraPosition;
    Vec3 cameraForward;
    Vec3 cameraUp;
    float fovDegrees = 0.0f;
    float nearClip = 0.0f;
    float farClip = 0.0f;
    float viewWindowX = 0.0f;
    float viewWindowY = 0.0f;
    unsigned cameraIndex = 0;
};

State g_state;
std::array<float, 16> g_viewMatrix{};
std::array<float, 16> g_projectionMatrix{};
DWORD g_renderMatrixTick = 0;
DWORD g_lastStateLogTick = 0;
DWORD g_lastFrameTick = 0;
bool g_initialized = false;
bool g_supported = false;
HANDLE g_log = INVALID_HANDLE_VALUE;
wchar_t g_logPath[MAX_PATH]{};
wchar_t g_framePath[MAX_PATH]{};
wchar_t g_tempFramePath[MAX_PATH]{};
safetyhook::InlineHook g_showRasterHook;

bool WriteAll(HANDLE file, const void* data, DWORD size) {
    DWORD written = 0;
    return WriteFile(file, data, size, &written, nullptr) && written == size;
}

void Log(const std::string& text) {
    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteAll(g_log, text.data(), static_cast<DWORD>(text.size()));
    static constexpr char newline[] = "\r\n";
    WriteAll(g_log, newline, 2);
}

bool Initialize() {
    if (g_initialized) {
        return g_supported;
    }
    g_initialized = true;

    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 32) {
        return false;
    }
    wchar_t* separator = wcsrchr(directory, L'\\');
    if (!separator) {
        return false;
    }
    separator[1] = L'\0';
    wcscpy_s(g_logPath, directory);
    wcscat_s(g_logPath, L"portal.log");
    wcscpy_s(g_framePath, directory);
    wcscat_s(g_framePath, L"portal-frame.bmp");
    wcscpy_s(g_tempFramePath, directory);
    wcscat_s(g_tempFramePath, L"portal-frame.tmp");

    g_log = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_supported = plugin::GetGameVersion() == GAME_10US_HOODLUM;
    Log(g_supported ? "PortalSA loaded: GTA SA 1.0 US HOODLUM"
                    : "PortalSA disabled: unsupported GTA SA executable");
    return g_supported;
}

const char* Phase(bool playerValid) {
    if (CLoadingScreen::m_bActive) {
        return "loading";
    }
    if (FrontEndMenuManager.m_bMenuActive) {
        return "menu_or_pause";
    }
    if (CCutsceneMgr::ms_running) {
        return "cutscene";
    }
    return playerValid ? "gameplay" : "no_player";
}

void AppendVec(std::ostringstream& line, const char* name, Vec3 value) {
    line << ' ' << name << "=(" << value.x << ',' << value.y << ',' << value.z << ')';
}

void AppendMatrix(std::ostringstream& line, const char* name, const std::array<float, 16>& matrix) {
    line << ' ' << name << "=[";
    for (size_t index = 0; index < matrix.size(); ++index) {
        if (index) {
            line << ',';
        }
        line << matrix[index];
    }
    line << ']';
}

void OnGameProcess() {
    if (!Initialize()) {
        return;
    }

    const DWORD now = GetTickCount();
    State state;
    state.tick = now;
    if (CPlayerPed* player = FindPlayerPed(0)) {
        state.playerValid = true;
        state.playerPosition = Copy(player->GetPosition());
        if (player->bInVehicle && player->m_pVehicle) {
            state.velocityFromVehicle = true;
            state.playerHeading = player->m_pVehicle->GetHeading();
            state.playerVelocity = Copy(player->m_pVehicle->m_vecMoveSpeed);
        } else {
            state.playerHeading = player->GetHeading();
            state.playerVelocity = Copy(player->m_vecMoveSpeed);
        }
    }

    if (RwCamera* rwCamera = TheCamera.m_pRwCamera) {
        const CMatrix& matrix = TheCamera.m_mCameraMatrix;
        state.cameraValid = true;
        state.cameraPosition = Copy(matrix.pos);
        // CMatrix maps forward to its up field and up to its at field.
        state.cameraForward = Copy(matrix.GetForward());
        state.cameraUp = Copy(matrix.GetUp());
        state.cameraIndex = TheCamera.m_nActiveCam;
        if (state.cameraIndex < 3) {
            state.fovDegrees = TheCamera.m_aCams[state.cameraIndex].m_fFOV;
        }
        state.nearClip = RwCameraGetNearClipPlane(rwCamera);
        state.farClip = RwCameraGetFarClipPlane(rwCamera);
        if (const RwV2d* window = RwCameraGetViewWindow(rwCamera)) {
            state.viewWindowX = window->x;
            state.viewWindowY = window->y;
        }
    }
    g_state = state;

    if (g_lastStateLogTick && now - g_lastStateLogTick < kStateLogIntervalMs) {
        return;
    }
    g_lastStateLogTick = now;

    std::ostringstream line;
    line << std::fixed << std::setprecision(4);
    line << "state tick_ms=" << now << " phase=" << Phase(state.playerValid)
         << " player_valid=" << state.playerValid << " camera_valid=" << state.cameraValid;
    if (state.playerValid) {
        AppendVec(line, "player_position", state.playerPosition);
        line << " player_heading_rad=" << state.playerHeading;
        AppendVec(line, "player_move_speed_per_tick", state.playerVelocity);
        line << " velocity_source=" << (state.velocityFromVehicle ? "vehicle" : "ped");
    }
    if (state.cameraValid) {
        AppendVec(line, "camera_position", state.cameraPosition);
        AppendVec(line, "camera_forward", state.cameraForward);
        AppendVec(line, "camera_up", state.cameraUp);
        line << " camera_index=" << state.cameraIndex << " camera_fov_deg=" << state.fovDegrees
             << " near=" << state.nearClip << " far=" << state.farClip
             << " view_window=(" << state.viewWindowX << ',' << state.viewWindowY << ')';
    }
    if (g_renderMatrixTick && now - g_renderMatrixTick < 1000) {
        line << " render_matrix_age_ms=" << now - g_renderMatrixTick;
        AppendMatrix(line, "render_view_row_major", g_viewMatrix);
        AppendMatrix(line, "render_projection_row_major", g_projectionMatrix);
    }
    Log(line.str());
}

void OnRenderScene() {
    if (!g_supported) {
        return;
    }
    const _D3DMATRIX* view = GetD3DViewTransform();
    const _D3DMATRIX* projection = GetD3DProjTransform();
    if (view && projection) {
        std::memcpy(g_viewMatrix.data(), view, sizeof(_D3DMATRIX));
        std::memcpy(g_projectionMatrix.data(), projection, sizeof(_D3DMATRIX));
        g_renderMatrixTick = GetTickCount();
    }
}

HRESULT SaveBackBuffer(IDirect3DDevice9* device, UINT& width, UINT& height) {
    IDirect3DSurface9* backBuffer = nullptr;
    HRESULT result = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(result)) {
        return result;
    }

    D3DSURFACE_DESC desc{};
    result = backBuffer->GetDesc(&desc);
    if (SUCCEEDED(result) && desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) {
        result = D3DERR_INVALIDCALL;
    }
    IDirect3DSurface9* readback = nullptr;
    IDirect3DSurface9* resolved = nullptr;
    if (SUCCEEDED(result)) {
        result = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                     D3DPOOL_SYSTEMMEM, &readback, nullptr);
    }
    if (SUCCEEDED(result)) {
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
            result = device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                                D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr);
            if (SUCCEEDED(result)) {
                result = device->StretchRect(backBuffer, nullptr, resolved, nullptr, D3DTEXF_NONE);
            }
        }
        if (SUCCEEDED(result)) {
            result = device->GetRenderTargetData(resolved ? resolved : backBuffer, readback);
        }
    }
    if (resolved) {
        resolved->Release();
    }
    backBuffer->Release();

    if (SUCCEEDED(result)) {
        D3DLOCKED_RECT pixels{};
        result = readback->LockRect(&pixels, nullptr, D3DLOCK_READONLY);
        if (SUCCEEDED(result)) {
            BITMAPFILEHEADER fileHeader{};
            BITMAPINFOHEADER infoHeader{};
            const DWORD rowBytes = desc.Width * 4;
            const DWORD imageBytes = rowBytes * desc.Height;
            fileHeader.bfType = 0x4D42;
            fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
            fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;
            infoHeader.biSize = sizeof(infoHeader);
            infoHeader.biWidth = static_cast<LONG>(desc.Width);
            infoHeader.biHeight = -static_cast<LONG>(desc.Height); // Top-down BGRA pixels.
            infoHeader.biPlanes = 1;
            infoHeader.biBitCount = 32;
            infoHeader.biCompression = BI_RGB;
            infoHeader.biSizeImage = imageBytes;

            HANDLE frame = CreateFileW(g_tempFramePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            bool saved = frame != INVALID_HANDLE_VALUE;
            if (saved) {
                saved = WriteAll(frame, &fileHeader, sizeof(fileHeader)) &&
                        WriteAll(frame, &infoHeader, sizeof(infoHeader));
                for (UINT row = 0; saved && row < desc.Height; ++row) {
                    const auto* source = static_cast<const BYTE*>(pixels.pBits) + row * pixels.Pitch;
                    saved = WriteAll(frame, source, rowBytes);
                }
                CloseHandle(frame);
                if (saved) {
                    saved = MoveFileExW(g_tempFramePath, g_framePath,
                                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
                }
            }
            readback->UnlockRect();
            result = saved ? S_OK : E_FAIL;
            if (SUCCEEDED(result)) {
                width = desc.Width;
                height = desc.Height;
            }
        }
    }
    if (readback) {
        readback->Release();
    }
    return result;
}

RwCamera* __cdecl OnShowRaster(RwCamera* camera, void* window, RwUInt32 flags) {
    const DWORD now = GetTickCount();
    if (now - g_lastFrameTick >= kFrameIntervalMs) {
        g_lastFrameTick = now;
        UINT width = 0;
        UINT height = 0;
        auto* device = static_cast<IDirect3DDevice9*>(GetD3DDevice());
        const HRESULT result = device ? SaveBackBuffer(device, width, height) : E_POINTER;
        std::ostringstream line;
        line << "frame tick_ms=" << now << " phase=" << Phase(g_state.playerValid)
             << " state_age_ms=" << (g_state.tick ? now - g_state.tick : 0)
             << " state_available=" << (g_state.tick != 0);
        if (SUCCEEDED(result)) {
            line << " result=ok width=" << width << " height=" << height << " file=portal-frame.bmp";
        } else {
            line << " result=failed hresult=0x" << std::hex << static_cast<unsigned>(result);
        }
        Log(line.str());
    }
    return g_showRasterHook.ccall<RwCamera*>(camera, window, flags);
}

void OnInitRw() {
    if (!Initialize() || g_showRasterHook) {
        return;
    }
    g_showRasterHook = safetyhook::create_inline(reinterpret_cast<void*>(kShowRasterAddress),
                                                 &OnShowRaster);
    Log(g_showRasterHook ? "Render output hook installed at RwCameraShowRaster"
                         : "Render output hook failed at RwCameraShowRaster");
}

struct PortalSA {
    PortalSA() {
        if (plugin::GetGameVersion() != GAME_10US_HOODLUM) {
            return;
        }
        plugin::Events::initRwEvent += OnInitRw;
        plugin::Events::gameProcessEvent += OnGameProcess;
        plugin::Events::renderSceneEvent += OnRenderScene;
    }
} g_portalSA;

} // namespace
