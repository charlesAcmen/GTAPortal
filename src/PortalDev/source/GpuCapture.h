#pragma once

#include <windows.h>
#include <cstdint>
#include <array>
#include <string>

class GpuCapture {
public:
    enum class Publication { None, Sa, V };
    bool Start(HWND gameWindow, HWND previewWindow, Publication publication = Publication::None);
    bool Pump();
    void Stop();
    bool Running() const { return static_cast<bool>(gameWindow_); }
    HWND GameWindow() const { return gameWindow_; }
    const std::wstring& Status() const { return status_; }
    uint64_t Frames() const { return frames_; }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
    uint64_t DroppedFrames() const { return droppedFrames_; }
    double LastAgeMs() const { return lastAgeMs_; }
    double AgeP95Ms() const;
    uint64_t PublishedFrames() const;
    uint64_t OverwrittenFrames() const;
    uint64_t PublishDrops() const;
    std::wstring PublishStatus() const;
    std::wstring SaConsumerStatus() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    HWND gameWindow_ = nullptr;
    std::wstring status_ = L"等待游戏窗口";
    uint64_t frames_ = 0;
    UINT width_ = 0;
    UINT height_ = 0;
    uint64_t droppedFrames_ = 0;
    double lastAgeMs_ = 0;
    std::array<double, 600> ageSamples_{};
    size_t ageCount_ = 0;
    size_t ageNext_ = 0;
};
