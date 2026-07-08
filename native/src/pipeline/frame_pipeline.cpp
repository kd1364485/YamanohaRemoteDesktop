// ============================================================================
// frame_pipeline.cpp — キャプチャ→エンコード→送信の3スレッドパイプライン実装
// ============================================================================

#include "pipeline/frame_pipeline.h"
#include "webrtc/webrtc_streamer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace yamanoha {

namespace {

// --- パイプライン動作パラメータ ---
constexpr size_t kEncodeTexturePoolSize = 4;      // エンコード用テクスチャプールサイズ
constexpr size_t kMaximumFramesInFlight = 3;       // 処理中フレームの上限（バックログ防止）
constexpr uint64_t kEncoderPollTimeoutUs = 5000;   // エンコーダ完了ポーリングタイムアウト（μs）
constexpr auto kSenderWaitTimeout = std::chrono::milliseconds(5);  // 送信キューのポップタイムアウト
constexpr auto kIdleWait = std::chrono::milliseconds(1);           // アイドル時のスリープ間隔
constexpr auto kPeriodicKeyFrameInterval = std::chrono::seconds(3); // 定期キーフレーム間隔
// 動的ビットレート制御の有効/無効
// 以前は非同期エンコード時に nvEncReconfigureEncoder が完了イベントを紛失する問題があったが、
// 現在は同期モード（kUseAsyncEncoding=false）のため安全に再設定可能
constexpr bool kEnableDynamicBitrate = true;

// time_point をマイクロ秒の uint64_t に変換するユーティリティ
uint64_t TimestampMicroseconds(
    std::chrono::steady_clock::time_point time_point) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            time_point.time_since_epoch()).count());
}

} // namespace

// デストラクタ: 確実にスレッド停止・リソース解放を行う
FramePipeline::~FramePipeline() {
    Stop();
}

// パイプラインの準備を行う（スレッド起動は StartThreads() で別途呼ぶ）
// 戻り値: 成功時 true。失敗時は内部リソースを解放して false を返す
bool FramePipeline::Start(
    const Display& display,
    int fps,
    int bitrate_kbps,
    WebrtcStreamer* streamer) {
    Stop();  // 既存のセッションがあれば停止
    streamer_ = streamer;
    if (fps <= 0 || bitrate_kbps <= 0) {
        std::printf("[FramePipeline] invalid fps or bitrate\n");
        return false;
    }

    // --- パラメータ初期化 ---
    fps_ = fps;
    current_bitrate_kbps_ = static_cast<uint32_t>(bitrate_kbps);

    // 全カウンタをリセット & 最初のフレームをキーフレームに強制
    // memory_order_release: これより前の全操作が可視化されてからストアする
    force_key_frame_.store(true, std::memory_order_release);
    captured_count_.store(0, std::memory_order_release);
    submitted_count_.store(0, std::memory_order_release);
    encoded_count_.store(0, std::memory_order_release);
    sent_count_.store(0, std::memory_order_release);
    send_failure_count_.store(0, std::memory_order_release);

    // --- 動的ビットレート制御の初期設定 ---
    // 最大ビットレート = 初期ビットレート
    // 最小ビットレート = 500kbps と初期ビットレートの小さい方
    //   ※ (std::min) とカッコで囲むのは Windows ヘッダの min/max マクロとの衝突を防ぐため
    AdaptiveBitrateController::Config bitrate_config;
    bitrate_config.maximum_kbps = current_bitrate_kbps_;
    bitrate_config.minimum_kbps =
        (std::min)(500u, current_bitrate_kbps_);
    bitrate_controller_.Reset(current_bitrate_kbps_, bitrate_config);

    // --- 4段階の初期化 ---
    // [1] キャプチャデバイス + DesktopDuplicator
    if (!InitializeCapture(display)) {
        ReleaseResources();
        return false;
    }

    // キャプチャの実際の解像度を取得（Display 情報より正確）
    const uint32_t width = capture_engine_->GetWidth();
    const uint32_t height = capture_engine_->GetHeight();

    // [2] エンコードデバイス（NVIDIA優先）
    // [3] クロスアダプタ転送用テクスチャプール
    // [4] NVENC → MF フォールバックでエンコーダ初期化
    if (!InitializeEncoderDevice(display) ||
        !CreateTransferTextures(width, height) ||
        !InitializeEncoder(width, height, bitrate_kbps)) {
        ReleaseResources();
        return false;
    }

    encoded_frames_.Reset();  // 送信キューをクリア
    prepared_ = true;         // 準備完了フラグ（StartThreads でチェック）

    std::printf(
        "[FramePipeline] prepared: %u x %u, %d FPS, %u kbps\n",
        width,
        height,
        fps_,
        current_bitrate_kbps_);
    std::fflush(stdout);
    return true;
}

