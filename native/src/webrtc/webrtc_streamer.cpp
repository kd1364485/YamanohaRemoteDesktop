// ============================================================================
// webrtc_streamer.cpp — libdatachannel を用いた WebRTC 配信の実装
// Pimpl イディオムで実装を隠蔽し、H.264 RTP パケット化・NACK再送・
// PLI/REMB 処理・入力用 DataChannel を管理する
// ============================================================================

#include "webrtc_streamer.h"

#include <rtc/h264rtppacketizer.hpp>  // H264RtpPacketizer
#include <rtc/rtc.hpp>                // libdatachannel の主要API

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace {

// --- WebRTC 定数 ---
constexpr rtc::SSRC kVideoSsrc = 1001;              // ビデオストリームのSSRC識別子
constexpr uint8_t kH264PayloadType = 96;             // H.264のRTPペイロードタイプ
constexpr size_t kRtpMtu = 1200;                     // RTPパケットの最大サイズ（MTU）
constexpr size_t kNackHistoryPackets = 2048;         // NACK再送用のパケット履歴サイズ
constexpr char kVideoCname[] = "video-stream";       // RTCP CNAME（ストリーム識別名）
constexpr char kInputChannelLabel[] = "input";       // 入力転送用DataChannelのラベル名

} // namespace

// ============================================================================
// WebrtcStreamer::Impl — Pimpl実装クラス
// libdatachannel の rtc::PeerConnection / Track / DataChannel を管理し、
// 非同期コールバックをスレッドセーフに処理する
// ============================================================================
class WebrtcStreamer::Impl {
public:
    // コールバック管理コンテキスト
    // shared_ptr で共有され、libdatachannel の非同期コールバック内で参照される
    // WebrtcStreamer より長寿命になる可能性があるため、active/connected で安全にチェック
    struct CallbackContext {
        std::mutex mutex;                                   // コールバック呼び出し中の安全性を保証
        LocalDescriptionCallback local_description;         // SDP生成時に呼ばれる（→ブラウザにAnswer送信）
        LocalCandidateCallback local_candidate;             // ICE候補生成時に呼ばれる（→ブラウザにCandidate送信）
        MessageCallback message;                            // DataChannelメッセージ受信時に呼ばれる（→入力イベント処理）
        StateChangeCallback state_change;                   // 接続状態変化時に呼ばれる（→Rust層に通知）
        KeyFrameRequestCallback keyframe_request;           // キーフレーム要求時に呼ばれる（→FramePipelineに通知）
        BitrateRecommendationCallback bitrate_recommendation; // ビットレート推定受信時に呼ばれる（→AdaptiveBitrateControllerに通知）
        std::atomic<bool> active{false};    // WebrtcStreamerがアクティブか（シャットダウン後にコールバックを無視）
        std::atomic<bool> connected{false}; // WebRTC接続が確立しているか（CaptureLoopがチェック）
    };

    // デストラクタ: コールバックをクリアしてからリソースを解放
    ~Impl() {
        Shutdown(true);
    }

