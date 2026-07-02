#include "pipeline/frame_pipeline.h"
#include "webrtc/webrtc_streamer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace yamanoha {

namespace {

constexpr size_t kEncodeTexturePoolSize = 4;
constexpr size_t kMaximumFramesInFlight = 3;
constexpr uint64_t kEncoderPollTimeoutUs = 5000;
constexpr auto kSenderWaitTimeout = std::chrono::milliseconds(5);
constexpr auto kIdleWait = std::chrono::milliseconds(1);
constexpr auto kPeriodicKeyFrameInterval = std::chrono::seconds(3);
// nvEncReconfigureEncoder can strand completion events when asynchronous
// frames are still in flight. Keep streaming at the configured bitrate until
// reconfiguration is implemented as a drain-and-resume operation.
constexpr bool kEnableDynamicBitrate = false;

uint64_t TimestampMicroseconds(
    std::chrono::steady_clock::time_point time_point) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            time_point.time_since_epoch()).count());
}

} // namespace

FramePipeline::~FramePipeline() {
    Stop();
}

bool FramePipeline::Start(
    const Display& display,
    int fps,
    int bitrate_kbps,
    WebrtcStreamer* streamer) {
    Stop();
    streamer_ = streamer;
    if (fps <= 0 || bitrate_kbps <= 0) {
        std::printf("[FramePipeline] invalid fps or bitrate\n");
        return false;
    }

    fps_ = fps;
    current_bitrate_kbps_ = static_cast<uint32_t>(bitrate_kbps);
    force_key_frame_.store(true, std::memory_order_release);
    captured_count_.store(0, std::memory_order_release);
    submitted_count_.store(0, std::memory_order_release);
    encoded_count_.store(0, std::memory_order_release);
    sent_count_.store(0, std::memory_order_release);
    send_failure_count_.store(0, std::memory_order_release);

    AdaptiveBitrateController::Config bitrate_config;
    bitrate_config.maximum_kbps = current_bitrate_kbps_;
    bitrate_config.minimum_kbps =
        (std::min)(500u, current_bitrate_kbps_);
    bitrate_controller_.Reset(current_bitrate_kbps_, bitrate_config);

    if (!InitializeCapture(display)) {
        ReleaseResources();
        return false;
    }

    const uint32_t width = capture_engine_->GetWidth();
    const uint32_t height = capture_engine_->GetHeight();
    if (!InitializeEncoderDevice(display) ||
        !CreateTransferTextures(width, height) ||
        !InitializeEncoder(width, height, bitrate_kbps)) {
        ReleaseResources();
        return false;
    }

    encoded_frames_.Reset();
    prepared_ = true;

    std::printf(
        "[FramePipeline] prepared: %u x %u, %d FPS, %u kbps\n",
        width,
        height,
        fps_,
        current_bitrate_kbps_);
    std::fflush(stdout);
    return true;
}

void FramePipeline::StartThreads() {
    if (!prepared_ || running_.exchange(true)) {
        return;
    }

    encoded_frames_.Reset();
    capture_thread_ = std::thread(&FramePipeline::CaptureLoop, this);
    encoder_thread_ = std::thread(&FramePipeline::EncoderLoop, this);
    sender_thread_ = std::thread(&FramePipeline::SenderLoop, this);
    std::printf("[FramePipeline] worker threads started\n");
    std::fflush(stdout);
}

void FramePipeline::Stop() {
    const bool had_resources =
        prepared_ || capture_engine_ || encoder_ || streamer_;

    if (had_resources) {
        std::printf("[FramePipeline] stopping worker threads...\n");
        std::fflush(stdout);
    }

    running_.store(false, std::memory_order_release);
    submitted_cv_.notify_all();
    encoded_frames_.Stop();

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }
    if (sender_thread_.joinable()) {
        sender_thread_.join();
    }

    if (encoder_) {
        std::vector<uint8_t> ignored;
        encoder_->Flush(ignored);
    }

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