// ワーカースレッドを起動する（Start() の後に呼ぶ）
// prepared_ が false なら起動しない。既に起動中なら何もしない
void FramePipeline::StartThreads() {
    if (!prepared_ || running_.exchange(true)) {
        return;
    }

    encoded_frames_.Reset();
    // 3つのワーカースレッドを起動: キャプチャ → エンコード → 送信
    capture_thread_ = std::thread(&FramePipeline::CaptureLoop, this);
    encoder_thread_ = std::thread(&FramePipeline::EncoderLoop, this);
    sender_thread_ = std::thread(&FramePipeline::SenderLoop, this);
    std::printf("[FramePipeline] worker threads started\n");
    std::fflush(stdout);
}

// パイプラインを停止し、全スレッドの join とリソース解放を行う
void FramePipeline::Stop() {
    const bool had_resources =
        prepared_ || capture_engine_ || encoder_ || streamer_;

    if (had_resources) {
        std::printf("[FramePipeline] stopping worker threads...\n");
        std::fflush(stdout);
    }

    // [1] running_ フラグを false にして全スレッドに終了を通知
    running_.store(false, std::memory_order_release);
    // EncoderLoop が submitted_cv_ で待機している可能性があるので起床
    submitted_cv_.notify_all();
    // SenderLoop が encoded_frames_ で待機している可能性があるので停止通知
    encoded_frames_.Stop();

    // [2] 全スレッドの join（確実に終了を待つ）
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    if (sender_thread_.joinable()) {
        sender_thread_.join();
    }

    // [3] エンコーダの残りフレームをフラッシュ
    if (encoder_) {
        std::vector<uint8_t> ignored;
        encoder_->Flush(ignored);
    }

    // [4] 送信キューをクリアしてリソース解放
    encoded_frames_.Clear();
    if (had_resources) {
        std::printf("[FramePipeline] releasing resources...\n");
        std::fflush(stdout);
    }
    ReleaseResources();

    if (had_resources) {
        std::printf("[FramePipeline] stopped\n");
        std::fflush(stdout);
    }
}

// キャプチャ解像度の幅を返す（未初期化時は 0）
uint32_t FramePipeline::GetWidth() const {
    return capture_engine_ ? capture_engine_->GetWidth() : 0;
}

// キャプチャ解像度の高さを返す（未初期化時は 0）
uint32_t FramePipeline::GetHeight() const {
    return capture_engine_ ? capture_engine_->GetHeight() : 0;
}

// [1] キャプチャエンジンを初期化（キャプチャ用GPU + DesktopDuplicator）
bool FramePipeline::InitializeCapture(const Display& display) {
    capture_engine_ = std::make_unique<CaptureEngine>();
    if (capture_engine_->Start(display)) {
        return true;
    }

    std::printf("[FramePipeline] capture initialization failed\n");
    capture_engine_.reset();
    return false;
}

// [2] エンコード用D3D11デバイスを初期化（NVIDIAアダプタを優先）
// ハイブリッドGPU環境（Intel+NVIDIA等）でNVENCを使うため、
// キャプチャアダプタとは別にエンコード用アダプタを選択する
bool FramePipeline::InitializeEncoderDevice(const Display& display) {
    encode_device_manager_ = std::make_unique<D3D11DeviceManager>();

    // NVIDIAアダプタ（vendor_id=0x10DE）を優先検索
    int adapter_index = FindNvidiaAdapterIndex();
    if (adapter_index < 0) {
        // NVIDIAが見つからない場合はキャプチャと同じアダプタを使用
        adapter_index = display.adapter_index;
    }

    if (encode_device_manager_->Initialize(adapter_index)) {
        return true;
    }

    // NVIDIAアダプタで失敗 & キャプチャアダプタと同じ場合はフォールバック不可
    if (adapter_index == display.adapter_index) {
        return false;
    }

    // NVIDIA失敗 → キャプチャアダプタにフォールバック
    std::printf(
        "[FramePipeline] NVIDIA device failed; using capture adapter\n");
    encode_device_manager_ = std::make_unique<D3D11DeviceManager>();
    return encode_device_manager_->Initialize(display.adapter_index);
}

