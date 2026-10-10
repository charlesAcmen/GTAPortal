#pragma once

#include <string>

namespace portal_sa::camera_override {

void OnGameProcess(void (*log)(const std::string&));
void Shutdown();

} // namespace portal_sa::camera_override
