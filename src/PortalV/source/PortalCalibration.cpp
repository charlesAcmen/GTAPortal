#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "PortalCalibration.h"

#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <mutex>

#include "../../PortalVisualConfig.h"

namespace portal_v::calibration {
namespace {

std::mutex g_mutex;
Snapshot g_locked;
Snapshot g_draft;
wchar_t g_path[MAX_PATH]{};
std::atomic<uint64_t> g_lastOverlayDrawTick{};
bool g_lastSaveFailed{};
float g_groundZ{portal_visual::gta_v::groundZ};

void Normalize(Snapshot& value) {
    value.width = std::clamp(value.width, 0.25f, 20.0f);
    value.height = std::clamp(value.height, 0.25f, 20.0f);
    value.cornerRadius = std::clamp(value.cornerRadius, 0.0f, 0.5f);
    value.edgeFeather = std::clamp(value.edgeFeather, 0.0f, 0.05f);
    value.yawDegrees = std::remainder(value.yawDegrees, 360.0f);
    value.crop[0] = std::clamp(value.crop[0], 0.0f, 0.95f);
    value.crop[1] = std::clamp(value.crop[1], 0.0f, 0.95f);
    value.crop[2] = std::clamp(value.crop[2], value.crop[0] + 0.01f, 1.0f);
    value.crop[3] = std::clamp(value.crop[3], value.crop[1] + 0.01f, 1.0f);
}

bool Finite(const Snapshot& value) {
    if (!std::isfinite(value.yawDegrees) || !std::isfinite(value.width) ||
        !std::isfinite(value.height) || !std::isfinite(value.cornerRadius) ||
        !std::isfinite(value.edgeFeather)) return false;
    for (float component : value.center) if (!std::isfinite(component)) return false;
    for (float component : value.crop) if (!std::isfinite(component)) return false;
    return true;
}

void ApplyCircleStyle(Snapshot& value) {
    value.width = portal_visual::kWidth;
    value.height = portal_visual::kHeight;
    value.shape = Shape::Ellipse;
    value.crop[0] = portal_visual::kCropLeft;
    value.crop[1] = portal_visual::kCropTop;
    value.crop[2] = portal_visual::kCropRight;
    value.crop[3] = portal_visual::kCropBottom;
}

Snapshot DefaultSnapshot() {
    Snapshot value;
    ApplyCircleStyle(value);
    value.center[0] = portal_visual::gta_v::groundX;
    value.center[1] = portal_visual::gta_v::groundY;
    value.center[2] = portal_visual::gta_v::groundZ + portal_visual::kCenterAboveGround;
    value.yawDegrees = portal_visual::gta_v::yawDegrees;
    value.valid = true;
    return value;
}

bool SaveLocked() {
    if (!g_path[0] || !g_locked.valid) return false;
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_path, L"w") != 0 || !file) return false;
    const int written = std::fprintf(file,
        "5 %.6f %.6f %.6f %.6f %.6f %.6f %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
        g_locked.center[0], g_locked.center[1], g_locked.center[2],
        g_locked.yawDegrees, g_locked.width, g_locked.height,
        static_cast<int>(g_locked.shape), g_locked.cornerRadius, g_locked.edgeFeather,
        g_locked.crop[0], g_locked.crop[1], g_locked.crop[2], g_locked.crop[3],g_groundZ);
    const bool closed = std::fclose(file) == 0;
    return written > 0 && closed;
}

