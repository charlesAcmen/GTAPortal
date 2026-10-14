#include <cmath>
#include <iostream>

#include "../PortalSA/source/CameraProjection.h"

namespace {

bool Near(float actual, float expected, float tolerance = 0.001f) {
    if (std::abs(actual - expected) <= tolerance) return true;
    std::cerr << "expected " << expected << ", got " << actual << '\n';
    return false;
}

} // namespace

int main() {
    using namespace portal_sa::camera_projection;

    if (!Near(kRenderAspect, 16.0f / 9.0f)) return 1;

    for (const float vertical : {5.0f, 30.0f, 50.0f, 70.0f, 100.0f, 140.0f}) {
        const float horizontal = VerticalToHorizontalFovDegrees(vertical);
        if (!std::isfinite(horizontal) || horizontal <= vertical || horizontal >= 180.0f) return 2;
        if (!Near(HorizontalToVerticalFovDegrees(horizontal), vertical)) return 3;
    }

    if (!Near(HorizontalToVerticalFovDegrees(VerticalToHorizontalFovDegrees(-10.0f)), 5.0f)) return 4;
    if (!Near(HorizontalToVerticalFovDegrees(VerticalToHorizontalFovDegrees(200.0f)), 140.0f)) return 5;

    std::cout << "Camera projection smoke test passed (fixed 960x540 / 16:9 mapping).\n";
    return 0;
}
