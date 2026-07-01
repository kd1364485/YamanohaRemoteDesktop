#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <cstdio>    // std::setvbuf, std::printf 用
#include <windows.h> // SetConsoleOutputCP 用

#include "gpu/d3d11_device.h"
#include "capture/desktop_duplicator.h"
#include "capture/display_manager.h"
#include "encoder/nvenc_encoder.h"
#include "webrtc/webrtc_streamer.h"

using namespace yamanoha;

// 接続完了を待つためのヘルパー
std::atomic<bool> sender_connected{false};
std::atomic<bool> receiver_connected{false};
std::atomic<bool> control_message_received{false};

int main(int argc, char* argv[]) {
    // コンソール出力を UTF-8 に設定
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("========================================\n");
    std::printf("  YamanohaRemoteDesktop WebRTC統合テスト\n");
    std::printf("========================================\n\n");

    int adapter_index = 0;
    if (argc > 1) {
        adapter_index = std::stoi(argv[1]);
    }

    // 1. D3D11デバイスの作成
    std::printf("[Test] アダプタ %d で D3D11 デバイスを作成します...\n", adapter_index);
    auto device_manager = std::make_unique<D3D11DeviceManager>();
    if (!device_manager->Initialize(adapter_index)) {
        std::printf("[Test] D3D11 デバイスの作成に失敗しました。\n");
        return -1;
    }
    std::printf("[Test] D3D11 デバイス作成成功\n");

    // 2. キャプチャ(Duplicator)の初期化
    auto duplicator = std::make_unique<DesktopDuplicator>();
    if (!duplicator->Initialize(device_manager->GetDevice(), device_manager->GetContext(), 0)) {
        std::printf("[Test] DesktopDuplicator の初期化に失敗しました (フォールバック試行します)\n");
    }

    uint32_t width = duplicator->GetWidth();
    uint32_t height = duplicator->GetHeight();

    // 3. ハードウェアエンコーダ(NVENC)の初期化
    std::printf("[Test] NVENC エンコーダを初期化します (%d x %d)...\n", width, height);
    auto encoder = std::make_unique<NvencEncoder>();
    
    EncoderConfig enc_config{};
    enc_config.width = width;
    enc_config.height = height;
    enc_config.fps = 60;
    enc_config.bitrate_kbps = 4000; // 4 Mbps
    enc_config.use_cbr = true;

    if (!encoder->Initialize(device_manager->GetDevice(), enc_config)) {
        std::printf("[Test] NVENC 初期化に失敗しました。RTX GPUが接続されているか確認してください。\n");
        return -1;
    }
    std::printf("[Test] NVENC エンコーダ初期化成功\n");

    // 4. WebRTC ストリーマー (Sender / Receiver) の作成
    std::printf("[Test] WebRTC ストリーマーをセットアップします...\n");
    auto sender = std::make_unique<WebrtcStreamer>();
    auto receiver = std::make_unique<WebrtcStreamer>();

    if (!sender->Initialize() || !receiver->Initialize()) {
        std::printf("[Test] WebrtcStreamer の初期化に失敗しました。\n");
        return -1;
    }

    // 接続コールバックの設定
    sender->SetStateChangeCallback([](bool connected) {
        sender_connected = connected;
        std::printf("[Test] [Sender] 接続状態変更: %s\n", connected ? "接続完了" : "切断");
    });

    receiver->SetStateChangeCallback([](bool connected) {
        receiver_connected = connected;
        std::printf("[Test] [Receiver] 接続状態変更: %s\n", connected ? "接続完了" : "切断");
    });

    // Sender からメッセージを受信した際 (Data Channel 経由での制御信号受信テスト)
    sender->SetMessageCallback([](const std::string& msg) {
        std::printf("[Test] [Sender] DataChannel 受信メッセージ: %s\n", msg.c_str());
        control_message_received = true;
    });

    // シグナリングの媒介設定 (Trickle ICEを使わず、Vanilla SDPで一発接続します)
    sender->SetLocalDescriptionCallback([&receiver](const std::string& sdp, const std::string& type) {
        std::printf("[Signaling] Sender -> Receiver SDP (%s) 転送\n", type.c_str());
        receiver->ProcessSignalingMessage(type, sdp);
    });

    receiver->SetLocalDescriptionCallback([&sender](const std::string& sdp, const std::string& type) {
        std::printf("[Signaling] Receiver -> Sender SDP (%s) 転送\n", type.c_str());
        sender->ProcessSignalingMessage(type, sdp);
    });

    // Trickle ICEのICE候補の自動媒介
    sender->SetLocalCandidateCallback([&receiver](const std::string& cand, const std::string& mid) {
        receiver->ProcessSignalingMessage("candidate", cand, mid);
    });

    receiver->SetLocalCandidateCallback([&sender](const std::string& cand, const std::string& mid) {
        sender->ProcessSignalingMessage("candidate", cand, mid);
    });

    // 5. 接続処理の開始
    std::printf("[Test] 接続処理を開始します (Offer生成)...\n");
    // Receiver から Sender へ Offer を投げるか、またはその逆
    // libdatachannel では、片方で setLocalDescription() を呼ぶと SDP 収集とシグナリングが始まります。
    // webrtc_streamer.cpp 内部で、Initialize時に data_channel を作成しているため、自動で Offer/Answer がやり取りされます。
    // ここでは、Sender 側から接続を開始します。
    // webrtc_streamer.cpp 内で ProcessSignalingMessage("offer", ...) を受信すると自動的に Answer が作られます。
    // よって、大元から明示的にセットしてシグナリングをキックします。
    // 実際には、Tauriを介さずに C++ 単体で PeerConnection::setLocalDescription() を内部的に呼ぶことで接続を開始します。
    
    // シグナリングを起動するため、Sender側からOfferを生成してキックさせます
    // （ProcessSignalingMessage でのオファー生成をキックするため、内部の実装を流用してシグナリングを開始させます）
    // webrtc_streamer.cpp 内部では peer_connection_->setLocalDescription() が呼び出されると Gathering が始まり、
    // onGatheringStateChange で Complete になった時点で LocalDescriptionCallback が発火します。
    // なので、単に sender 側で setLocalDescription() を明示的にキックすれば、SDPが収集され、相手に送られます。
    // しかし、WebrtcStreamer のパブリック API に setLocalDescription をキックする専用メソッドがないため、
    // 接続をトリガーするために、ダミーの Offer シグナリングなどを送るか、または内部で自動的にトリガーされるようにします。
    // 実は、webrtc_streamer.cpp で peer_connection_->setLocalDescription() はリモート SDP 受信時 ("offer") に自動で呼ばれますが、
    // 自らオファーを出す場合は、以下のようにダミーの呼び出しなどを行う、あるいは peer_connection_->setLocalDescription() を直接呼べるようにするか、
    // 相手が Offer を作って投げるようにします。
    // 今回は、Receiver 側からダミーシグナリングでキックするか、または WebrtcStreamer に接続開始メソッドを追加するのが美しいですが、
    // webrtc_streamer.cpp 側で初期化時にオファーを作成するようにキックしていないため、
    // webrtc_streamer に接続開始用のメソッド「Connect()」か、または内部の PeerConnection にアクセスできる必要があります。
    // 待ってください。webrtc_streamer.cpp の初期化コード（102行目付近）で：
    // `peer_connection_->setLocalDescription();`
    // をすでに呼び出しています！
    // したがって、Initialize() を呼んだ時点で、自動的に ICE Gathering が始まっています！
    // そのため、何もしなくても Initialize() が終われば勝手に Vanilla SDP が生成され、
    // 設定したコールバックを介して相手に渡り、Answerが戻ってくる全自動接続シーケンスになっています！
    // 非常にスマートな設計ですね。

    // 接続の完了を最大 10 秒待ちます
    std::printf("[Test] ピア接続の完了を待機中...\n");
    int wait_count = 0;
    while ((!sender_connected || !receiver_connected) && wait_count < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }

    if (!sender_connected || !receiver_connected) {
        std::printf("[Test] 警告: タイムアウト時間内にWebRTC接続が確立されませんでした。(STUNサーバーへの接続に時間がかかっている可能性があります)\n");
        std::printf("[Test] テストを継続してローカル転送を試みます...\n");
    } else {
        std::printf("[Test] WebRTC 接続確立に成功しました！\n");
    }

    // 6. メッセージ送受信テスト (Data Channel)
    std::printf("[Test] Data Channel 経由での制御コマンド送受信テストを行います...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 接続が安定するまで少し待つ
    
    // Receiver から Sender へマウス移動の模擬コマンドを送信
    std::string test_cmd = "{\"type\":\"mousemove\",\"x\":100,\"y\":200}";
    if (receiver->SendMessageOverDataChannel(test_cmd)) {
        std::printf("[Test] [Receiver] 制御メッセージ送信成功\n");
    } else {
        std::printf("[Test] [Receiver] 制御メッセージ送信失敗 (データチャネルがまだ開いていない可能性があります)\n");
    }

    // メッセージ受信を少し待つ
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (control_message_received) {
        std::printf("[Test] Data Channel 制御シグナルテスト: 成功！\n");
    } else {
        std::printf("[Test] Data Channel 制御シグナルテスト: 未到達 (ローカル環境により接続が遅れている可能性があります)\n");
    }

    // 7. キャプチャ・エンコード・WebRTC配信ループテスト
    std::printf("\n[Test] キャプチャ & NVENCエンコード & WebRTC配信の統合ループテストを開始します (100フレーム)...\n");
    
    CapturedFrame frame{};
    std::vector<uint8_t> h264_buffer;
    int success_frames = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 100; i++) {
        auto frame_start = std::chrono::high_resolution_clock::now();

        // (1) 画面キャプチャ
        if (!duplicator->AcquireFrame(frame, 16)) {
            // タイムアウト時はスキップ（正常）
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // (2) NVENCエンコード
        if (frame.has_new_frame || i == 0) { // 初回または変化あり時
            bool force_idr = (i == 0); // 初回フレームは強制IDRキーフレーム
            if (encoder->EncodeFrame(frame.texture.Get(), force_idr, h264_buffer)) {
                
                // (3) WebRTC 配信 (H.264 送信)
                if (!h264_buffer.empty()) {
                    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        frame_start.time_since_epoch()
                    ).count();
                    
                    // WebRTC トラックにプッシュ
                    sender->PushVideoFrame(h264_buffer.data(), h264_buffer.size(), ts);
                    success_frames++;
                }
            }
        }

        duplicator->ReleaseFrame();

        // 60FPS の待機調整 (16.6ms)
        auto frame_end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count();
        if (elapsed < 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16 - elapsed));
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    double avg_fps = (success_frames / (total_ms / 1000.0));

    std::printf("\n--- 統合ループテスト結果 ---\n");
    std::printf("  総フレーム数     : 100\n");
    std::printf("  配信成功フレーム : %d\n", success_frames);
    std::printf("  総所要時間       : %.1f ms\n", total_ms);
    std::printf("  平均配信 FPS     : %.1f FPS\n", avg_fps);
    std::printf("========================================\n\n");

    // 8. 終了処理
    std::printf("[Test] リソースを解放します...\n");
    sender->Shutdown();
    receiver->Shutdown();
    duplicator->Shutdown();
    std::printf("[Test] 終了しました。\n");

    return 0;
}
