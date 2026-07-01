#include "pipeline/frame_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace yamanoha {

namespace {

constexpr size_t kEncodeTexturePoolSize = 4;
constexpr size_t kMaximumFramesInFlight = 3;
constexpr uint64_t kEncoderPollTimeoutUs = 5000;
constexpr auto kSenderWaitTimeout = std::chrono::milliseconds(5);
constexpr auto kIdleWait = std::chrono::milliseconds(1);

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
    int bitrate_kbps) {
    Stop();
    if (fps <= 0 || bitrate_kbps <= 0) {
        std::printf("[FramePipeline] invalid fps or bitrate\n");
        return false;
    }

    fps_ = fps;
    current_bitrate_kbps_ = static_cast<uint32_t>(bitrate_kbps);
    force_key_frame_.store(true, std::memory_order_release);

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

    streamer_ = std::make_unique<WebrtcStreamer>();
    ConfigureStreamerCallbacks();
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

    ClearSubmittedFrames();
    encoded_frames_.Clear();
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
    D3D11_TEXTURE2D_DESC staging_description{};
    staging_description.Width = width;
    staging_description.Height = height;
    staging_description.MipLevels = 1;
    staging_description.ArraySize = 1;
    staging_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_description.SampleDesc.Count = 1;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT result = capture_engine_->GetDevice()->CreateTexture2D(
        &staging_description, nullptr, &staging_texture_);
    if (FAILED(result)) {
        std::printf(
            "[FramePipeline] staging texture creation failed: 0x%08lX\n",
            result);
        return false;
    }

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
        result = encode_device_manager_->GetDevice()->CreateTexture2D(
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

    encoder_ = std::make_unique<NvencEncoder>();
    if (encoder_->Initialize(encode_device_manager_->GetDevice(), config)) {
        return true;
    }

    std::printf("[FramePipeline] NVENC initialization failed\n");
    encoder_.reset();
    return false;
}

void FramePipeline::ConfigureStreamerCallbacks() {
    streamer_->SetKeyFrameRequestCallback([this] {
        RequestKeyFrame();
    });
    streamer_->SetBitrateRecommendationCallback(
        [this](uint32_t bitrate_bps) {
            bitrate_controller_.OnReceiverEstimate(bitrate_bps);
        });
}

void FramePipeline::ReleaseResources() {
    prepared_ = false;
    if (streamer_) {
        streamer_->Shutdown();
        streamer_.reset();
    }
    if (encoder_) {
        encoder_->Shutdown();
        encoder_.reset();
    }

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

    while (running_.load(std::memory_order_acquire)) {
        const auto frame_started = std::chrono::steady_clock::now();
        if (!streamer_ || !streamer_->IsConnected()) {
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

        if (!captured_frame.has_new_frame && !first_frame) {
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

        const bool force_key_frame =
            first_frame ||
            force_key_frame_.exchange(false, std::memory_order_acq_rel);
        const uint64_t timestamp_us =
            TimestampMicroseconds(frame_started);

        int frame_id = -1;
        {
            std::lock_guard<std::mutex> lock(submitted_mutex_);
            frame_id = encoder_->SubmitFrame(
                encode_texture, force_key_frame);
            if (frame_id >= 0) {
                submitted_frames_.emplace(
                    frame_id,
                    SubmittedFrame{encode_texture, timestamp_us});
            }
        }
        if (frame_id >= 0) {
            submitted_cv_.notify_one();
        }

        if (frame_id < 0) {
            ReturnEncodeTexture(encode_texture);
            if (force_key_frame) {
                RequestKeyFrame();
            }
            continue;
        }

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

        encoded_frames_.Push(EncodedFrame{
            std::move(bitstream),
            submitted->timestamp_us,
        });
    }
}

void FramePipeline::SenderLoop() {
    while (running_.load(std::memory_order_acquire) ||
           encoded_frames_.Size() > 0) {
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
            RequestKeyFrame();
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

    capture_context->CopyResource(staging_texture_.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = capture_context->Map(
        staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        std::printf(
            "[FramePipeline] staging texture map failed: 0x%08lX\n",
            result);
        return false;
    }

    encode_context->UpdateSubresource(
        destination, 0, nullptr, mapped.pData, mapped.RowPitch, 0);
    capture_context->Unmap(staging_texture_.Get(), 0);
    return true;
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