void LoadLocked() {
    FILE* file = nullptr;
    if (!g_path[0] || _wfopen_s(&file, g_path, L"r") != 0 || !file) {
        g_locked = DefaultSnapshot();
        g_draft = g_locked;
        g_groundZ=portal_visual::gta_v::groundZ;
        SaveLocked();
        return;
    }
    Snapshot value;
    int version = 0;
    bool legacyFour = false;
    int shape = static_cast<int>(Shape::RoundedRectangle);
    const int header = ::fscanf_s(file, "%d", &version);
    std::rewind(file);
    int read = 0;
    if(header==1&&version==5){
        read=::fscanf_s(file,
            "%d %f %f %f %f %f %f %d %f %f %f %f %f %f %f",
            &version,&value.center[0],&value.center[1],&value.center[2],
            &value.yawDegrees,&value.width,&value.height,&shape,
            &value.cornerRadius,&value.edgeFeather,&value.crop[0],&value.crop[1],
            &value.crop[2],&value.crop[3],&g_groundZ);
        if(read==15){value.shape=static_cast<Shape>(std::clamp(shape,0,2));value.valid=true;}
    } else if (header == 1 && (version == 4 || version == 3)) {
        read = ::fscanf_s(file,
            "%d %f %f %f %f %f %f %d %f %f %f %f %f %f",
            &version, &value.center[0], &value.center[1], &value.center[2],
            &value.yawDegrees, &value.width, &value.height, &shape,
            &value.cornerRadius, &value.edgeFeather, &value.crop[0], &value.crop[1],
            &value.crop[2], &value.crop[3]);
        if (read == 14) {
            value.shape = static_cast<Shape>(std::clamp(shape, 0, 2));
            value.valid = true;
        }
    } else if (header == 1 && version == 2) {
        read = ::fscanf_s(file,
            "%d %f %f %f %f %f %f %d %f %f %f %f %f",
            &version, &value.center[0], &value.center[1], &value.center[2],
            &value.yawDegrees, &value.width, &value.height, &shape,
            &value.cornerRadius, &value.crop[0], &value.crop[1],
            &value.crop[2], &value.crop[3]);
        if (read == 13) {
            value.shape = static_cast<Shape>(std::clamp(shape, 0, 2));
            value.valid = true;
        }
    }
    if (!value.valid) {
        std::rewind(file);
        if (::fscanf_s(file, "%f %f %f %f", &value.center[0], &value.center[1],
                        &value.center[2], &value.yawDegrees) == 4) {
            value.valid = true;
            legacyFour = true;
        }
    }
    std::fclose(file);
    if (value.valid && Finite(value) && std::isfinite(g_groundZ)) {
        const bool migrate=version!=5;
        if(version<=3){
            const float oldHalfHeight = legacyFour ? 1.10f :
                (value.height > 0.0f ? value.height * 0.5f : 1.10f);
            const float groundZ = value.center[2] - oldHalfHeight;
            ApplyCircleStyle(value);
            value.center[2] = groundZ + portal_visual::kCenterAboveGround;
        }
        Normalize(value);
        g_locked = value;
        g_draft = value;
        if(version!=5)g_groundZ=value.center[2]-portal_visual::kCenterAboveGround;
        if (migrate) SaveLocked();
    } else {
        g_locked = DefaultSnapshot();
        g_draft = g_locked;
        g_groundZ=portal_visual::gta_v::groundZ;
        SaveLocked();
    }
}

} // namespace

void Initialize(HMODULE module) {
    std::lock_guard lock(g_mutex);
    if (g_path[0]) return;
    if (!GetModuleFileNameW(module, g_path, MAX_PATH)) return;
    wchar_t* slash = std::wcsrchr(g_path, L'\\');
    if (!slash) { g_path[0] = L'\0'; return; }
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - g_path),
             L"PortalVPlane.txt");
    LoadLocked();
}

Snapshot GetSnapshot() {
    std::lock_guard lock(g_mutex);
    return g_draft.editing ? g_draft : g_locked;
}

void BeginEditing() {
    std::lock_guard lock(g_mutex);
    g_draft = g_locked.valid ? g_locked : DefaultSnapshot();
    g_draft.editing = true;
    g_lastSaveFailed = false;
}