// [3] エンコード用テクスチャプールを作成（エンコードGPU上）
// クロスアダプタ環境では、キャプチャGPUからCPU経由でこのテクスチャにデータを転送する
// プールサイズ分のテクスチャを事前作成し、AcquireEncodeTexture/ReturnEncodeTexture で再利用
bool FramePipeline::CreateTransferTextures(
    uint32_t width,
    uint32_t height) {
    D3D11_TEXTURE2D_DESC encode_description{};
    encode_description.Width = width;
    encode_description.Height = height;
    encode_description.MipLevels = 1;
    encode_description.ArraySize = 1;
    encode_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // BGRA8
    encode_description.SampleDesc.Count = 1;
    encode_description.Usage = D3D11_USAGE_DEFAULT;
    encode_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    for (size_t index = 0; index < kEncodeTexturePoolSize; ++index) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        HRESULT result = encode_device_manager_->GetDevice()->CreateTexture2D(
            &encode_description, nullptr, &texture);
        if (FAILED(result)) {
            std::printf(
                "[FramePipeline] encode texture creation failed: 0x%08lX\n",
                result);
            return false;
        }
        // free_encode_textures_ に生ポインタ、encode_textures_ に ComPtr を保持
        // ComPtr が所有権を持ち、生ポインタはプール管理用
        free_encode_textures_.push_back(texture.Get());
        encode_textures_.push_back(std::move(texture));
    }
    return true;
}

// [4] エンコーダを初期化（NVENC優先 → Media Foundation フォールバック）
bool FramePipeline::InitializeEncoder(
    uint32_t width,
    uint32_t height,
    int bitrate_kbps) {
    // リモートデスクトップ最適化のエンコード設定
    EncoderConfig config{};
    config.width = width;
    config.height = height;
    config.fps = static_cast<uint32_t>(fps_);
    config.bitrate_kbps = static_cast<uint32_t>(bitrate_kbps);
    config.max_bitrate_kbps = config.bitrate_kbps;
    config.use_cbr = true;        // 定数ビットレート（帯域予測しやすい）
    config.zero_latency = true;   // ゼロレイテンシ（超低遅延）
    config.preset = 1;            // P1: 最高速/最低遅延
    config.gop_size = config.fps; // GOP = 1秒（fpsフレームごとにキーフレーム）

    // 1. NVENC (NVIDIA GPU) を優先的に試行
    auto nvenc = std::make_unique<NvencEncoder>();
    if (nvenc->Initialize(encode_device_manager_->GetDevice(), config)) {
        encoder_ = std::move(nvenc);
        return true;
    }

    std::printf("[FramePipeline] NVENC initialization failed, trying Media Foundation encoder\n");
    nvenc.reset();

    // 2. Media Foundation (Intel QuickSync / AMD AMF / ソフトウェア) にフォールバック
    auto mf = std::make_unique<MfEncoder>();
    if (mf->Initialize(encode_device_manager_->GetDevice(), config)) {
        encoder_ = std::move(mf);
        return true;
    }

    std::printf("[FramePipeline] Media Foundation encoder initialization failed\n");
    encoder_.reset();
    return false;
}

// 全リソースを解放する（Stop() と Start() 失敗時から呼ばれる）
void FramePipeline::ReleaseResources() {
    prepared_ = false;
    streamer_ = nullptr;  // 所有権はないのでポインタをクリアするだけ

    if (encoder_) {
        encoder_->Shutdown();
        encoder_.reset();
    }

    // submitted_frames_ に残っているテクスチャをプールに戻してからクリア
    ClearSubmittedFrames();
    staging_texture_.Reset();  // CPU転送用ステージングテクスチャを解放

    // エンコード用テクスチャプールを解放
    {
        std::lock_guard<std::mutex> lock(texture_pool_mutex_);
        free_encode_textures_.clear();
        encode_textures_.clear();
    }

    encode_device_manager_.reset();  // エンコード用D3D11デバイスを解放
    capture_engine_.reset();         // キャプチャエンジンを解放
    fps_ = 0;
    current_bitrate_kbps_ = 0;
}

