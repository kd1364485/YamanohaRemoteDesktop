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
constexpr char kInputChannelLabel[] = "input";

} // namespace

class WebrtcStreamer::Impl {
public:
    struct CallbackContext {
        std::mutex mutex;
        LocalDescriptionCallback local_description;
        LocalCandidateCallback local_candidate;
        MessageCallback message;
        StateChangeCallback state_change;
        KeyFrameRequestCallback keyframe_request;
        BitrateRecommendationCallback bitrate_recommendation;
        std::atomic<bool> active{false};
        std::atomic<bool> connected{false};
    };

    ~Impl() {
        Shutdown(true);
    }

    bool Initialize() {
        Shutdown();
        first_video_timestamp_us_ = 0;
        callback_context_->active.store(true, std::memory_order_release);

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

    void Shutdown(bool clear_callbacks = false) {
        callback_context_->active.store(false, std::memory_order_release);
        callback_context_->connected.store(false, std::memory_order_release);
        {
            // Callback invocation also holds this mutex. Acquiring it here
            // waits for in-flight callbacks before their owners are destroyed.
            std::lock_guard<std::mutex> lock(callback_context_->mutex);
            if (clear_callbacks) {
                callback_context_->local_description = {};
                callback_context_->local_candidate = {};
                callback_context_->message = {};
                callback_context_->state_change = {};
                callback_context_->keyframe_request = {};
                callback_context_->bitrate_recommendation = {};
            }
        }

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
            !callback_context_->connected.load(std::memory_order_acquire)) {
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
        return callback_context_->connected.load(std::memory_order_acquire);
    }

    void SetLocalDescriptionCallback(LocalDescriptionCallback callback) {
        std::lock_guard<std::mutex> lock(callback_context_->mutex);
        callback_context_->local_description = std::move(callback);
    }

    void SetLocalCandidateCallback(LocalCandidateCallback callback) {
        std::lock_guard<std::mutex> lock(callback_context_->mutex);
        callback_context_->local_candidate = std::move(callback);
    }

    void SetMessageCallback(MessageCallback callback) {
        std::lock_guard<std::mutex> lock(callback_context_->mutex);
        callback_context_->message = std::move(callback);
    }

    void SetStateChangeCallback(StateChangeCallback callback) {
        std::lock_guard<std::mutex> lock(callback_context_->mutex);
        callback_context_->state_change = std::move(callback);
    }

    void SetKeyFrameRequestCallback(KeyFrameRequestCallback callback) {
        std::lock_guard<std::mutex> lock(callback_context_->mutex);
        callback_context_->keyframe_request = std::move(callback);
    }

    void SetBitrateRecommendationCallback(
        BitrateRecommendationCallback callback) {
        std::lock_guard<std::mutex> lock(callback_context_->mutex);
        callback_context_->bitrate_recommendation = std::move(callback);
    }

private:
    void ConfigurePeerConnection(
        const std::shared_ptr<rtc::PeerConnection>& peer_connection) {
        const auto context = callback_context_;
        peer_connection->onStateChange([context](rtc::PeerConnection::State state) {
            if (!context->active.load(std::memory_order_acquire)) {
                return;
            }
            const bool connected =
                state == rtc::PeerConnection::State::Connected;
            context->connected.store(connected, std::memory_order_release);

            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->state_change) {
                context->state_change(connected);
            }
        });

        std::weak_ptr<rtc::PeerConnection> weak_peer_connection =
            peer_connection;
        peer_connection->onGatheringStateChange(
            [context, weak_peer_connection](
                rtc::PeerConnection::GatheringState state) {
                if (!context->active.load(std::memory_order_acquire)) {
                    return;
                }
                if (state != rtc::PeerConnection::GatheringState::Complete) {
                    return;
                }

                auto peer_connection = weak_peer_connection.lock();
                if (!peer_connection) {
                    return;
                }
                auto description = peer_connection->localDescription();
                if (!description) {
                    return;
                }

                std::lock_guard<std::mutex> lock(context->mutex);
                if (context->local_description) {
                    context->local_description(
                        std::string(description.value()),
                        description->typeString());
                }
            });

        peer_connection->onLocalCandidate([context](rtc::Candidate candidate) {
            if (!context->active.load(std::memory_order_acquire)) {
                return;
            }
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->local_candidate) {
                context->local_candidate(candidate.candidate(), candidate.mid());
            }
        });

        peer_connection->onDataChannel(
            [context](std::shared_ptr<rtc::DataChannel> data_channel) {
                if (!context->active.load(std::memory_order_acquire)) {
                    return;
                }
                if (data_channel->label() == kInputChannelLabel ||
                    data_channel->label() == "control") {
                    ConfigureControlChannel(data_channel, context);
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
        const auto context = callback_context_;
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([context] {
            if (!context->active.load(std::memory_order_acquire)) {
                return;
            }
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->keyframe_request) {
                context->keyframe_request();
            }
        }));
        packetizer->addToChain(
            std::make_shared<rtc::RembHandler>([context](unsigned int bitrate_bps) {
                if (!context->active.load(std::memory_order_acquire)) {
                    return;
                }
                std::lock_guard<std::mutex> lock(context->mutex);
                if (context->bitrate_recommendation && bitrate_bps > 0) {
                    context->bitrate_recommendation(bitrate_bps);
                }
            }));
        video_track->setMediaHandler(packetizer);

        std::lock_guard<std::mutex> lock(transport_mutex_);
        video_track_ = std::move(video_track);
    }

    void CreateControlChannel(
        const std::shared_ptr<rtc::PeerConnection>& peer_connection) {
        auto data_channel =
            peer_connection->createDataChannel(kInputChannelLabel);
        ConfigureControlChannel(data_channel, callback_context_);

        std::lock_guard<std::mutex> lock(transport_mutex_);
        data_channel_ = std::move(data_channel);
    }

    static void ConfigureControlChannel(
        const std::shared_ptr<rtc::DataChannel>& data_channel,
        const std::shared_ptr<CallbackContext>& context) {
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
        data_channel->onMessage([context](rtc::message_variant message) {
            if (!context->active.load(std::memory_order_acquire)) {
                return;
            }
            if (!std::holds_alternative<std::string>(message)) {
                return;
            }

            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->message) {
                context->message(std::get<std::string>(message));
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

    std::shared_ptr<CallbackContext> callback_context_ =
        std::make_shared<CallbackContext>();
    uint64_t first_video_timestamp_us_ = 0;
};

WebrtcStreamer::WebrtcStreamer()
    : impl_(std::make_unique<Impl>()) {}

WebrtcStreamer::~WebrtcStreamer() = default;

bool WebrtcStreamer::Initialize() {
    return impl_->Initialize();
}

void WebrtcStreamer::Shutdown() {
    impl_->Shutdown(true);
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