    // WebRTCセッションを初期化する
    // PeerConnection作成 → コールバック設定 → ビデオトラック作成 → DataChannel作成
    bool Initialize() {
        Shutdown();  // 既存セッションがあればクリーンアップ
        first_video_timestamp_us_ = 0;  // タイムスタンプ基準をリセット
        callback_context_->active.store(true, std::memory_order_release);

        try {
            // libdatachannel のログレベルを Info に設定
            rtc::InitLogger(rtc::LogLevel::Info);

            // ICEサーバー設定（STUN）
            // Google のパブリックSTUNサーバーを使用してNAT越え
            rtc::Configuration configuration;
            configuration.iceServers.emplace_back("stun:stun.l.google.com:19302");
            configuration.iceServers.emplace_back("stun:stun1.l.google.com:19302");
            configuration.iceServers.emplace_back("stun:stun2.l.google.com:19302");

            // TURNサーバー（厳格なNAT環境用のリレー）
            // 環境変数 YAMANOHA_TURN_SERVER から取得可能
            // 形式: "turn:host:port:username:credential" または "turns:host:port:username:credential"
            if (const char* turn_env = std::getenv("YAMANOHA_TURN_SERVER")) {
                std::string turn_str(turn_env);
                std::printf("[WebrtcStreamer] TURN server from env: %s\n", turn_str.c_str());
                configuration.iceServers.emplace_back(turn_str);
            }

            // PeerConnection作成
            auto peer_connection =
                std::make_shared<rtc::PeerConnection>(configuration);
            {
                std::lock_guard<std::mutex> lock(transport_mutex_);
                peer_connection_ = peer_connection;
            }

            // 3つのコンポーネントを設定
            ConfigurePeerConnection(peer_connection);   // 状態変化・SDP・ICE候補・DataChannel受信のコールバック
            CreateVideoTrack(peer_connection);          // H.264ビデオトラック（RTPパケット化チェーン構築）
            CreateControlChannel(peer_connection);      // 入力転送用DataChannel（信頼性なしモード）
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

    // WebRTCセッションをシャットダウンする
    // clear_callbacks=true の場合はコールバック関数もクリアする（デストラクタ用）
    void Shutdown(bool clear_callbacks = false) {
        // [1] active/connected フラグを false にして非同期コールバックを無効化
        callback_context_->active.store(false, std::memory_order_release);
        callback_context_->connected.store(false, std::memory_order_release);

        // [2] 実行中のコールバックが完了するまで待機（mutex取得で保証）
        // コールバック呼び出し側もこの mutex を保持するため、ここで取得すれば
        // コールバックの所有者が破棄される前に完了を待てる
        {
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

        // [3] transport系オブジェクトをロック内で移動（デッドロック防止）
        std::shared_ptr<rtc::DataChannel> data_channel;
        std::shared_ptr<rtc::PeerConnection> peer_connection;
        {
            std::lock_guard<std::mutex> lock(transport_mutex_);
            data_channel = std::move(data_channel_);
            video_track_.reset();
            peer_connection = std::move(peer_connection_);
        }

        // [4] ロック外でクローズ（libdatachannel の close は内部でロックを取る可能性あり）
        if (data_channel) {
            data_channel->close();
        }
        if (peer_connection) {
            peer_connection->close();
        }
    }

    // シグナリングメッセージを処理する
    // ブラウザから受信した SDP Answer / ICE Candidate を PeerConnection に設定
    // message_type: "offer", "answer", または "candidate"
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
                // リモートSDPを設定
                rtc::Description description(sdp_or_candidate, message_type);
                peer_connection->setRemoteDescription(description);
                // offerを受信した場合は Answer を生成（ローカルSDP作成）
                if (message_type == "offer") {
                    peer_connection->setLocalDescription();
                }
                return true;
            }

            if (message_type == "candidate") {
                // リモートICE候補を追加
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

    // H.264エンコード済みフレームをWebRTC経由で送信する
    // FramePipeline::SenderLoop から呼ばれる
    // data: H.264ビットストリーム, timestamp_us: キャプチャ時刻（マイクロ秒）
    bool PushVideoFrame(
        const uint8_t* data,
        size_t size,
        uint64_t timestamp_us) {
        // 接続していない、またはデータが空の場合は送信しない
        if (!data || size == 0 ||
            !callback_context_->connected.load(std::memory_order_acquire)) {
            return false;
        }

        // video_track をスレッドセーフに取得
        std::shared_ptr<rtc::Track> video_track;
        {
            std::lock_guard<std::mutex> lock(transport_mutex_);
            video_track = video_track_;
        }
        // トラックが未準備・未接続の場合はスキップ（ログは2秒に1回のみ出力）
        if (!video_track || !video_track->isOpen()) {
            static auto last_track_warn =
                std::chrono::steady_clock::time_point::min();
            auto now = std::chrono::steady_clock::now();
            if (now - last_track_warn > std::chrono::seconds(2)) {
                std::printf("[WebrtcStreamer] PushVideoFrame: track not open (track=%p isOpen=%d)\n",
                    video_track.get(), video_track ? video_track->isOpen() : 0);
                std::fflush(stdout);
                last_track_warn = now;
            }
            return false;
        }

        try {
            // RTPタイムスタンプを相対値に変換（最初のフレームを0とする）
            // タイムスタンプが巻き戻った場合（セッション切替等）は基準を更新
            if (first_video_timestamp_us_ == 0 ||
                timestamp_us < first_video_timestamp_us_) {
                first_video_timestamp_us_ = timestamp_us;
            }
            const uint64_t relative_timestamp_us =
                timestamp_us - first_video_timestamp_us_;

            // H.264ビットストリームをRTPパケット化して送信
            // sendFrame 内部で H264RtpPacketizer → SR → NACK → PLI → REMB チェーンが実行される
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

    // DataChannel経由でメッセージを送信する（ホスト→ブラウザ方向）
    // 入力イベントの確認応答や状態通知などに使用
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

    // WebRTC接続が確立しているかを返す
    // FramePipeline::CaptureLoop がフレーム送信可否の判定に使用
    bool IsConnected() const {
        return callback_context_->connected.load(std::memory_order_acquire);
    }

    // --- コールバック登録メソッド ---
    // 全て callback_context_->mutex を保持してスレッドセーフに登録する
    // DisplaySessionManager 経由で FramePipeline と接続される

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
    // ============================================================================
    // ConfigurePeerConnection — PeerConnection のコールバックを設定
    // 状態変化・SDP生成・ICE候補・DataChannel受信の4種類を登録
    // ============================================================================
    void ConfigurePeerConnection(
        const std::shared_ptr<rtc::PeerConnection>& peer_connection) {
        const auto context = callback_context_;

        // --- [1] 接続状態変化コールバック ---
        // Connected / Disconnected / Failed などの状態変化時に呼ばれる
        peer_connection->onStateChange([context](rtc::PeerConnection::State state) {
            if (!context->active.load(std::memory_order_acquire)) {
                return;  // シャットダウン後は無視
            }

            // 状態名を文字列に変換してログ出力
            const char* state_name = "Unknown";
            switch (state) {
                case rtc::PeerConnection::State::New:         state_name = "New"; break;
                case rtc::PeerConnection::State::Connecting:  state_name = "Connecting"; break;
                case rtc::PeerConnection::State::Connected:   state_name = "Connected"; break;
                case rtc::PeerConnection::State::Disconnected: state_name = "Disconnected"; break;
                case rtc::PeerConnection::State::Failed:      state_name = "Failed"; break;
                case rtc::PeerConnection::State::Closed:      state_name = "Closed"; break;
            }
            std::printf("[WebrtcStreamer] PeerConnection state: %s\n", state_name);
            std::fflush(stdout);

            // connected フラグを更新（CaptureLoop が IsConnected で参照）
            const bool connected =
                state == rtc::PeerConnection::State::Connected;
            const bool was_connected =
                context->connected.load(std::memory_order_acquire);
            context->connected.store(connected, std::memory_order_release);

            // 切断→再接続時にキーフレームを強制要求
            // クライアントデコーダが一時的な切断後に再同期するため
            if (connected && !was_connected) {
                std::lock_guard<std::mutex> lock(context->mutex);
                if (context->keyframe_request) {
                    std::printf("[WebrtcStreamer] Forcing keyframe on connection recovery\n");
                    std::fflush(stdout);
                    context->keyframe_request();
                }
            }

            // 接続状態変化を上位層に通知
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->state_change) {
                context->state_change(connected);
            }
        });

        // --- [2] ICE候補収集完了コールバック ---
        // 全てのICE候補の収集が完了した時にローカルSDPを生成して通知
        // weak_ptr をキャプチャして PeerConnection の寿命を安全に管理
        std::weak_ptr<rtc::PeerConnection> weak_peer_connection =
            peer_connection;
        peer_connection->onGatheringStateChange(
            [context, weak_peer_connection](
                rtc::PeerConnection::GatheringState state) {
                if (!context->active.load(std::memory_order_acquire)) {
                    return;
                }
                // Complete 状態（全候補収集完了）の時のみ処理
                if (state != rtc::PeerConnection::GatheringState::Complete) {
                    return;
                }

                auto peer_connection = weak_peer_connection.lock();
                if (!peer_connection) {
                    return;
                }
                // ローカルSDP（全ICE候補込み）を取得
                auto description = peer_connection->localDescription();
                if (!description) {
                    return;
                }

                // SDPを上位層に通知（→Rust経由でブラウザに送信）
                std::lock_guard<std::mutex> lock(context->mutex);
                if (context->local_description) {
                    context->local_description(
                        std::string(description.value()),
                        description->typeString());
                }
            });

        // --- [3] ローカルICE候補生成コールバック ---
        // 個別のICE候補が見つかるたびに呼ばれる（Trickle ICE）
        peer_connection->onLocalCandidate([context](rtc::Candidate candidate) {
            if (!context->active.load(std::memory_order_acquire)) {
                return;
            }
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->local_candidate) {
                context->local_candidate(candidate.candidate(), candidate.mid());
            }
        });

        // --- [4] リモートDataChannel受信コールバック ---
        // ブラウザからDataChannelが作成された時に呼ばれる
        // "input" または "control" ラベルのチャネルを入力転送用として設定
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

    // ============================================================================
    // CreateVideoTrack — H.264ビデオトラックを作成し、RTPパケット化チェーンを構築
    // 
    // チェーン構成:
    //   H264RtpPacketizer → RtcpSrReporter → RtcpNackResponder → PliHandler → RembHandler
    //   (NAL分割)        (SR送信)        (NACK再送)         (PLI→KeyFrame) (REMB→Bitrate)
    // ============================================================================
    void CreateVideoTrack(
        const std::shared_ptr<rtc::PeerConnection>& peer_connection) {
        // ビデオトラックのSDP記述を作成（送信専用、H.264コーデック）
        rtc::Description::Video description(
            "video", rtc::Description::Direction::SendOnly);
        description.addH264Codec(kH264PayloadType);
        description.addSSRC(kVideoSsrc, kVideoCname);

        // PeerConnectionにトラックを追加
        auto video_track = peer_connection->addTrack(description);

        // RTPパケット化の設定（SSRC, CNAME, ペイロードタイプ, クロックレート）
        auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
            kVideoSsrc,
            kVideoCname,
            kH264PayloadType,
            rtc::H264RtpPacketizer::defaultClockRate);  // 90000Hz（ビデオ標準）

        // H.264 RTPパケット化器を作成
        // StartSequence: NAL区切りに4バイト開始コードを使用
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence,
            rtp_config,
            kRtpMtu);

        // --- RTP処理チェーンを構築 ---

        // [1] RTCP Sender Report: 送信統計情報を定期的に送信
        packetizer->addToChain(
            std::make_shared<rtc::RtcpSrReporter>(rtp_config));

        // [2] NACK再送: パケットロス時にブラウザからのNACK要求で再送
        //     2048パケット分の履歴を保持
        packetizer->addToChain(
            std::make_shared<rtc::RtcpNackResponder>(kNackHistoryPackets));

        // [3] PLIハンドラ: ブラウザからPLI（Picture Loss Indication）を受信
        //     デコーダがフレームをロストした時、キーフレーム要求を発火
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

        // [4] REMBハンドラ: ブラウザからREMB（Receiver Estimated Maximum Bitrate）を受信
        //     ネットワーク帯域に基づくビットレート推定値を上位層に通知
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

        // パケット化チェーンをビデオトラックに設定
        video_track->setMediaHandler(packetizer);

        // トラッククローズ時のログ
        video_track->onClosed([] {
            std::printf("[WebrtcStreamer] Video track closed\n");
            std::fflush(stdout);
        });

        // メンバに保存（スレッドセーフ）
        std::lock_guard<std::mutex> lock(transport_mutex_);
        video_track_ = std::move(video_track);
    }

    // ============================================================================
    // CreateControlChannel — 入力転送用DataChannelを作成
    // マウス/キーボード入力をブラウザからホストに転送するチャネル
    // ============================================================================
    void CreateControlChannel(
        const std::shared_ptr<rtc::PeerConnection>& peer_connection) {
        rtc::DataChannelInit dc_config;
        // 入力イベントはリアルタイム性が重要:
        // 順序保証なし・再送なし（UDPライク）で、パケットロスが後続入力をブロックしない
        dc_config.reliability.unordered = true;
        dc_config.reliability.maxRetransmits = 0;

        // "input" ラベルでDataChannelを作成（ホスト側から作成）
        auto data_channel =
            peer_connection->createDataChannel(kInputChannelLabel, dc_config);
        ConfigureControlChannel(data_channel, callback_context_);

        std::lock_guard<std::mutex> lock(transport_mutex_);
        data_channel_ = std::move(data_channel);
    }

    // DataChannelのコールバックを設定する（静的メソッド）
    // CreateControlChannel と onDataChannel の両方から呼ばれる
    static void ConfigureControlChannel(
        const std::shared_ptr<rtc::DataChannel>& data_channel,
        const std::shared_ptr<CallbackContext>& context) {
        const std::string label = data_channel->label();

        // チャネルオープン時のログ
        data_channel->onOpen([label] {
            std::printf(
                "[WebrtcStreamer] DataChannel opened: %s\n",
                label.c_str());
            std::fflush(stdout);
        });

        // チャネルクローズ時のログ
        data_channel->onClosed([label] {
            std::printf(
                "[WebrtcStreamer] DataChannel closed: %s\n",
                label.c_str());
            std::fflush(stdout);
        });

        // メッセージ受信コールバック: ブラウザからの入力イベントを処理
        data_channel->onMessage([context](rtc::message_variant message) {
            if (!context->active.load(std::memory_order_acquire)) {
                return;
            }
            // 文字列メッセージのみ処理（バイナリは無視）
            if (!std::holds_alternative<std::string>(message)) {
                return;
            }

            // 入力メッセージを上位層に通知（→Rust層で入力イベントとして処理）
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->message) {
                context->message(std::get<std::string>(message));
            }
        });
    }

    // PeerConnectionをスレッドセーフに取得
    std::shared_ptr<rtc::PeerConnection> GetPeerConnection() const {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        return peer_connection_;
    }

    // --- メンバ変数 ---
    mutable std::mutex transport_mutex_;                    // transport系オブジェクトの保護
    std::shared_ptr<rtc::PeerConnection> peer_connection_;  // WebRTC PeerConnection
    std::shared_ptr<rtc::Track> video_track_;               // H.264ビデオトラック
    std::shared_ptr<rtc::DataChannel> data_channel_;        // 入力転送用DataChannel

    std::shared_ptr<CallbackContext> callback_context_ =   // コールバック管理（shared_ptrで非同期コールバックと共有）
        std::make_shared<CallbackContext>();
    uint64_t first_video_timestamp_us_ = 0;                 // RTPタイムスタンプの基準時刻
};

// ============================================================================
// WebrtcStreamer — 公開API（Pimplパターンの委譲）
// 全てのメソッドは Impl に処理を委譲する
// ============================================================================

// コンストラクタ: Impl を生成
WebrtcStreamer::WebrtcStreamer()
    : impl_(std::make_unique<Impl>()) {}

// デストラクタ: デフォルト（Impl のデストラクタで Shutdown が呼ばれる）
WebrtcStreamer::~WebrtcStreamer() = default;

// WebRTCセッションを初期化
bool WebrtcStreamer::Initialize() {
    return impl_->Initialize();
}

// WebRTCセッションをシャットダウン
void WebrtcStreamer::Shutdown() {
    impl_->Shutdown(true);
}

// --- コールバック登録（全てImplに委譲） ---
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

// シグナリングメッセージ処理（SDP / ICE Candidate）
bool WebrtcStreamer::ProcessSignalingMessage(
    const std::string& message_type,
    const std::string& sdp_or_candidate,
    const std::string& mid) {
    return impl_->ProcessSignalingMessage(
        message_type, sdp_or_candidate, mid);
}

// H.264フレーム送信
bool WebrtcStreamer::PushVideoFrame(
    const uint8_t* data,
    size_t size,
    uint64_t timestamp_us) {
    return impl_->PushVideoFrame(data, size, timestamp_us);
}

// DataChannel経由でメッセージ送信
bool WebrtcStreamer::SendMessageOverDataChannel(
    const std::string& message) {
    return impl_->SendMessageOverDataChannel(message);
}

// 接続状態を取得
bool WebrtcStreamer::IsConnected() const {
    return impl_->IsConnected();
}
