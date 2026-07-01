#include "pipeline/adaptive_bitrate_controller.h"
#include "pipeline/blocking_queue.h"

#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono_literals;

    yamanoha::AdaptiveBitrateController controller;
    yamanoha::AdaptiveBitrateController::Config config;
    config.minimum_kbps = 500;
    config.maximum_kbps = 4000;
    controller.Reset(4000, config);

    controller.OnReceiverEstimate(2'000'000);
    const auto now = std::chrono::steady_clock::now();
    const auto reduced = controller.TakeUpdate(4000, now);
    assert(reduced && *reduced == 1700);

    controller.OnReceiverEstimate(10'000'000);
    assert(!controller.TakeUpdate(1700, now + 500ms));
    const auto increased = controller.TakeUpdate(1700, now + 1100ms);
    assert(increased && *increased == 4000);

    yamanoha::BlockingQueue<int> queue;
    assert(queue.Push(10));
    int value = 0;
    assert(queue.WaitPopFor(value, 1ms));
    assert(value == 10);

    queue.Stop();
    assert(!queue.Push(20));
    queue.Reset();
    assert(queue.Push(30));
    assert(queue.WaitPopFor(value, 1ms));
    assert(value == 30);
    return 0;
}
