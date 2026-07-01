#include "pipeline/adaptive_bitrate_controller.h"

#include <algorithm>

namespace yamanoha {

void AdaptiveBitrateController::Reset(uint32_t initial_kbps, Config config) {
    config.maximum_kbps = (std::max)(config.maximum_kbps, 1u);
    config.minimum_kbps = (std::clamp)(
        config.minimum_kbps, 1u, config.maximum_kbps);
    config_ = config;
    requested_kbps_.store(
        (std::clamp)(initial_kbps, config_.minimum_kbps, config_.maximum_kbps),
        std::memory_order_release);
    last_update_ = std::chrono::steady_clock::now() - config_.update_interval;
}

void AdaptiveBitrateController::OnReceiverEstimate(uint32_t bitrate_bps) {
    if (bitrate_bps == 0) {
        return;
    }

    const uint64_t with_headroom =
        static_cast<uint64_t>(bitrate_bps) * config_.receiver_headroom_percent / 100;
    const uint32_t target_kbps = (std::clamp)(
        static_cast<uint32_t>(with_headroom / 1000),
        config_.minimum_kbps,
        config_.maximum_kbps);
    requested_kbps_.store(target_kbps, std::memory_order_release);
}

std::optional<uint32_t> AdaptiveBitrateController::TakeUpdate(
    uint32_t current_kbps,
    std::chrono::steady_clock::time_point now) {
    const uint32_t requested = requested_kbps_.load(std::memory_order_acquire);
    if (requested == 0 || requested == current_kbps ||
        now - last_update_ < config_.update_interval) {
        return std::nullopt;
    }

    const uint32_t difference = requested > current_kbps
        ? requested - current_kbps
        : current_kbps - requested;
    const uint32_t threshold = (std::max)(
        config_.minimum_change_kbps,
        current_kbps * config_.change_threshold_percent / 100);
    if (difference < threshold) {
        return std::nullopt;
    }

    last_update_ = now;
    return requested;
}

} // namespace yamanoha
