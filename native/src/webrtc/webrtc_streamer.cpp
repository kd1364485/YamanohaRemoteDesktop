#include "webrtc_streamer.h"
#include <rtc/rtc.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <iostream>
#include <chrono>


// ---------------------------------------------------------------------------
// WebrtcStreamer::Impl 定義
// ---------------------------------------------------------------------------
class WebrtcStreamer::Impl {
public:
    Impl() = default;
    ~Impl() { Shutdown(); }

    bool Initialize() {
        Shutdown();

        try {
            // libdatachannelのログ設定 (Infoレベルで出力)
            rtc::InitLogger(rtc::LogLevel::Info);

            rtc::Configuration config;
            // パブリックなSTUNサーバーを登録
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");
            config.iceServers.emplace_back("stun:stun1.l.google.com:19302");
            config.iceServers.emplace_back("stun:stun2.l.google.com:19302");

            // PeerConnectionの作成
            peer_connection_ = std::make_shared<rtc::PeerConnection>(config);

            // 接続状態変更コールバック
            peer_connection_->onStateChange([this](rtc::PeerConnection::State state) {
                std::printf("[WebrtcStreamer] PeerConnection 状態変更: %d\n", static_cast<int>(state));
                std::fflush(stdout);

                bool connected = (state == rtc::PeerConnection::State::Connected);
                is_connected_ = connected;
                if (state_change_cb_) {
                    state_change_cb_(connected);
                }
            });

            // ICE Gathering完了コールバック（Vanilla SDP用）
            peer_connection_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
                std::printf("[WebrtcStreamer] ICE Gathering 状態変更: %d\n", static_cast<int>(state));
                std::fflush(stdout);

                if (state == rtc::PeerConnection::GatheringState::Complete) {
                    auto local_desc = peer_connection_->localDescription();
                    if (local_desc && local_description_cb_) {
                        std::printf("[WebrtcStreamer] Vanilla SDP生成完了 (%s)\n", local_desc->typeString().c_str());
                        std::fflush(stdout);
                        local_description_cb_(std::string(local_desc.value()), local_desc->typeString());
                    }
                }
            });

            // ローカルICE候補が生成された時のコールバック (Trickle ICE用)
            peer_connection_->onLocalCandidate([this](rtc::Candidate candidate) {
                if (local_candidate_cb_) {
                    local_candidate_cb_(candidate.candidate(), candidate.mid());
                }
            });

            // 映像トラック（H.264）のセットアップ
            const rtc::SSRC ssrc = 1001;
            rtc::Description::Video video_desc("video", rtc::Description::Direction::SendOnly);
            video_desc.addH264Codec(96); // ペイロードタイプ 96
            video_desc.addSSRC(ssrc, "video-stream");

            video_track_ = peer_connection_->addTrack(video_desc);

            // H.264 RTP パケタイザをセット
            auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "video-stream", 96, rtc::H264RtpPacketizer::defaultClockRate);
            // MTUサイズを標準的な 1200 バイトに制限してフラグメンテーションを防ぐ
            auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence,
                rtp_config,
                1200
            );
            video_track_->setMediaHandler(packetizer);

            // マウス・キーボードなどの入力制御用 DataChannel のセットアップ
            data_channel_ = peer_connection_->createDataChannel("control");
            SetupDataChannelCallbacks(data_channel_);

            // リモートがデータチャネルを作成した際もハンドリングできるように登録
            peer_connection_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
                std::printf("[WebrtcStreamer] リモートによる DataChannel 開設を検知: %s\n", dc->label().c_str());
                std::fflush(stdout);
                if (dc->label() == "control") {
                    data_channel_ = dc;
                    SetupDataChannelCallbacks(data_channel_);
                }
            });

            return true;

        } catch (const std::exception& e) {
            std::printf("[WebrtcStreamer] 初期化中に例外発生: %s\n", e.what());
            std::fflush(stdout);
            return false;
        }
    }

    void Shutdown() {
        is_connected_ = false;
        if (data_channel_) {
            data_channel_->close();
            data_channel_.reset();
        }
        if (video_track_) {
            video_track_.reset();
        }
        if (peer_connection_) {
            peer_connection_->close();
            peer_connection_.reset();
        }
    }

    bool ProcessSignalingMessage(const std::string& msg_type, const std::string& sdp_or_candidate, const std::string& mid) {
        if (!peer_connection_) return false;

        try {
            if (msg_type == "offer" || msg_type == "answer") {
                rtc::Description desc(sdp_or_candidate, msg_type);
                peer_connection_->setRemoteDescription(desc);
                std::printf("[WebrtcStreamer] リモートSDP (%s) の設定に成功しました\n", msg_type.c_str());
                std::fflush(stdout);

                // もしオファーを受け取った場合は、アンサーを生成してローカルにセットする
                if (msg_type == "offer") {
                    peer_connection_->setLocalDescription();
                }
                return true;
            } 
            else if (msg_type == "candidate") {
                rtc::Candidate candidate(sdp_or_candidate, mid);
                peer_connection_->addRemoteCandidate(candidate);
                return true;
            }
        } catch (const std::exception& e) {
            std::printf("[WebrtcStreamer] シグナリング処理中に例外発生: %s\n", e.what());
            std::fflush(stdout);
        }
        return false;
    }

    bool PushVideoFrame(const uint8_t* data, size_t size, uint64_t timestamp_us) {
        if (!video_track_ || !video_track_->isOpen() || !is_connected_) {
            return false;
        }

        try {
            // NALU データをトラックに送信。パケタイザが自動でFU-A分割・RTP化を行う
            // 送信時は std::byte 形式へキャストする
            auto t_send_inner_start = std::chrono::high_resolution_clock::now();
            video_track_->send(reinterpret_cast<const std::byte*>(data), size);
            auto t_send_inner_end = std::chrono::high_resolution_clock::now();
            auto inner_us = std::chrono::duration_cast<std::chrono::microseconds>(t_send_inner_end - t_send_inner_start).count();

            // 1秒に1回だけ send() 内部時間を出力
            static auto last_inner_log = std::chrono::high_resolution_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(t_send_inner_end - last_inner_log).count() >= 1) {
                last_inner_log = t_send_inner_end;
                std::printf("[WebrtcStreamer] video_track->send inner: %lld us, size: %zu bytes\n", inner_us, size);
                std::fflush(stdout);
            }
            return true;
        } catch (const std::exception& e) {
            std::printf("[WebrtcStreamer] ビデオフレーム送信中に例外発生: %s\n", e.what());
            std::fflush(stdout);
            return false;
        }
    }

    bool SendMessageOverDataChannel(const std::string& msg) {
        if (!data_channel_ || !data_channel_->isOpen()) {
            return false;
        }
        try {
            data_channel_->send(msg);
            return true;
        } catch (const std::exception& e) {
            std::printf("[WebrtcStreamer] メッセージ送信中に例外発生: %s\n", e.what());
            std::fflush(stdout);
            return false;
        }
    }

    bool IsConnected() const { return is_connected_; }

    // コールバック登録関数群
    void SetLocalDescriptionCallback(std::function<void(const std::string& sdp, const std::string& type)> cb) {
        local_description_cb_ = cb;
    }

    void SetLocalCandidateCallback(std::function<void(const std::string& candidate, const std::string& mid)> cb) {
        local_candidate_cb_ = cb;
    }

    void SetMessageCallback(std::function<void(const std::string& msg)> cb) {
        message_cb_ = cb;
    }

    void SetStateChangeCallback(std::function<void(bool connected)> cb) {
        state_change_cb_ = cb;
    }

