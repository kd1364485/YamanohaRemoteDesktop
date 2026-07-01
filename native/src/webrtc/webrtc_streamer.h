#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class WebrtcStreamer {
public:
    using LocalDescriptionCallback =
        std::function<void(const std::string& sdp, const std::string& type)>;
    using LocalCandidateCallback =
        std::function<void(const std::string& candidate, const std::string& mid)>;
    using MessageCallback = std::function<void(const std::string& message)>;
    using StateChangeCallback = std::function<void(bool connected)>;
    using KeyFrameRequestCallback = std::function<void()>;
    using BitrateRecommendationCallback =
        std::function<void(uint32_t bitrate_bps)>;

    WebrtcStreamer();
    ~WebrtcStreamer();

    WebrtcStreamer(const WebrtcStreamer&) = delete;
    WebrtcStreamer& operator=(const WebrtcStreamer&) = delete;

    bool Initialize();
    void Shutdown();

    void SetLocalDescriptionCallback(LocalDescriptionCallback callback);
    void SetLocalCandidateCallback(LocalCandidateCallback callback);
    void SetMessageCallback(MessageCallback callback);
    void SetStateChangeCallback(StateChangeCallback callback);
    void SetKeyFrameRequestCallback(KeyFrameRequestCallback callback);
    void SetBitrateRecommendationCallback(
        BitrateRecommendationCallback callback);

    bool ProcessSignalingMessage(
        const std::string& message_type,
        const std::string& sdp_or_candidate,
        const std::string& mid = "");

    bool PushVideoFrame(
        const uint8_t* data,
        size_t size,
        uint64_t timestamp_us);
    bool SendMessageOverDataChannel(const std::string& message);

    [[nodiscard]] bool IsConnected() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
