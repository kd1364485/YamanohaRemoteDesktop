#include "webrtc_streamer.h"

#include <rtc/h264rtppacketizer.hpp>
#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <utility>

namespace {

constexpr rtc::SSRC kVideoSsrc = 1001;
constexpr uint8_t kH264PayloadType = 96;
constexpr size_t kRtpMtu = 1200;
constexpr size_t kNackHistoryPackets = 2048;
constexpr char kVideoCname[] = "video-stream";
constexpr char kControlChannelLabel[] = "control";

} // namespace

class WebrtcStreamer::Impl {
public:
    ~Impl() {
        Shutdown();
    }

    bool Initialize() {
        Shutdown();
        first_video_timestamp_us_ = 0;

        try {
            rtc::InitLogger(rtc::LogLevel::Info);

            rtc::Configuration configuration;
            configuration.iceServers.emplace_back("stun:stun.l.google.com:19302");
            configuration.iceServers.emplace_back("stun:stun1.l.google.com:19302");
            configuration.iceServers.emplace_back("stun:stun2.l.google.com:19302");

            auto peer_connection =
                std::make_shared<rtc::PeerConnection>(configuration);
            {
                std::lock_guard<std::mutex> lock(transport_mutex_);
                peer_connection_ = peer_connection;
            }

            ConfigurePeerConnection(peer_connection);
            CreateVideoTrack(peer_connection);
            CreateControlChannel(peer_connection);
            return true;
        } catch (const std::exception& error) {
            std::printf(
                "[WebrtcStreamer] initialization failed: %s\n",
                error.what());
            std::fflush(stdout);
            Shutdown();
            return false;
        }
    }

    void Shutdown() {
        connected_.store(false, std::memory_order_release);

        std::shared_ptr<rtc::DataChannel> data_channel;
        std::shared_ptr<rtc::PeerConnection> peer_connection;
        {
            std::lock_guard<std::mutex> lock(transport_mutex_);
            data_channel = std::move(data_channel_);
            video_track_.reset();
            peer_connection = std::move(peer_connection_);
        }

        if (data_channel) {
            data_channel->close();
        }
        if (peer_connection) {
            peer_connection->close();
        }
    }

    bool ProcessSignalingMessage(
        const std::string& message_type,
        const std::string& sdp_or_candidate,
        const std::string& mid) {
        auto peer_connection = GetPeerConnection();
        if (!peer_connection) {
            return false;
        }

        try {
            if (message_type == "offer" || message_type == "answer") {
                rtc::Description description(sdp_or_candidate, message_type);
                peer_connection->setRemoteDescription(description);
                if (message_type == "offer") {
                    peer_connection->setLocalDescription();
                }
                return true;
            }

            if (message_type == "candidate") {
                peer_connection->addRemoteCandidate(
                    rtc::Candidate(sdp_or_candidate, mid));
                return true;
            }
        } catch (const std::exception& error) {
            std::printf(
                "[WebrtcStreamer] signaling failed (%s): %s\n",
                message_type.c_str(),
                error.what());
            std::fflush(stdout);
        }
        return false;
    }

    bool PushVideoFrame(
        const uint8_t* data,
        size_t size,
        uint64_t timestamp_us) {
        if (!data || size == 0 ||
            !connected_.load(std::memory_order_acquire)) {
            return false;
        }

        std::shared_ptr<rtc::Track> video_track;
        {
            std::lock_guard<std::mutex> lock(transport_mutex_);
            video_track = video_track_;
        }
        if (!video_track || !video_track->isOpen()) {
            return false;
        }

        try {
            if (first_video_timestamp_us_ == 0 ||
                timestamp_us < first_video_timestamp_us_) {
                first_video_timestamp_us_ = timestamp_us;
            }
            const uint64_t relative_timestamp_us =
                timestamp_us - first_video_timestamp_us_;

            video_track->sendFrame(
                reinterpret_cast<const std::byte*>(data),
                size,
                std::chrono::duration<double, std::micro>(
                    relative_timestamp_us));
            return true;
        } catch (const std::exception& error) {
            std::printf(
                "[WebrtcStreamer] video send failed: %s\n",
                error.what());
            std::fflush(stdout);
            return false;
        }
    }

