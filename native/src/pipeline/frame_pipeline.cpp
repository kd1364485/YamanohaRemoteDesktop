// ============================================================================
// frame_pipeline.cpp
// 非同期フレームパイプラインクラスの実装
// ============================================================================

#include "pipeline/frame_pipeline.h"

#include <cstdio>
#include <chrono>

namespace yamanoha {

namespace {
    // エンコード用テクスチャプールサイズ（トリプルバッファリング + 1）
    constexpr size_t kEncodeTexturePoolSize = 4;
    // EncoderThread のポーリング間隔
    constexpr uint64_t kEncoderPollTimeoutUs = 5000; // 5ms
    // SenderThread の wait_for タイムアウト
    constexpr int kSenderPollTimeoutMs = 5;
}

FramePipeline::FramePipeline() = default;

FramePipeline::~FramePipeline() {
    Stop();
}

// ---------------------------------------------------------------------------
// パイプライン開始
// ---------------------------------------------------------------------------
bool FramePipeline::Start(const Display& display, int fps, int bitrate_kbps) {
    Stop();

    fps_ = fps;

    // 1. キャプチャエンジン開始
    capture_engine_ = std::make_unique<CaptureEngine>();
    if (!capture_engine_->Start(display)) {
        std::printf("[FramePipeline] CaptureEngine 開始失敗\n");
        capture_engine_.reset();
        return false;
    }

    const uint32_t width = capture_engine_->GetWidth();
    const uint32_t height = capture_engine_->GetHeight();

    // 2. エンコード用デバイス作成（NVIDIA アダプタを優先）
    encode_device_manager_ = std::make_unique<D3D11DeviceManager>();
    int encode_adapter = FindNvidiaAdapterIndex();
    if (encode_adapter < 0) {
        encode_adapter = display.adapter_index;
        std::printf("[FramePipeline] NVIDIA アダプタが見つからないため、キャプチャアダプタでエンコードします\n");
    }

    if (!encode_device_manager_->Initialize(encode_adapter)) {
        std::printf("[FramePipeline] エンコード用 D3D11 デバイス初期化失敗。キャプチャアダプタにフォールバックします。\n");
        encode_device_manager_.reset();
        encode_device_manager_ = std::make_unique<D3D11DeviceManager>();
        if (!encode_device_manager_->Initialize(display.adapter_index)) {
            std::printf("[FramePipeline] フォールバック D3D11 デバイスも初期化失敗\n");
            Stop();
            return false;
        }
    }

    // 3. CPU コピー用ステージングテクスチャ（キャプチャデバイス上）
    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = width;
    staging_desc.Height = height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    HRESULT hr = capture_engine_->GetDevice()->CreateTexture2D(&staging_desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        std::printf("[FramePipeline] ステージングテクスチャ作成失敗: 0x%08lX\n", hr);
        Stop();
        return false;
    }

    // 4. エンコード用テクスチャプール（エンコードデバイス上）
    D3D11_TEXTURE2D_DESC encode_desc{};
    encode_desc.Width = width;
    encode_desc.Height = height;
    encode_desc.MipLevels = 1;
    encode_desc.ArraySize = 1;
    encode_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    encode_desc.SampleDesc.Count = 1;
    encode_desc.Usage = D3D11_USAGE_DEFAULT;
    encode_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    encode_desc.MiscFlags = 0;

    for (size_t i = 0; i < kEncodeTexturePoolSize; ++i) {
        ComPtr<ID3D11Texture2D> texture;
        hr = encode_device_manager_->GetDevice()->CreateTexture2D(&encode_desc, nullptr, &texture);
        if (FAILED(hr)) {
            std::printf("[FramePipeline] エンコード用テクスチャ作成失敗: 0x%08lX\n", hr);
            Stop();
            return false;
        }
        encode_textures_.push_back(texture);
        free_encode_textures_.push_back(texture.Get());
    }

    // 5. NVENC エンコーダ初期化
    encoder_ = std::make_unique<NvencEncoder>();
    EncoderConfig enc_config{};
    enc_config.width = width;
    enc_config.height = height;
    enc_config.fps = static_cast<uint32_t>(fps_);
    enc_config.bitrate_kbps = static_cast<uint32_t>(bitrate_kbps);
    enc_config.max_bitrate_kbps = bitrate_kbps * 2;
    enc_config.use_cbr = true;
    enc_config.zero_latency = true;
    enc_config.preset = 1;
    enc_config.gop_size = static_cast<uint32_t>(fps_);

    if (!encoder_->Initialize(encode_device_manager_->GetDevice(), enc_config)) {
        std::printf("[FramePipeline] NVENC エンコーダ初期化失敗\n");
        Stop();
        return false;
    }

    // 6. WebRTC ストリーマー作成（コールバック設定後に Initialize() すること）
    streamer_ = std::make_unique<WebrtcStreamer>();

    std::printf("[FramePipeline] 非同期パイプラインリソース作成完了: %u x %u, %d FPS, %zu テクスチャプール\n",
                width, height, fps_, kEncodeTexturePoolSize);
    std::fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// 各スレッド開始
// ---------------------------------------------------------------------------
void FramePipeline::StartThreads() {
    if (running_) {
        return;
    }

    running_ = true;
    submit_stop_ = false;
    bitstream_stop_ = false;

    capture_thread_ = std::thread(&FramePipeline::CaptureThreadFunc, this);
    encoder_thread_ = std::thread(&FramePipeline::EncoderThreadFunc, this);
    sender_thread_ = std::thread(&FramePipeline::SenderThreadFunc, this);

    std::printf("[FramePipeline] 非同期パイプラインスレッド開始\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// パイプライン停止
// ---------------------------------------------------------------------------
void FramePipeline::Stop() {
    running_ = false;

    // キューに停止通知を送る
    {
        std::lock_guard<std::mutex> lock(submit_mutex_);
        submit_stop_ = true;
        submit_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(bitstream_mutex_);
        bitstream_stop_ = true;
        bitstream_cv_.notify_all();
    }

    // スレッド終了を待つ
    if (capture_thread_.joinable()) capture_thread_.join();
    if (encoder_thread_.joinable()) encoder_thread_.join();
    if (sender_thread_.joinable()) sender_thread_.join();

    // 未処理の submit_queue をクリアし、テクスチャをプールに戻す
    {
        std::lock_guard<std::mutex> lock(submit_mutex_);
        std::lock_guard<std::mutex> pool_lock(texture_pool_mutex_);
        for (auto& frame : submit_queue_) {
            if (frame.texture) {
                free_encode_textures_.push_back(frame.texture);
            }
        }
        submit_queue_.clear();
    }

    // 未送信のビットストリームキューをクリア
    {
        std::lock_guard<std::mutex> lock(bitstream_mutex_);
        bitstream_queue_.clear();
    }

    // エンコーダのシャットダウン
    if (encoder_) {
        std::vector<uint8_t> bitstream;
        encoder_->Flush(bitstream);
        encoder_->Shutdown();
        encoder_.reset();
    }

    streamer_.reset();
    encode_device_manager_.reset();

    staging_texture_.Reset();
    encode_textures_.clear();
    {
        std::lock_guard<std::mutex> lock(texture_pool_mutex_);
        free_encode_textures_.clear();
    }

    capture_engine_.reset();

    fps_ = 0;

    std::printf("[FramePipeline] 停止しました\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// キャプチャスレッド
// ---------------------------------------------------------------------------
void FramePipeline::CaptureThreadFunc() {
    std::printf("[FramePipeline] CaptureThread 開始\n");
    std::fflush(stdout);

    bool is_first_frame = true;

    while (running_) {
        auto frame_start = std::chrono::high_resolution_clock::now();

        // フレーム取得
        CapturedFrame frame{};
        if (!capture_engine_->AcquireFrame(frame, 0)) {
            // 取得失敗時は短くスリープして再試行
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (!frame.has_new_frame && !is_first_frame) {
            capture_engine_->ReleaseFrame();
            continue;
        }

        // エンコード用テクスチャをプールから取得
        ID3D11Texture2D* encode_texture = nullptr;
        {
            std::lock_guard<std::mutex> lock(texture_pool_mutex_);
            if (free_encode_textures_.empty()) {
                std::printf("[FramePipeline] エンコードテクスチャプール枯渇: フレームをドロップ\n");
                std::fflush(stdout);
                capture_engine_->ReleaseFrame();
                // エンコーダが追いつくまで少し待つ
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            encode_texture = free_encode_textures_.back();
            free_encode_textures_.pop_back();
        }

        // エンコード用テクスチャに CPU コピー
        bool copy_ok = CopyToEncodeTexture(frame.texture.Get(), encode_texture);

        // 取得フレームを解放
        capture_engine_->ReleaseFrame();

        if (!copy_ok) {
            std::printf("[FramePipeline] CPU コピー失敗\n");
            std::fflush(stdout);
            {
                std::lock_guard<std::mutex> lock(texture_pool_mutex_);
                free_encode_textures_.push_back(encode_texture);
            }
            continue;
        }

        // エンコーダに非同期投入
        const bool force_idr = is_first_frame;
        int frame_id = encoder_->SubmitFrame(encode_texture, force_idr);
        if (frame_id < 0) {
            std::printf("[FramePipeline] SubmitFrame 失敗\n");
            std::fflush(stdout);
            {
                std::lock_guard<std::mutex> lock(texture_pool_mutex_);
                free_encode_textures_.push_back(encode_texture);
            }
            continue;
        }

        is_first_frame = false;

        // タイムスタンプを作成してエンコーダキューに投入
        uint64_t timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            frame_start.time_since_epoch()
        ).count();

        {
            std::lock_guard<std::mutex> lock(submit_mutex_);
            submit_queue_.push_back({frame_id, encode_texture, timestamp_us});
            submit_cv_.notify_one();
        }

        // フレームレート制御
        auto frame_end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count();
        const int sleep_ms = (fps_ > 0) ? (1000 / fps_) : 33;
        if (elapsed < sleep_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms - elapsed));
        }
    }

    std::printf("[FramePipeline] CaptureThread 終了\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// キャプチャテクスチャをエンコード用テクスチャに CPU コピー
// ---------------------------------------------------------------------------
bool FramePipeline::CopyToEncodeTexture(ID3D11Texture2D* src, ID3D11Texture2D* dst) {
    if (!src || !dst) return false;

    auto capture_ctx = capture_engine_->GetContext();
    auto encode_ctx = encode_device_manager_->GetContext();
    if (!capture_ctx || !encode_ctx) return false;

    capture_ctx->CopyResource(staging_texture_.Get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = capture_ctx->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::printf("[FramePipeline] ステージングテクスチャの Map に失敗: 0x%08lX\n", hr);
        return false;
    }

    encode_ctx->UpdateSubresource(dst, 0, nullptr, mapped.pData, mapped.RowPitch, 0);
    capture_ctx->Unmap(staging_texture_.Get(), 0);

    return true;
}

// ---------------------------------------------------------------------------
// エンコーダスレッド
// ---------------------------------------------------------------------------
void FramePipeline::EncoderThreadFunc() {
    std::printf("[FramePipeline] EncoderThread 開始\n");
    std::fflush(stdout);

    while (running_) {
        // 完了したフレームを取得
        std::vector<uint8_t> bitstream;
        ID3D11Texture2D* completed_texture = nullptr;
        int completed_frame_id = 0;

        bool got = encoder_->GetCompletedFrame(bitstream, &completed_texture, &completed_frame_id, kEncoderPollTimeoutUs);
        if (!got) {
            continue;
        }

        // 対応するタイムスタンプを探す
        uint64_t timestamp_us = 0;
        {
            std::lock_guard<std::mutex> lock(submit_mutex_);
            for (auto it = submit_queue_.begin(); it != submit_queue_.end(); ++it) {
                if (it->frame_id == completed_frame_id) {
                    timestamp_us = it->timestamp_us;
                    submit_queue_.erase(it);
                    break;
                }
            }
        }

        // テクスチャをプールに戻す
        {
            std::lock_guard<std::mutex> lock(texture_pool_mutex_);
            if (completed_texture) {
                free_encode_textures_.push_back(completed_texture);
            }
        }

        // 送信キューに投入
        if (!bitstream.empty()) {
            EncodedFrame encoded{};
            encoded.data = std::move(bitstream);
            encoded.timestamp_us = timestamp_us;
            {
                std::lock_guard<std::mutex> lock(bitstream_mutex_);
                bitstream_queue_.push_back(std::move(encoded));
                bitstream_cv_.notify_one();
            }
        }
    }

    std::printf("[FramePipeline] EncoderThread 終了\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 送信スレッド
// ---------------------------------------------------------------------------
void FramePipeline::SenderThreadFunc() {
    std::printf("[FramePipeline] SenderThread 開始\n");
    std::fflush(stdout);

    while (running_) {
        EncodedFrame frame;
        bool got = false;
        {
            std::unique_lock<std::mutex> lock(bitstream_mutex_);
            got = bitstream_cv_.wait_for(lock, std::chrono::milliseconds(kSenderPollTimeoutMs), [this] {
                return !bitstream_queue_.empty() || bitstream_stop_;
            });
            if (!bitstream_queue_.empty()) {
                frame = std::move(bitstream_queue_.front());
                bitstream_queue_.pop_front();
            }
        }

        if (!got || frame.data.empty()) {
            continue;
        }

        streamer_->PushVideoFrame(frame.data.data(), frame.data.size(), frame.timestamp_us);
    }

    std::printf("[FramePipeline] SenderThread 終了\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// NVIDIA アダプタのインデックスを検索
// ---------------------------------------------------------------------------
int FramePipeline::FindNvidiaAdapterIndex() const {
    auto adapters = D3D11DeviceManager::EnumerateAdapters();
    for (const auto& adapter : adapters) {
        if (adapter.vendor_id == 0x10DE) { // NVIDIA Vendor ID
            std::printf("[FramePipeline] NVIDIA アダプタ検出: %ls (index=%d, VRAM=%llu MB)\n",
                        adapter.name.c_str(), adapter.index,
                        adapter.dedicated_vram / (1024 * 1024));
            return adapter.index;
        }
    }
    std::printf("[FramePipeline] NVIDIA アダプタが検出されませんでした\n");
    return -1;
}

} // namespace yamanoha
