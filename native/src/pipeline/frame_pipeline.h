#pragma once

#include "capture/capture_engine.h"
#include "capture/display_manager.h"
#include "encoder/nvenc_encoder.h"
#include "gpu/d3d11_device.h"
#include "pipeline/adaptive_bitrate_controller.h"
#include "pipeline/blocking_queue.h"
#include "webrtc/webrtc_streamer.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace yamanoha {

class FramePipeline {
public:
    FramePipeline() = default;
    ~FramePipeline();

    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;
    FramePipeline(FramePipeline&&) = delete;
    FramePipeline& operator=(FramePipeline&&) = delete;

    bool Start(const Display& display, int fps, int bitrate_kbps);
    void StartThreads();
    void Stop();

    [[nodiscard]] bool IsRunning() const {
        return running_.load(std::memory_order_acquire);
    }
    [[nodiscard]] WebrtcStreamer* GetStreamer() const {
        return streamer_.get();
    }
    [[nodiscard]] uint32_t GetWidth() const;
    [[nodiscard]] uint32_t GetHeight() const;

private:
    struct SubmittedFrame {
        ID3D11Texture2D* texture = nullptr;
        uint64_t timestamp_us = 0;
    };

    struct EncodedFrame {
        std::vector<uint8_t> data;
        uint64_t timestamp_us = 0;
    };

    bool InitializeCapture(const Display& display);
    bool InitializeEncoderDevice(const Display& display);
    bool CreateTransferTextures(uint32_t width, uint32_t height);
    bool InitializeEncoder(uint32_t width, uint32_t height, int bitrate_kbps);
    void ConfigureStreamerCallbacks();
    void ReleaseResources();

    void CaptureLoop();
    void EncoderLoop();
    void SenderLoop();

    bool CopyToEncodeTexture(
        ID3D11Texture2D* source,
        ID3D11Texture2D* destination);
    ID3D11Texture2D* AcquireEncodeTexture();
    void ReturnEncodeTexture(ID3D11Texture2D* texture);

    bool IsBacklogged() const;
    void ApplyBitrateRecommendation();
    void RequestKeyFrame();

    std::optional<SubmittedFrame> RemoveSubmittedFrame(int frame_id);
    void ClearSubmittedFrames();
    int FindNvidiaAdapterIndex() const;

    std::unique_ptr<CaptureEngine> capture_engine_;
    std::unique_ptr<D3D11DeviceManager> encode_device_manager_;
    std::unique_ptr<NvencEncoder> encoder_;
    std::unique_ptr<WebrtcStreamer> streamer_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> encode_textures_;

    mutable std::mutex texture_pool_mutex_;
    std::vector<ID3D11Texture2D*> free_encode_textures_;

    mutable std::mutex submitted_mutex_;
    std::condition_variable submitted_cv_;
    std::unordered_map<int, SubmittedFrame> submitted_frames_;

    BlockingQueue<EncodedFrame> encoded_frames_;
    AdaptiveBitrateController bitrate_controller_;

    std::thread capture_thread_;
    std::thread encoder_thread_;
    std::thread sender_thread_;

    std::atomic<bool> running_{false};
    std::atomic<bool> force_key_frame_{true};
    bool prepared_ = false;
    int fps_ = 0;
    uint32_t current_bitrate_kbps_ = 0;
};

} // namespace yamanoha