    bool SendMessageOverDataChannel(const std::string& message) {
        std::shared_ptr<rtc::DataChannel> data_channel;
        {
            std::lock_guard<std::mutex> lock(transport_mutex_);
            data_channel = data_channel_;
        }
        if (!data_channel || !data_channel->isOpen()) {
            return false;
        }

        try {
            return data_channel->send(message);
        } catch (const std::exception& error) {
            std::printf(
                "[WebrtcStreamer] data channel send failed: %s\n",
                error.what());
            std::fflush(stdout);
            return false;
        }
    }

    bool IsConnected() const {
        return connected_.load(std::memory_order_acquire);
    }

    void SetLocalDescriptionCallback(LocalDescriptionCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        local_description_callback_ = std::move(callback);
    }

    void SetLocalCandidateCallback(LocalCandidateCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        local_candidate_callback_ = std::move(callback);
    }

    void SetMessageCallback(MessageCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        message_callback_ = std::move(callback);
    }

    void SetStateChangeCallback(StateChangeCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        state_change_callback_ = std::move(callback);
    }

    void SetKeyFrameRequestCallback(KeyFrameRequestCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        keyframe_request_callback_ = std::move(callback);
    }

    void SetBitrateRecommendationCallback(
        BitrateRecommendationCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        bitrate_recommendation_callback_ = std::move(callback);
    }

private:
    void ConfigurePeerConnection(
        const std::shared_ptr<rtc::PeerConnection>& peer_connection) {
        peer_connection->onStateChange([this](rtc::PeerConnection::State state) {
            const bool connected =
                state == rtc::PeerConnection::State::Connected;
            connected_.store(connected, std::memory_order_release);

            StateChangeCallback callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback = state_change_callback_;
            }
            if (callback) {
                callback(connected);
            }
        });

        peer_connection->onGatheringStateChange(
            [this](rtc::PeerConnection::GatheringState state) {
                if (state != rtc::PeerConnection::GatheringState::Complete) {
                    return;
                }

                auto peer_connection = GetPeerConnection();
                if (!peer_connection) {
                    return;
                }
                auto description = peer_connection->localDescription();
                if (!description) {
                    return;
                }

                LocalDescriptionCallback callback;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    callback = local_description_callback_;
                }
                if (callback) {
                    callback(
                        std::string(description.value()),
                        description->typeString());
                }
            });

        peer_connection->onLocalCandidate([this](rtc::Candidate candidate) {
            LocalCandidateCallback callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback = local_candidate_callback_;
            }
            if (callback) {
                callback(candidate.candidate(), candidate.mid());
            }
        });

        peer_connection->onDataChannel(
            [this](std::shared_ptr<rtc::DataChannel> data_channel) {
                if (data_channel->label() == kControlChannelLabel) {
                    ConfigureControlChannel(data_channel);
                    std::lock_guard<std::mutex> lock(transport_mutex_);
                    data_channel_ = std::move(data_channel);
                }
            });
    }

    void CreateVideoTrack(
        const std::shared_ptr<rtc::PeerConnection>& peer_connection) {
        rtc::Description::Video description(
            "video", rtc::Description::Direction::SendOnly);
        description.addH264Codec(kH264PayloadType);
        description.addSSRC(kVideoSsrc, kVideoCname);

        auto video_track = peer_connection->addTrack(description);
        auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
            kVideoSsrc,
            kVideoCname,
            kH264PayloadType,
            rtc::H264RtpPacketizer::defaultClockRate);
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence,
            rtp_config,
            kRtpMtu);

        packetizer->addToChain(
            std::make_shared<rtc::RtcpSrReporter>(rtp_config));
        packetizer->addToChain(
            std::make_shared<rtc::RtcpNackResponder>(kNackHistoryPackets));
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([this] {
            KeyFrameRequestCallback callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback = keyframe_request_callback_;
            }
            if (callback) {
                callback();
            }
        }));
        packetizer->addToChain(
            std::make_shared<rtc::RembHandler>([this](unsigned int bitrate_bps) {
                BitrateRecommendationCallback callback;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    callback = bitrate_recommendation_callback_;
                }
                if (callback && bitrate_bps > 0) {
                    callback(bitrate_bps);
                }
            }));
        video_track->setMediaHandler(packetizer);

        std::lock_guard<std::mutex> lock(transport_mutex_);
        video_track_ = std::move(video_track);
    }

    void CreateControlChannel(
        const std::shared_ptr<rtc::PeerConnection>& peer_connection) {
        auto data_channel =
            peer_connection->createDataChannel(kControlChannelLabel);
        ConfigureControlChannel(data_channel);

        std::lock_guard<std::mutex> lock(transport_mutex_);
        data_channel_ = std::move(data_channel);
    }

    void ConfigureControlChannel(
        const std::shared_ptr<rtc::DataChannel>& data_channel) {
        const std::string label = data_channel->label();
        data_channel->onOpen([label] {
            std::printf(
                "[WebrtcStreamer] DataChannel opened: %s\n",
                label.c_str());
            std::fflush(stdout);
        });
        data_channel->onClosed([label] {
            std::printf(
                "[WebrtcStreamer] DataChannel closed: %s\n",
                label.c_str());
            std::fflush(stdout);
        });
        data_channel->onMessage([this](rtc::message_variant message) {
            if (!std::holds_alternative<std::string>(message)) {
                return;
            }

            MessageCallback callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback = message_callback_;
            }
            if (callback) {
                callback(std::get<std::string>(message));
            }
        });
    }

    std::shared_ptr<rtc::PeerConnection> GetPeerConnection() const {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        return peer_connection_;
    }

    mutable std::mutex transport_mutex_;
    std::shared_ptr<rtc::PeerConnection> peer_connection_;
    std::shared_ptr<rtc::Track> video_track_;
    std::shared_ptr<rtc::DataChannel> data_channel_;

    mutable std::mutex callback_mutex_;
    LocalDescriptionCallback local_description_callback_;
    LocalCandidateCallback local_candidate_callback_;
    MessageCallback message_callback_;
    StateChangeCallback state_change_callback_;
    KeyFrameRequestCallback keyframe_request_callback_;
    BitrateRecommendationCallback bitrate_recommendation_callback_;

    std::atomic<bool> connected_{false};
    uint64_t first_video_timestamp_us_ = 0;
};

