#define WIN32_LEAN_AND_MEAN
#include "PortalOverlay.h"

#include <d3d9.h>
#include <windows.h>

#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>

#include "VConsumer.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace portal_sa::portal_overlay { namespace {
bool g_initialized{};
bool g_visible{};
HWND g_window{};
WNDPROC g_originalWndProc{};

LRESULT CALLBACK OverlayWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_visible && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return TRUE;
    if (g_visible && ImGui::GetCurrentContext()) {
        const ImGuiIO& io=ImGui::GetIO();
        if ((io.WantCaptureMouse&&(message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST))||
            (io.WantCaptureKeyboard&&(message>=WM_KEYFIRST&&message<=WM_KEYLAST))) return TRUE;
    }
    return CallWindowProcW(g_originalWndProc,window,message,wParam,lParam);
}

bool Initialize(IDirect3DDevice9* device) {
    if(g_initialized)return true;
    D3DDEVICE_CREATION_PARAMETERS parameters{};
    if(FAILED(device->GetCreationParameters(&parameters))||!parameters.hFocusWindow)return false;
    g_window=parameters.hFocusWindow;
    IMGUI_CHECKVERSION();ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if(!ImGui_ImplWin32_Init(g_window)||!ImGui_ImplDX9_Init(device)){
        ImGui::DestroyContext();return false;
    }
    g_originalWndProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&OverlayWndProc)));
    if(!g_originalWndProc){ImGui_ImplDX9_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();return false;}
    g_initialized=true;return true;
}
} // namespace

void Render(IDirect3DDevice9* device) {
    if(!device||!Initialize(device))return;
    if(v_consumer::ConsumeOverlayOpenRequest())g_visible=true;
    if(!g_visible)return;
    ImGui_ImplDX9_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor=true;
    bool open=true;
    ImGui::SetNextWindowSize(ImVec2(360.0f,0.0f),ImGuiCond_FirstUseEver);
    if(ImGui::Begin("GTAPortal SA Circle",&open,ImGuiWindowFlags_AlwaysAutoResize)){
        auto tuning=v_consumer::GetVisualTuning();
        ImGui::TextUnformatted(tuning.followingPlayer?
            "F9 follow: ON (press F9 again to lock)":"F9 follow: OFF");
        bool changed=false;
        changed|=ImGui::DragFloat("Radius",&tuning.radius,0.02f,0.25f,20.0f,"%.2f m");
        changed|=ImGui::DragFloat("Center above ground",&tuning.centerAboveGround,
                                  0.01f,-20.0f,20.0f,"%.2f m");
        if(changed)v_consumer::SetVisualTuning(tuning.radius,tuning.centerAboveGround);
        if(ImGui::Button("Save circle tuning"))v_consumer::SaveVisualTuning();
        ImGui::SameLine();ImGui::TextUnformatted("F9 places/follows; no XY/Yaw controls here.");
    }
    ImGui::End();g_visible=open;
    ImGui::Render();ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void BeforeReset(){if(g_initialized)ImGui_ImplDX9_InvalidateDeviceObjects();}
void AfterReset(){if(g_initialized)ImGui_ImplDX9_CreateDeviceObjects();}
void Shutdown(){
    if(!g_initialized)return;
    if(g_window&&g_originalWndProc)SetWindowLongPtrW(g_window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(g_originalWndProc));
    ImGui_ImplDX9_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();
    g_initialized=false;g_visible=false;g_window=nullptr;g_originalWndProc=nullptr;
}
} // namespace portal_sa::portal_overlay