uint32_t FramePipeline::GetWidth() const {
    return capture_engine_ ? capture_engine_->GetWidth() : 0;
}

uint32_t FramePipeline::GetHeight() const {
    return capture_engine_ ? capture_engine_->GetHeight() : 0;
}

bool FramePipeline::InitializeCapture(const Display& display) {
    capture_engine_ = std::make_unique<CaptureEngine>();
    if (capture_engine_->Start(display)) {
        return true;
    }

    std::printf("[FramePipeline] capture initialization failed\n");
    capture_engine_.reset();
    return false;
}

bool FramePipeline::InitializeEncoderDevice(const Display& display) {
    encode_device_manager_ = std::make_unique<D3D11DeviceManager>();
    int adapter_index = FindNvidiaAdapterIndex();
    if (adapter_index < 0) {
        adapter_index = display.adapter_index;
    }

    if (encode_device_manager_->Initialize(adapter_index)) {
        return true;
    }

    if (adapter_index == display.adapter_index) {
        return false;
    }

    std::printf(
        "[FramePipeline] NVIDIA device failed; using capture adapter\n");
    encode_device_manager_ = std::make_unique<D3D11DeviceManager>();
    return encode_device_manager_->Initialize(display.adapter_index);
}

bool FramePipeline::CreateTransferTextures(
    uint32_t width,
    uint32_t height) {
    D3D11_TEXTURE2D_DESC encode_description{};
    encode_description.Width = width;
    encode_description.Height = height;
    encode_description.MipLevels = 1;
    encode_description.ArraySize = 1;
    encode_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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
        free_encode_textures_.push_back(texture.Get());
        encode_textures_.push_back(std::move(texture));
    }
    return true;
}