WebrtcStreamer::WebrtcStreamer()
    : impl_(std::make_unique<Impl>()) {}

WebrtcStreamer::~WebrtcStreamer() = default;

bool WebrtcStreamer::Initialize() {
    return impl_->Initialize();
}

void WebrtcStreamer::Shutdown() {
    impl_->Shutdown();
}

void WebrtcStreamer::SetLocalDescriptionCallback(
    LocalDescriptionCallback callback) {
    impl_->SetLocalDescriptionCallback(std::move(callback));
}

void WebrtcStreamer::SetLocalCandidateCallback(
    LocalCandidateCallback callback) {
    impl_->SetLocalCandidateCallback(std::move(callback));
}

void WebrtcStreamer::SetMessageCallback(MessageCallback callback) {
    impl_->SetMessageCallback(std::move(callback));
}

void WebrtcStreamer::SetStateChangeCallback(StateChangeCallback callback) {
    impl_->SetStateChangeCallback(std::move(callback));
}

void WebrtcStreamer::SetKeyFrameRequestCallback(
    KeyFrameRequestCallback callback) {
    impl_->SetKeyFrameRequestCallback(std::move(callback));
}

void WebrtcStreamer::SetBitrateRecommendationCallback(
    BitrateRecommendationCallback callback) {
    impl_->SetBitrateRecommendationCallback(std::move(callback));
}

bool WebrtcStreamer::ProcessSignalingMessage(
    const std::string& message_type,
    const std::string& sdp_or_candidate,
    const std::string& mid) {
    return impl_->ProcessSignalingMessage(
        message_type, sdp_or_candidate, mid);
}

bool WebrtcStreamer::PushVideoFrame(
    const uint8_t* data,
    size_t size,
    uint64_t timestamp_us) {
    return impl_->PushVideoFrame(data, size, timestamp_us);
}

bool WebrtcStreamer::SendMessageOverDataChannel(
    const std::string& message) {
    return impl_->SendMessageOverDataChannel(message);
}

bool WebrtcStreamer::IsConnected() const {
    return impl_->IsConnected();
}