private:
    void SetupDataChannelCallbacks(std::shared_ptr<rtc::DataChannel>& dc) {
        dc->onOpen([dc]() {
            std::printf("[WebrtcStreamer] DataChannel (%s) が開きました\n", dc->label().c_str());
            std::fflush(stdout);
        });

        dc->onClosed([dc]() {
            std::printf("[WebrtcStreamer] DataChannel (%s) が閉じました\n", dc->label().c_str());
            std::fflush(stdout);
        });

        dc->onMessage([this](rtc::message_variant message) {
            if (std::holds_alternative<std::string>(message)) {
                auto str_msg = std::get<std::string>(message);
                if (message_cb_) {
                    message_cb_(str_msg);
                }
            }
        });
    }

    std::shared_ptr<rtc::PeerConnection> peer_connection_;
    std::shared_ptr<rtc::Track> video_track_;
    std::shared_ptr<rtc::DataChannel> data_channel_;

    std::function<void(const std::string& sdp, const std::string& type)> local_description_cb_;
    std::function<void(const std::string& candidate, const std::string& mid)> local_candidate_cb_;
    std::function<void(const std::string& msg)> message_cb_;
    std::function<void(bool connected)> state_change_cb_;

    bool is_connected_ = false;
};

// ---------------------------------------------------------------------------
// WebrtcStreamer インターフェース実装
// ---------------------------------------------------------------------------
WebrtcStreamer::WebrtcStreamer() : impl_(std::make_unique<Impl>()) {}
WebrtcStreamer::~WebrtcStreamer() = default;

bool WebrtcStreamer::Initialize() { return impl_->Initialize(); }
void WebrtcStreamer::Shutdown() { impl_->Shutdown(); }

void WebrtcStreamer::SetLocalDescriptionCallback(std::function<void(const std::string& sdp, const std::string& type)> cb) {
    impl_->SetLocalDescriptionCallback(cb);
}

void WebrtcStreamer::SetLocalCandidateCallback(std::function<void(const std::string& candidate, const std::string& mid)> cb) {
    impl_->SetLocalCandidateCallback(cb);
}

void WebrtcStreamer::SetMessageCallback(std::function<void(const std::string& msg)> cb) {
    impl_->SetMessageCallback(cb);
}

void WebrtcStreamer::SetStateChangeCallback(std::function<void(bool connected)> cb) {
    impl_->SetStateChangeCallback(cb);
}

bool WebrtcStreamer::ProcessSignalingMessage(const std::string& msg_type, const std::string& sdp_or_candidate, const std::string& mid) {
    return impl_->ProcessSignalingMessage(msg_type, sdp_or_candidate, mid);
}

bool WebrtcStreamer::PushVideoFrame(const uint8_t* data, size_t size, uint64_t timestamp_us) {
    return impl_->PushVideoFrame(data, size, timestamp_us);
}

bool WebrtcStreamer::SendMessageOverDataChannel(const std::string& msg) {
    return impl_->SendMessageOverDataChannel(msg);
}

bool WebrtcStreamer::IsConnected() const {
    return impl_->IsConnected();
}