bool FramePipeline::InitializeEncoder(
    uint32_t width,
    uint32_t height,
    int bitrate_kbps) {
    EncoderConfig config{};
    config.width = width;
    config.height = height;
    config.fps = static_cast<uint32_t>(fps_);
    config.bitrate_kbps = static_cast<uint32_t>(bitrate_kbps);
    config.max_bitrate_kbps = config.bitrate_kbps;
    config.use_cbr = true;
    config.zero_latency = true;
    config.preset = 1;
    config.gop_size = config.fps;

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

void FramePipeline::ReleaseResources() {
    prepared_ = false;
    streamer_ = nullptr;
    if (encoder_) {
        encoder_->Shutdown();
        encoder_.reset();
    }

    ClearSubmittedFrames();
    staging_texture_.Reset();
    {
        std::lock_guard<std::mutex> lock(texture_pool_mutex_);
        free_encode_textures_.clear();
        encode_textures_.clear();
    }
    encode_device_manager_.reset();
    capture_engine_.reset();
    fps_ = 0;
    current_bitrate_kbps_ = 0;
}

void FramePipeline::CaptureLoop() {
    bool first_frame = true;
    auto last_keyframe_time = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        const auto frame_started = std::chrono::steady_clock::now();
        if (!streamer_ || !streamer_->IsConnected()) {
            // Reset first_frame so the next frame after reconnection is a keyframe
            first_frame = true;
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        ApplyBitrateRecommendation();
        if (IsBacklogged()) {
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        CapturedFrame captured_frame{};
        if (!capture_engine_->AcquireFrame(captured_frame, 0)) {
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        // 周期キーフレームが必要かどうかを先に判定
        const bool periodic_keyframe_due =
            (frame_started - last_keyframe_time >= kPeriodicKeyFrameInterval);

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

        ID3D11Texture2D* encode_texture = AcquireEncodeTexture();
        if (!encode_texture) {
            capture_engine_->ReleaseFrame();
            std::this_thread::sleep_for(kIdleWait);
            continue;
        }

        const bool copied = CopyToEncodeTexture(
            captured_frame.texture.Get(), encode_texture);
        capture_engine_->ReleaseFrame();
        if (!copied) {
            ReturnEncodeTexture(encode_texture);
            continue;
        }
        captured_count_.fetch_add(1, std::memory_order_relaxed);

        const bool force_key_frame =
            first_frame ||
            force_key_frame_.exchange(false, std::memory_order_acq_rel) ||
            (frame_started - last_keyframe_time >= kPeriodicKeyFrameInterval);
        const uint64_t timestamp_us =
            TimestampMicroseconds(frame_started);

        // NvEncEncodePicture may block in synchronous mode until the output
        // thread drains an earlier frame. Never hold submitted_mutex_ across
        // that driver call or EncoderLoop cannot perform the drain.
        const int frame_id = encoder_->SubmitFrame(
            encode_texture, force_key_frame);
        if (frame_id < 0) {
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

        {
            std::lock_guard<std::mutex> lock(submitted_mutex_);
            submitted_frames_.emplace(
                frame_id,
                SubmittedFrame{encode_texture, timestamp_us});
        }
        submitted_count_.fetch_add(1, std::memory_order_relaxed);
        submitted_cv_.notify_one();

        first_frame = false;
        const auto elapsed = std::chrono::steady_clock::now() - frame_started;
        const auto frame_interval =
            std::chrono::microseconds(1000000 / fps_);
        if (elapsed < frame_interval) {
            std::this_thread::sleep_for(frame_interval - elapsed);
        }
    }
}

void FramePipeline::EncoderLoop() {
    while (running_.load(std::memory_order_acquire)) {
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

        const auto submitted = RemoveSubmittedFrame(completed_frame_id);
        ReturnEncodeTexture(completed_texture);
        if (!submitted || bitstream.empty()) {
            continue;
        }

        encoded_count_.fetch_add(1, std::memory_order_relaxed);
        encoded_frames_.Push(EncodedFrame{
            std::move(bitstream),
            submitted->timestamp_us,
        });
    }
}

void FramePipeline::SenderLoop() {
    auto next_health_log =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (running_.load(std::memory_order_acquire) ||
           encoded_frames_.Size() > 0) {
        const auto now = std::chrono::steady_clock::now();
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

        EncodedFrame frame;
        if (!encoded_frames_.WaitPopFor(frame, kSenderWaitTimeout)) {
            continue;
        }
        if (frame.data.empty()) {
            continue;
        }

        if (!streamer_->PushVideoFrame(
                frame.data.data(),
                frame.data.size(),
                frame.timestamp_us)) {
            send_failure_count_.fetch_add(1, std::memory_order_relaxed);
            RequestKeyFrame();
        } else {
            sent_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool FramePipeline::CopyToEncodeTexture(
    ID3D11Texture2D* source,
    ID3D11Texture2D* destination) {
    if (!source || !destination) {
        return false;
    }

    auto* capture_context = capture_engine_->GetContext();
    auto* encode_context = encode_device_manager_->GetContext();
    if (!capture_context || !encode_context) {
        return false;
    }

    D3D11_TEXTURE2D_DESC source_desc{};
    D3D11_TEXTURE2D_DESC dest_desc{};
    source->GetDesc(&source_desc);
    destination->GetDesc(&dest_desc);

    if (!EnsureStagingTexture(source_desc)) {
        return false;
    }

    capture_context->CopyResource(staging_texture_.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT result = capture_context->Map(
        staging_texture_.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
    if (FAILED(result)) {
        std::printf(
            "[FramePipeline] staging texture map failed: 0x%08lX\n",
            result);
        return false;
    }

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

    if (!needs_rotation) {
        CopyWithoutRotation(mapped, output_width, output_height, destination);
    } else {
        CopyWithRotation(
            mapped, source_desc, output_width, output_height, destination);
    }

    capture_context->Unmap(staging_texture_.Get(), 0);
    return true;
}

bool FramePipeline::EnsureStagingTexture(
    const D3D11_TEXTURE2D_DESC& source_desc) {
    D3D11_TEXTURE2D_DESC staging_desc{};
    if (staging_texture_) {
        staging_texture_->GetDesc(&staging_desc);
    }

    if (staging_texture_ &&
        source_desc.Width == staging_desc.Width &&
        source_desc.Height == staging_desc.Height &&
        source_desc.Format == staging_desc.Format) {
        return true;
    }

    D3D11_TEXTURE2D_DESC new_desc = source_desc;
    new_desc.Usage = D3D11_USAGE_STAGING;
    new_desc.BindFlags = 0;
    new_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    new_desc.MiscFlags = 0;
    staging_texture_.Reset();

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

void FramePipeline::CopyWithoutRotation(
    const D3D11_MAPPED_SUBRESOURCE& mapped,
    uint32_t output_width,
    uint32_t output_height,
    ID3D11Texture2D* destination) {
    const auto& display = capture_engine_->GetDisplay();
    cursor_compositor_.Composite(
        static_cast<uint8_t*>(mapped.pData),
        mapped.RowPitch,
        output_width,
        output_height,
        display.bounds);
    encode_device_manager_->GetContext()->UpdateSubresource(
        destination, 0, nullptr, mapped.pData, mapped.RowPitch, 0);
}

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

    std::vector<uint32_t> rotated_pixels(
        static_cast<size_t>(output_width) * output_height);

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

    const uint32_t output_stride = output_width * sizeof(uint32_t);
    cursor_compositor_.Composite(
        reinterpret_cast<uint8_t*>(rotated_pixels.data()),
        output_stride,
        output_width,
        output_height,
        display.bounds);
    encode_device_manager_->GetContext()->UpdateSubresource(
        destination,
        0,
        nullptr,
        rotated_pixels.data(),
        output_stride,
        0);
}

ID3D11Texture2D* FramePipeline::AcquireEncodeTexture() {
    std::lock_guard<std::mutex> lock(texture_pool_mutex_);
    if (free_encode_textures_.empty()) {
        return nullptr;
    }

    ID3D11Texture2D* texture = free_encode_textures_.back();
    free_encode_textures_.pop_back();
    return texture;
}

void FramePipeline::ReturnEncodeTexture(ID3D11Texture2D* texture) {
    if (!texture) {
        return;
    }
    std::lock_guard<std::mutex> lock(texture_pool_mutex_);
    free_encode_textures_.push_back(texture);
}

bool FramePipeline::IsBacklogged() const {
    size_t submitted_count = 0;
    {
        std::lock_guard<std::mutex> lock(submitted_mutex_);
        submitted_count = submitted_frames_.size();
    }
    return submitted_count >= kMaximumFramesInFlight ||
           encoded_frames_.Size() >= kMaximumFramesInFlight;
}

void FramePipeline::ApplyBitrateRecommendation() {
    if (!kEnableDynamicBitrate) {
        return;
    }

    const auto target = bitrate_controller_.TakeUpdate(
        current_bitrate_kbps_, std::chrono::steady_clock::now());
    if (!target) {
        return;
    }

    if (encoder_->ReconfigureBitrate(*target)) {
        current_bitrate_kbps_ = *target;
        RequestKeyFrame();
    }
}

void FramePipeline::RequestKeyFrame() {
    force_key_frame_.store(true, std::memory_order_release);
}

void FramePipeline::OnBitrateEstimate(uint32_t bitrate_bps) {
    bitrate_controller_.OnReceiverEstimate(bitrate_bps);
}

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

void FramePipeline::ClearSubmittedFrames() {
    std::vector<ID3D11Texture2D*> textures;
    {
        std::lock_guard<std::mutex> lock(submitted_mutex_);
        textures.reserve(submitted_frames_.size());
        for (const auto& [frame_id, frame] : submitted_frames_) {
            (void)frame_id;
            if (frame.texture) {
                textures.push_back(frame.texture);
            }
        }
        submitted_frames_.clear();
    }

    for (auto* texture : textures) {
        ReturnEncodeTexture(texture);
    }
}

int FramePipeline::FindNvidiaAdapterIndex() const {
    const auto adapters = D3D11DeviceManager::EnumerateAdapters();
    for (const auto& adapter : adapters) {
        if (adapter.vendor_id == 0x10DE) {
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
