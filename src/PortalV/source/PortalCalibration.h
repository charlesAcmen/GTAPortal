#pragma once

#include <windows.h>

namespace reshade::api { struct effect_runtime; }

namespace portal_v::calibration {

enum class Shape : int { Rectangle = 0, RoundedRectangle = 1, Ellipse = 2 };

struct Snapshot {
    float center[3]{};
    float yawDegrees{};
    float width{8.0f};
    float height{8.0f};
    float cornerRadius{0.16f};
    float edgeFeather{0.008f};
    float crop[4]{0.21875f, 0.0f, 0.78125f, 1.0f};
    Shape shape{Shape::Ellipse};
    bool valid{};
    bool editing{};
};

void Initialize(HMODULE module);
Snapshot GetSnapshot();
void BeginEditing();
void BeginAtPortalBase(const float (&position)[3], float yawDegrees);
void AdjustCircle(float radiusDelta, float centerHeightDelta);
void DrawOverlay(reshade::api::effect_runtime* runtime);

} // namespace portal_v::calibration
