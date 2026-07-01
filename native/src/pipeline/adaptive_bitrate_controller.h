#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace yamanoha {

class AdaptiveBitrateController {
public:
    struct Config {
        uint32_t minimum_kbps = 500;
        uint32_t maximum_kbps = 4000;
        uint32_t minimum_change_kbps = 100;
        uint32_t change_threshold_percent = 10;
        uint32_t receiver_headroom_percent = 85;
        std::chrono::milliseconds update_interval{1000};
    };

    void Reset(uint32_t initial_kbps, Config config);
    void OnReceiverEstimate(uint32_t bitrate_bps);

    std::optional<uint32_t> TakeUpdate(
        uint32_t current_kbps,
        std::chrono::steady_clock::time_point now);

private:
    Config config_{};
    std::atomic<uint32_t> requested_kbps_{0};
    std::chrono::steady_clock::time_point last_update_{};
};

} // namespace yamanoha
