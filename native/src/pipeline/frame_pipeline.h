// ============================================================================
// frame_pipeline.h
// 非同期フレームパイプラインクラス
//
// 設計方針:
// - CaptureEngine からフレームを取得し、別スレッドでコピー・エンコード・送信を行う。
// - 3つの専用スレッドで構成:
//   1. CaptureThread: フレーム取得 + CPUコピー + SubmitFrame
//   2. EncoderThread: NVENC 完了フレームの取得
//   3. SenderThread: WebRTC 送信
// - CaptureEngine / NvencEncoder / WebrtcStreamer の責務を分離する。
// ============================================================================
#pragma once

#include "capture/capture_engine.h"
#include "capture/display_manager.h"
#include "encoder/nvenc_encoder.h"
#include "gpu/d3d11_device.h"
#include "webrtc/webrtc_streamer.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace yamanoha {

class FramePipeline {
public:
    FramePipeline();
    ~FramePipeline();

    // コピー禁止・ムーブ許可
    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;
    FramePipeline(FramePipeline&&) noexcept = default;
    FramePipeline& operator=(FramePipeline&&) noexcept = default;

    /// @brief パイプラインのリソースを作成する
    /// @param display キャプチャ対象の Display 情報
    /// @param fps 目標フレームレート
    /// @param bitrate_kbps 目標ビットレート (kbps)
    /// @return 成功時 true
    /// @details このメソッドは WebrtcStreamer の初期化までは行わない。
    ///          呼び出し側は GetStreamer() で取得したストリーマーにコールバックを
    ///          設定し Initialize() した後、StartThreads() を呼び出すこと。
    bool Start(const Display& display, int fps, int bitrate_kbps);

    /// @brief キャプチャ・エンコード・送信スレッドを開始する
    /// @details Start() 成功後、WebrtcStreamer の Initialize() 完了後に呼び出すこと
    void StartThreads();

    /// @brief パイプラインを停止し、リソースを解放する
    void Stop();

    /// @brief パイプラインが実行中かどうか
    bool IsRunning() const { return running_.load(); }

    /// @brief WebRTC ストリーマーへのアクセスを提供する
    /// @details core.cpp 側でシグナリングコールバックを設定するために使用する
    WebrtcStreamer* GetStreamer() const { return streamer_.get(); }

    /// @brief キャプチャ幅を取得する
    uint32_t GetWidth() const { return capture_engine_ ? capture_engine_->GetWidth() : 0; }

    /// @brief キャプチャ高さを取得する
    uint32_t GetHeight() const { return capture_engine_ ? capture_engine_->GetHeight() : 0; }

private:
    // 各スレッドのメインループ
    void CaptureThreadFunc();
    void EncoderThreadFunc();
    void SenderThreadFunc();

    // キャプチャされたテクスチャをエンコード用テクスチャに CPU コピーする
    bool CopyToEncodeTexture(ID3D11Texture2D* src, ID3D11Texture2D* dst);

    // NVIDIA アダプタのインデックスを検索
    int FindNvidiaAdapterIndex() const;

    // キャプチャエンジン
    std::unique_ptr<CaptureEngine> capture_engine_;

    // エンコード用デバイスとエンコーダ
    std::unique_ptr<D3D11DeviceManager> encode_device_manager_;
    std::unique_ptr<NvencEncoder> encoder_;

    // WebRTC ストリーマー
    std::unique_ptr<WebrtcStreamer> streamer_;

    // CPU コピー用ステージングテクスチャ（キャプチャデバイス上）
    ComPtr<ID3D11Texture2D> staging_texture_;

    // エンコード用テクスチャプール（エンコードデバイス上）
    std::vector<ComPtr<ID3D11Texture2D>> encode_textures_;
    std::vector<ID3D11Texture2D*> free_encode_textures_;
    std::mutex texture_pool_mutex_;

    // CaptureThread → EncoderThread 間のフレーム情報キュー
    struct SubmittedFrame {
        int frame_id;
        ID3D11Texture2D* texture;
        uint64_t timestamp_us;
    };
    std::mutex submit_mutex_;
    std::condition_variable submit_cv_;
    std::deque<SubmittedFrame> submit_queue_;
    bool submit_stop_ = false;

    // EncoderThread → SenderThread 間のビットストリームキュー
    struct EncodedFrame {
        std::vector<uint8_t> data;
        uint64_t timestamp_us;
    };
    std::mutex bitstream_mutex_;
    std::condition_variable bitstream_cv_;
    std::deque<EncodedFrame> bitstream_queue_;
    bool bitstream_stop_ = false;

    // スレッド
    std::thread capture_thread_;
    std::thread encoder_thread_;
    std::thread sender_thread_;

    // 状態
    std::atomic<bool> running_{false};
    int fps_ = 0;
};

} // namespace yamanoha