// ============================================================================
// CaptureLoop — キャプチャスレッドのメインループ
// Desktop Duplication からフレームを取得し、カーソルを合成してエンコーダに投入する
// ============================================================================
void FramePipeline::CaptureLoop() {
    bool first_frame = true;  // 接続後の最初のフレームをキーフレームにするためのフラグ
    auto last_keyframe_time = std::chrono::steady_clock::now();  // 最後のキーフレーム時刻

    while (running_.load(std::memory_order_acquire)) {
        const auto frame_started = std::chrono::steady_clock::now();

        // WebRTC接続が確立していない場合は待機
        // first_frame をリセットして再接続時にキーフレームを送る
        if (!streamer_ || !streamer_->IsConnected()) {
            first_frame = true;
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        // 動的ビットレートの推奨値があれば適用
        ApplyBitrateRecommendation();

        // バックログ（処理遅延）がある場合はスキップして遅延が蓄積しないようにする
        if (IsBacklogged()) {
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        // Desktop Duplication からフレームを取得（タイムアウト0 = 即座にリターン）
        CapturedFrame captured_frame{};
        if (!capture_engine_->AcquireFrame(captured_frame, 0)) {
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        // 周期キーフレームが必要かどうかを先に判定
        const bool periodic_keyframe_due =
            (frame_started - last_keyframe_time >= kPeriodicKeyFrameInterval);

        // 画面に変化がなく、初回でもなく、周期キーフレーム也不要ならスキップ
        if (!captured_frame.has_new_frame && !first_frame && !periodic_keyframe_due) {
            capture_engine_->ReleaseFrame();
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        // キャッシュテクスチャがない場合はスキップ（初回やACCESS_LOST後）
        if (!captured_frame.texture) {
            capture_engine_->ReleaseFrame();
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        // エンコード用テクスチャをプールから取得
        ID3D11Texture2D* encode_texture = AcquireEncodeTexture();
        if (!encode_texture) {
            // プールが枯渇している場合はスキップ
            capture_engine_->ReleaseFrame();
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        // クロスアダプタコピー: キャプチャGPU → CPU（ステージング） → カーソル合成 → エンコードGPU
        const bool copied = CopyToEncodeTexture(
            captured_frame.texture.Get(), encode_texture);
        capture_engine_->ReleaseFrame();  // Desktop Duplication のフレームを解放
        if (!copied) {
            ReturnEncodeTexture(encode_texture);  // テクスチャをプールに戻す
            continue;
        }
        captured_count_.fetch_add(1, std::memory_order_relaxed);

        // キーフレーム強制の判定:
        // - first_frame: 接続後の最初のフレーム
        // - force_key_frame_: 外部からの要求（RequestKeyFrame）
        // - periodic_keyframe_due: 3秒ごとの定期キーフレーム
        const bool force_key_frame =
            first_frame ||
            force_key_frame_.exchange(false, std::memory_order_acq_rel) ||
            (frame_started - last_keyframe_time >= kPeriodicKeyFrameInterval);
        const uint64_t timestamp_us =
            TimestampMicroseconds(frame_started);

        // エンコーダにフレームを投入
        // ※ submitted_mutex_ を保持したまま SubmitFrame を呼ぶと
        //    同期モードで EncoderLoop がドレインできずデッドロックするため注意
        const int frame_id = encoder_->SubmitFrame(
            encode_texture, force_key_frame);
        if (frame_id < 0) {
            // エンコード失敗: テクスチャをプールに戻して再試行を要求
            ReturnEncodeTexture(encode_texture);
            if (force_key_frame) {
                RequestKeyFrame();
            }
            continue;
        }

        if (force_key_frame) {
            last_keyframe_time = frame_started;
            std::printf("[FramePipeline] IDR keyframe sent (frame_id=%d)\n",
                        frame_id);
            std::fflush(stdout);
        }

        // submitted_frames_ に登録（エンコード完了時にテクスチャを回収するため）
        {
            std::lock_guard<std::mutex> lock(submitted_mutex_);
            submitted_frames_.emplace(
                frame_id,
                SubmittedFrame{encode_texture, timestamp_us});
        }
        submitted_count_.fetch_add(1, std::memory_order_relaxed);
        // EncoderLoop に通知（待機中なら起床）
        submitted_cv_.notify_one();

        first_frame = false;

        // FPS制御: フレーム処理時間がフレーム間隔より短ければ残り時間スリープ
        const auto elapsed = std::chrono::steady_clock::now() - frame_started;
        const auto frame_interval =
            std::chrono::microseconds(1000000 / fps_);
        if (elapsed < frame_interval) {
            std::this_thread::sleep_for(frame_interval - elapsed);
        }
    }
}

// ============================================================================
// EncoderLoop — エンコードスレッドのメインループ
// CaptureLoop から投入されたフレームのエンコード完了を待ち、H.264ビットストリームを取得する
// ============================================================================
void FramePipeline::EncoderLoop() {
    while (running_.load(std::memory_order_acquire)) {
        // submitted_frames_ にフレームが追加されるまで待機
        {
            std::unique_lock<std::mutex> lock(submitted_mutex_);
            submitted_cv_.wait(lock, [this] {
                return !running_.load(std::memory_order_acquire) ||
                       !submitted_frames_.empty();
            });
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
        }

        // エンコード完了をポーリング（タイムアウト5ms）
        // 同期モードでは SubmitFrame 時に既にエンコード完了しているため即座に返る
        std::vector<uint8_t> bitstream;
        ID3D11Texture2D* completed_texture = nullptr;
        int completed_frame_id = 0;
        if (!encoder_->GetCompletedFrame(
                bitstream,
                &completed_texture,
                &completed_frame_id,
                kEncoderPollTimeoutUs)) {
            continue;
        }

        // エンコード完了したテクスチャを submitted_frames_ から回収し、プールに戻す
        const auto submitted = RemoveSubmittedFrame(completed_frame_id);
        ReturnEncodeTexture(completed_texture);
        if (!submitted || bitstream.empty()) {
            continue;
        }

        // エンコード済みフレームを送信キューに追加
        encoded_count_.fetch_add(1, std::memory_order_relaxed);
        encoded_frames_.Push(EncodedFrame{
            std::move(bitstream),
            submitted->timestamp_us,
        });
    }
}

// ============================================================================
// SenderLoop — 送信スレッドのメインループ
// エンコード済みH.264フレームをWebRTC経由でブラウザに送信する
// ============================================================================
void FramePipeline::SenderLoop() {
    // ヘルスログ出力の次回時刻（2秒ごと）
    auto next_health_log =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);

    // running_ が false でもキューに残りフレームがあれば送信し切る
    while (running_.load(std::memory_order_acquire) ||
           encoded_frames_.Size() > 0) {
        const auto now = std::chrono::steady_clock::now();

        // 2秒ごとにパイプライン全体のスループットをログ出力
        if (now >= next_health_log) {
            size_t in_flight = 0;
            {
                std::lock_guard<std::mutex> lock(submitted_mutex_);
                in_flight = submitted_frames_.size();
            }
            std::printf(
                "[FramePipeline] health capture=%llu submit=%llu "
                "encode=%llu sent=%llu send_fail=%llu in_flight=%zu queued=%zu\n",
                static_cast<unsigned long long>(
                    captured_count_.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    submitted_count_.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    encoded_count_.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    sent_count_.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    send_failure_count_.load(std::memory_order_relaxed)),
                in_flight,
                encoded_frames_.Size());
            std::fflush(stdout);
            next_health_log = now + std::chrono::seconds(2);
        }

        // エンコード済みフレームをキューから取得（5msタイムアウト）
        EncodedFrame frame;
        if (!encoded_frames_.WaitPopFor(frame, kSenderWaitTimeout)) {
            continue;
        }
        if (frame.data.empty()) {
            continue;
        }

        // WebRTC経由でH.264フレームを送信
        if (!streamer_->PushVideoFrame(
                frame.data.data(),
                frame.data.size(),
                frame.timestamp_us)) {
            // 送信失敗: 次フレームをキーフレームにしてデコーダに再同期させる
            send_failure_count_.fetch_add(1, std::memory_order_relaxed);
            RequestKeyFrame();
        } else {
            sent_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ============================================================================
// CopyToEncodeTexture — クロスアダプタコピー（キャプチャGPU → CPU → エンコードGPU）
// ハイブリッドGPU環境でキャプチャとエンコードが異なるGPUの場合に必要な処理
// フロー: キャプチャGPU → ステージングテクスチャ(CPU) → カーソル合成 → エンコードGPU
// ============================================================================
bool FramePipeline::CopyToEncodeTexture(
    ID3D11Texture2D* source,
    ID3D11Texture2D* destination) {
    if (!source || !destination) {
        return false;
    }

    // キャプチャGPUとエンコードGPUのコンテキストを取得
    auto* capture_context = capture_engine_->GetContext();
    auto* encode_context = encode_device_manager_->GetContext();
    if (!capture_context || !encode_context) {
        return false;
    }

    // ソースとデスティネーションのテクスチャ情報を取得
    D3D11_TEXTURE2D_DESC source_desc{};
    D3D11_TEXTURE2D_DESC dest_desc{};
    source->GetDesc(&source_desc);
    destination->GetDesc(&dest_desc);

    // CPU転送用ステージングテクスチャを確保（解像度変更時のみ再作成）
    if (!EnsureStagingTexture(source_desc)) {
        return false;
    }

    // [1] キャプチャGPU: キャプチャテクスチャ → ステージングテクスチャにコピー
    capture_context->CopyResource(staging_texture_.Get(), source);

    // [2] ステージングテクスチャをCPUにマップ（読み書き可能）
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT result = capture_context->Map(
        staging_texture_.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
    if (FAILED(result)) {
        std::printf(
            "[FramePipeline] staging texture map failed: 0x%08lX\n",
            result);
        return false;
    }

    // [3] 回転の必要性を判定
    // 90°/270°回転は縦横が入れ替わる（quarter_turn）場合のみ処理
    // 180°回転は縦横が同じなので常に処理
    const auto& display = capture_engine_->GetDisplay();
    const uint32_t output_width = dest_desc.Width;
    const uint32_t output_height = dest_desc.Height;
    const bool quarter_turn =
        source_desc.Width == output_height &&
        source_desc.Height == output_width;
    const bool needs_rotation =
        display.rotation == DXGI_MODE_ROTATION_ROTATE180 ||
        ((display.rotation == DXGI_MODE_ROTATION_ROTATE90 ||
          display.rotation == DXGI_MODE_ROTATION_ROTATE270) &&
         quarter_turn);

    // [4] 回転なし: そのままカーソル合成 → エンコードGPUに転送
    //     回転あり: ピクセル単位で回転処理 → カーソル合成 → エンコードGPUに転送
    if (!needs_rotation) {
        CopyWithoutRotation(mapped, output_width, output_height, destination);
    } else {
        CopyWithRotation(
            mapped, source_desc, output_width, output_height, destination);
    }

    // ステージングテクスチャのマップを解除
    capture_context->Unmap(staging_texture_.Get(), 0);
    return true;
}

// CPU転送用ステージングテクスチャを確保（解像度・フォーマットが同じなら再利用）
// ステージングテクスチャは D3D11_USAGE_STAGING で CPU アクセス可能
bool FramePipeline::EnsureStagingTexture(
    const D3D11_TEXTURE2D_DESC& source_desc) {
    D3D11_TEXTURE2D_DESC staging_desc{};
    if (staging_texture_) {
        staging_texture_->GetDesc(&staging_desc);
    }

    // 既存のステージングテクスチャが流用可能ならそのまま返す
    if (staging_texture_ &&
        source_desc.Width == staging_desc.Width &&
        source_desc.Height == staging_desc.Height &&
        source_desc.Format == staging_desc.Format) {
        return true;
    }

    // 新しいステージングテクスチャを作成
    D3D11_TEXTURE2D_DESC new_desc = source_desc;
    new_desc.Usage = D3D11_USAGE_STAGING;       // CPU転送用
    new_desc.BindFlags = 0;                      // バインド不要
    new_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    new_desc.MiscFlags = 0;
    staging_texture_.Reset();

    // キャプチャGPU上にステージングテクスチャを作成
    HRESULT result = capture_engine_->GetDevice()->CreateTexture2D(
        &new_desc, nullptr, &staging_texture_);
    if (FAILED(result)) {
        std::printf(
            "[FramePipeline] staging texture creation failed: 0x%08lX\n",
            result);
        return false;
    }

    std::printf(
        "[FramePipeline] staging texture created: %ux%u, rotation=%d\n",
        source_desc.Width,
        source_desc.Height,
        static_cast<int>(capture_engine_->GetDisplay().rotation));
    return true;
}

// 回転なしの場合のコピー: カーソル合成 → エンコードGPUに転送
// ステージングテクスチャのピクセルデータに直接カーソルを合成し、
// そのまま UpdateSubresource でエンコードGPUテクスチャに転送する
void FramePipeline::CopyWithoutRotation(
    const D3D11_MAPPED_SUBRESOURCE& mapped,
    uint32_t output_width,
    uint32_t output_height,
    ID3D11Texture2D* destination) {
    const auto& display = capture_engine_->GetDisplay();
    // CPU上でマウスカーソルをBGRAバッファにアルファブレンド合成
    cursor_compositor_.Composite(
        static_cast<uint8_t*>(mapped.pData),
        mapped.RowPitch,
        output_width,
        output_height,
        display.bounds);
    // エンコードGPU: CPUデータからテクスチャに転送
    encode_device_manager_->GetContext()->UpdateSubresource(
        destination, 0, nullptr, mapped.pData, mapped.RowPitch, 0);
}

// 回転ありの場合のコピー: ピクセル単位で回転 → カーソル合成 → エンコードGPUに転送
// 90°/180°/270°回転をCPU上で行う（GPUシェーダを使わずシンプルにCPU処理）
void FramePipeline::CopyWithRotation(
    const D3D11_MAPPED_SUBRESOURCE& mapped,
    const D3D11_TEXTURE2D_DESC& source_desc,
    uint32_t output_width,
    uint32_t output_height,
    ID3D11Texture2D* destination) {
    const auto& display = capture_engine_->GetDisplay();
    const auto* source_pixels =
        reinterpret_cast<const uint32_t*>(mapped.pData);
    const uint32_t source_stride = mapped.RowPitch / sizeof(uint32_t);

    // 回転後のピクセルバッファを確保
    std::vector<uint32_t> rotated_pixels(
        static_cast<size_t>(output_width) * output_height);

    // ピクセル単位で回転処理
    // 90°: (x,y) → (y, H-1-x)
    // 180°: (x,y) → (W-1-x, H-1-y)
    // 270°: (x,y) → (W-1-y, x)
    for (uint32_t y = 0; y < output_height; ++y) {
        for (uint32_t x = 0; x < output_width; ++x) {
            uint32_t sx = x;
            uint32_t sy = y;
            switch (display.rotation) {
                case DXGI_MODE_ROTATION_ROTATE90:
                    sx = y;
                    sy = source_desc.Height - 1 - x;
                    break;
                case DXGI_MODE_ROTATION_ROTATE180:
                    sx = source_desc.Width - 1 - x;
                    sy = source_desc.Height - 1 - y;
                    break;
                case DXGI_MODE_ROTATION_ROTATE270:
                    sx = source_desc.Width - 1 - y;
                    sy = x;
                    break;
                default:
                    break;
            }
            rotated_pixels[static_cast<size_t>(y) * output_width + x] =
                source_pixels[
                    static_cast<size_t>(sy) * source_stride + sx];
        }
    }

    // 回転後のバッファにカーソルを合成
    const uint32_t output_stride = output_width * sizeof(uint32_t);
    cursor_compositor_.Composite(
        reinterpret_cast<uint8_t*>(rotated_pixels.data()),
        output_stride,
        output_width,
        output_height,
        display.bounds);
    // エンコードGPU: 回転+カーソル合成済みデータをテクスチャに転送
    encode_device_manager_->GetContext()->UpdateSubresource(
        destination,
        0,
        nullptr,
        rotated_pixels.data(),
        output_stride,
        0);
}

// エンコード用テクスチャをプールから取得（スレッドセーフ）
// プールが空の場合は nullptr を返す（CaptureLoop はスキップする）
ID3D11Texture2D* FramePipeline::AcquireEncodeTexture() {
    std::lock_guard<std::mutex> lock(texture_pool_mutex_);
    if (free_encode_textures_.empty()) {
        return nullptr;
    }

    // 末尾から取り出し（vector の back/pop_back は O(1)）
    ID3D11Texture2D* texture = free_encode_textures_.back();
    free_encode_textures_.pop_back();
    return texture;
}

// エンコード用テクスチャをプールに戻す（エンコード完了後やエラー時に呼ばれる）
void FramePipeline::ReturnEncodeTexture(ID3D11Texture2D* texture) {
    if (!texture) {
        return;
    }
    std::lock_guard<std::mutex> lock(texture_pool_mutex_);
    free_encode_textures_.push_back(texture);
}

// バックログ検出: 処理中フレームが上限以上ある場合は true を返す
// submitted_frames_（エンコード待ち）と encoded_frames_（送信待ち）の両方をチェック
// バックログ時は CaptureLoop がキャプチャをスキップし、遅延が蓄積しないようにする
bool FramePipeline::IsBacklogged() const {
    size_t submitted_count = 0;
    {
        std::lock_guard<std::mutex> lock(submitted_mutex_);
        submitted_count = submitted_frames_.size();
    }
    return submitted_count >= kMaximumFramesInFlight ||
           encoded_frames_.Size() >= kMaximumFramesInFlight;
}

// 動的ビットレートの推奨値があればエンコーダに適用する
// AdaptiveBitrateController から推奨ビットレートを取得し、
// エンコーダの ReconfigureBitrate で動的に変更する
// 変更後はキーフレームを要求して画質の急変を緩和する
void FramePipeline::ApplyBitrateRecommendation() {
    if (!kEnableDynamicBitrate) {
        return;
    }

    // コントローラから推奨ビットレートを取得（更新間隔内なら nullopt）
    const auto target = bitrate_controller_.TakeUpdate(
        current_bitrate_kbps_, std::chrono::steady_clock::now());
    if (!target) {
        return;
    }

    // エンコーダのビットレートを再設定
    if (encoder_->ReconfigureBitrate(*target)) {
        current_bitrate_kbps_ = *target;
        RequestKeyFrame();  // ビットレート変更後にキーフレームを強制
    }
}

// 外部からキーフレーム要求をセットする（atomicなのでスレッドセーフ）
// CaptureLoop の次回フレームで force_key_frame_ がチェックされる
void FramePipeline::RequestKeyFrame() {
    force_key_frame_.store(true, std::memory_order_release);
}

// WebRTCのREMB（Receiver Estimated Maximum Bitrate）を受信した時に呼ばれる
// DisplaySessionManager 経由で WebrtcStreamer から呼び出される
void FramePipeline::OnBitrateEstimate(uint32_t bitrate_bps) {
    bitrate_controller_.OnReceiverEstimate(bitrate_bps);
}

// 指定した frame_id の SubmittedFrame を submitted_frames_ から取り出して削除
// エンコード完了時にテクスチャとタイムスタンプを回収するために使用
std::optional<FramePipeline::SubmittedFrame>
FramePipeline::RemoveSubmittedFrame(int frame_id) {
    std::lock_guard<std::mutex> lock(submitted_mutex_);
    const auto iterator = submitted_frames_.find(frame_id);
    if (iterator == submitted_frames_.end()) {
        return std::nullopt;
    }

    SubmittedFrame frame = iterator->second;
    submitted_frames_.erase(iterator);
    return frame;
}

// submitted_frames_ に残っている全テクスチャをプールに戻してクリア
// Stop() や ReleaseResources() から呼ばれる
void FramePipeline::ClearSubmittedFrames() {
    std::vector<ID3D11Texture2D*> textures;
    {
        std::lock_guard<std::mutex> lock(submitted_mutex_);
        textures.reserve(submitted_frames_.size());
        for (const auto& [frame_id, frame] : submitted_frames_) {
            (void)frame_id;  // frame_id は使用しない
            if (frame.texture) {
                textures.push_back(frame.texture);
            }
        }
        submitted_frames_.clear();
    }

    // ロック外でテクスチャをプールに戻す（デッドロック防止）
    for (auto* texture : textures) {
        ReturnEncodeTexture(texture);
    }
}

// NVIDIAアダプタ（vendor_id=0x10DE）のインデックスを検索する
// 見つからない場合は -1 を返す（キャプチャアダプタにフォールバック）
int FramePipeline::FindNvidiaAdapterIndex() const {
    const auto adapters = D3D11DeviceManager::EnumerateAdapters();
    for (const auto& adapter : adapters) {
        if (adapter.vendor_id == 0x10DE) {  // NVIDIA
            std::printf(
                "[FramePipeline] NVIDIA adapter: %ls (index=%d)\n",
                adapter.name.c_str(),
                adapter.index);
            return adapter.index;
        }
    }
    return -1;
}

} // namespace yamanoha
