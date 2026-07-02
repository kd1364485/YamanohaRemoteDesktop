#include "pipeline/display_session_manager.h"
#include "pipeline/frame_pipeline.h"

#include <cstdio>

namespace yamanoha {

DisplaySessionManager::~DisplaySessionManager() {
    Stop();
}

bool DisplaySessionManager::Start(
    const Display& display,
    int fps,
    int bitrate_kbps) {
    if (streamer_) {
        Stop();
    }

    fps_ = fps;
    bitrate_kbps_ = bitrate_kbps;

    streamer_ = std::make_unique<WebrtcStreamer>();

    if (!CreatePipeline(display)) {
        streamer_.reset();
        return false;
    }

    SetupStreamerCallbacks();

    pipeline_for_callbacks_.store(
        pipeline_.get(), std::memory_order_release);

    std::printf(
        "[DisplaySessionManager] prepared: %u x %u, %d FPS, %d kbps\n",
        GetWidth(), GetHeight(), fps_, bitrate_kbps_);
    return true;
}

bool DisplaySessionManager::SwitchDisplay(
    const Display& display,
    int fps,
    int bitrate_kbps) {
    if (!streamer_) {
        std::printf(
            "[DisplaySessionManager] cannot switch - no active session\n");
        return false;
    }

    fps_ = fps;
    bitrate_kbps_ = bitrate_kbps;

    pipeline_for_callbacks_.store(nullptr, std::memory_order_release);

    if (pipeline_) {
        pipeline_->Stop();
        pipeline_.reset();
    }

    if (!CreatePipeline(display)) {
        std::printf(
            "[DisplaySessionManager] failed to create pipeline for switch\n");
        return false;
    }

    pipeline_for_callbacks_.store(
        pipeline_.get(), std::memory_order_release);

    pipeline_->StartThreads();

    std::printf(
        "[DisplaySessionManager] display switched: %u x %u, %d FPS\n",
        GetWidth(), GetHeight(), fps_);
    return true;
}

void DisplaySessionManager::Stop() {
    pipeline_for_callbacks_.store(nullptr, std::memory_order_release);

    if (pipeline_) {
        pipeline_->Stop();
        pipeline_.reset();
    }

    if (streamer_) {
        streamer_->Shutdown();
        streamer_.reset();
    }

    fps_ = 0;
    bitrate_kbps_ = 0;
}

void DisplaySessionManager::SetupStreamerCallbacks() {
    streamer_->SetKeyFrameRequestCallback([this] {
        FramePipeline* p =
            pipeline_for_callbacks_.load(std::memory_order_acquire);
        if (p) p->RequestKeyFrame();
    });
    streamer_->SetBitrateRecommendationCallback(
        [this](uint32_t bitrate_bps) {
            FramePipeline* p =
                pipeline_for_callbacks_.load(std::memory_order_acquire);
            if (p) p->OnBitrateEstimate(bitrate_bps);
        });
}

bool DisplaySessionManager::CreatePipeline(const Display& display) {
    pipeline_ = std::make_unique<FramePipeline>();
    if (!pipeline_->Start(
            display, fps_, bitrate_kbps_, streamer_.get())) {
        std::printf(
            "[DisplaySessionManager] FramePipeline start failed\n");
        pipeline_.reset();
        return false;
    }
    return true;
}

uint32_t DisplaySessionManager::GetWidth() const {
    return pipeline_ ? pipeline_->GetWidth() : 0;
}

uint32_t DisplaySessionManager::GetHeight() const {
    return pipeline_ ? pipeline_->GetHeight() : 0;
}

} // namespace yamanoha
