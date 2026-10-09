#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

class GpuCapture {
public:
    bool Start(HWND gameWindow, HWND previewWindow);
    bool Pump();
    void Stop();
    bool Running() const { return static_cast<bool>(gameWindow_); }
    HWND GameWindow() const { return gameWindow_; }
    const std::wstring& Status() const { return status_; }
    uint64_t Frames() const { return frames_; }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    HWND gameWindow_ = nullptr;
    std::wstring status_ = L"等待 dev 游戏窗口";
    uint64_t frames_ = 0;
    UINT width_ = 0;
    UINT height_ = 0;
};
