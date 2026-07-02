#pragma once

#include "capture/display_manager.h"
#include "webrtc/webrtc_streamer.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace yamanoha {

class FramePipeline;

class DisplaySessionManager {
public:
    DisplaySessionManager() = default;
    ~DisplaySessionManager();

    DisplaySessionManager(const DisplaySessionManager&) = delete;
    DisplaySessionManager& operator=(const DisplaySessionManager&) = delete;
    DisplaySessionManager(DisplaySessionManager&&) = delete;
    DisplaySessionManager& operator=(DisplaySessionManager&&) = delete;

    bool Start(const Display& display, int fps, int bitrate_kbps);
    bool SwitchDisplay(const Display& display, int fps, int bitrate_kbps);
    void Stop();

    [[nodiscard]] WebrtcStreamer* GetStreamer() const {
        return streamer_.get();
    }

    [[nodiscard]] FramePipeline* GetPipeline() const {
        return pipeline_.get();
    }

    [[nodiscard]] uint32_t GetWidth() const;
    [[nodiscard]] uint32_t GetHeight() const;
    [[nodiscard]] bool IsActive() const { return static_cast<bool>(streamer_); }

private:
    void SetupStreamerCallbacks();
    bool CreatePipeline(const Display& display);

    std::unique_ptr<WebrtcStreamer> streamer_;
    std::unique_ptr<FramePipeline> pipeline_;
    std::atomic<FramePipeline*> pipeline_for_callbacks_{nullptr};
    int fps_ = 0;
    int bitrate_kbps_ = 0;
};

} // namespace yamanoha