void BeginAtPortalBase(const float (&position)[3], float yawDegrees) {
    std::lock_guard lock(g_mutex);
    g_draft = g_locked.valid ? g_locked : Snapshot{};
    const float savedCenterAboveGround=g_locked.valid?
        g_locked.center[2]-g_groundZ:portal_visual::kCenterAboveGround;
    g_draft.shape=Shape::Ellipse;
    const float savedRadius=std::clamp((g_draft.width+g_draft.height)*0.25f,0.25f,20.0f);
    g_draft.width=g_draft.height=savedRadius*2.0f;
    g_draft.center[0] = position[0];
    g_draft.center[1] = position[1];
    // TP is the portal's ground contact, not a point three metres in front.
    g_draft.center[2] = position[2] + savedCenterAboveGround;
    g_groundZ=position[2];
    g_draft.yawDegrees = yawDegrees;
    g_draft.valid = Finite(g_draft);
    g_draft.editing = true;
    g_lastSaveFailed = false;
    Normalize(g_draft);
    // F9 is already an explicit placement action. Persist that exact placement
    // immediately; subsequent overlay edits still require Lock In.
    const Snapshot previousLocked = g_locked;
    g_locked = g_draft;
    g_locked.editing = false;
    g_lastSaveFailed = !SaveLocked();
    if (g_lastSaveFailed) g_locked = previousLocked;
}

void AdjustCircle(float radiusDelta,float centerHeightDelta){
    std::lock_guard lock(g_mutex);
    if(!g_draft.valid)return;
    float radius=(g_draft.width+g_draft.height)*0.25f;
    radius=std::clamp(radius+radiusDelta,0.25f,20.0f);
    g_draft.width=g_draft.height=radius*2.0f;
    g_draft.center[2]=std::clamp(g_draft.center[2]+centerHeightDelta,
                                 g_groundZ-20.0f,g_groundZ+20.0f);
    g_draft.shape=Shape::Ellipse;g_draft.editing=true;
    Normalize(g_draft);
    const Snapshot previous=g_locked;g_locked=g_draft;g_locked.editing=false;
    g_lastSaveFailed=!SaveLocked();if(g_lastSaveFailed)g_locked=previous;
}

void DrawOverlay(reshade::api::effect_runtime*) {
    g_lastOverlayDrawTick.store(GetTickCount64(), std::memory_order_relaxed);
    std::lock_guard lock(g_mutex);
    ImGui::TextWrapped("F9 places the circle at the player. Every adjustment below is applied live and saved immediately.");
    if (!g_draft.editing) {
        ImGui::TextUnformatted(g_locked.valid ? "Locked calibration loaded." : "Press F9 in Story Mode to start calibration.");
        return;
    }
    bool changed=false;
    float radius=(g_draft.width+g_draft.height)*0.25f;
    if(ImGui::DragFloat("Radius",&radius,0.02f,0.25f,20.0f,"%.2f m")){
        g_draft.width=g_draft.height=radius*2.0f;
        g_draft.shape=Shape::Ellipse;
        changed=true;
    }
    float centerAboveGround=g_draft.center[2]-g_groundZ;
    if(ImGui::DragFloat("Center above ground",&centerAboveGround,0.01f,-20.0f,20.0f,"%.2f m")){
        g_draft.center[2]=g_groundZ+centerAboveGround;
        changed=true;
    }
    changed|=ImGui::SliderFloat("Edge feather", &g_draft.edgeFeather, 0.0f, 0.05f, "%.3f");
    ImGui::SeparatorText("Source crop (normalized)");
    changed|=ImGui::DragFloat4("Left / Top / Right / Bottom", g_draft.crop, 0.0025f, 0.0f, 1.0f, "%.3f");
    Normalize(g_draft);
    if(changed){
        const Snapshot previous=g_locked;
        g_locked=g_draft;g_locked.editing=false;
        g_lastSaveFailed=!SaveLocked();
        if(g_lastSaveFailed)g_locked=previous;
    }
    if (ImGui::Button("Lock In")) {
        const Snapshot previous = g_locked;
        g_locked = g_draft;
        g_locked.editing = false;
        g_lastSaveFailed = !SaveLocked();
        if (g_lastSaveFailed) {
            g_locked = previous;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        g_draft = g_locked;
        g_draft.editing = false;
        g_lastSaveFailed = false;
    }
    if (g_lastSaveFailed)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "Save failed: PortalVPlane.txt is not writable.");
}

} // namespace portal_v::calibration
