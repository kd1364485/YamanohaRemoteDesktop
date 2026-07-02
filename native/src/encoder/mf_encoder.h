// ============================================================================
// mf_encoder.h
// Media Foundation H.264 エンコーダ — NVENC非搭載PC向けフォールバック
// Intel QuickSync / AMD AMF ハードウェアアクセラレーションを自動利用
// ============================================================================
#pragma once

#include "encoder_config.h"
#include "video_encoder.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// Media Foundation 前方宣言（実装ファイル内でヘッダをinclude）
struct IMFDXGIDeviceManager;
struct IMFTransform;
struct IMFMediaType;
struct IMFSample;
struct IMFMediaEventGenerator;

using Microsoft::WRL::ComPtr;

namespace yamanoha {

class MfEncoder : public VideoEncoder {
public:
    MfEncoder();
    ~MfEncoder();

    MfEncoder(const MfEncoder&) = delete;
    MfEncoder& operator=(const MfEncoder&) = delete;
    MfEncoder(MfEncoder&&) noexcept = default;
    MfEncoder& operator=(MfEncoder&&) noexcept = default;

    bool Initialize(ID3D11Device* device, const EncoderConfig& config) override;
    int SubmitFrame(ID3D11Texture2D* texture, bool force_idr) override;
    bool GetCompletedFrame(
        std::vector<uint8_t>& out_bitstream,
        ID3D11Texture2D** out_texture,
        int* out_frame_id,
        uint64_t timeout_us) override;
    bool Flush(std::vector<uint8_t>& out_bitstream) override;
    bool ReconfigureBitrate(uint32_t bitrate_kbps) override;
    void Shutdown() override;

    [[nodiscard]] bool IsInitialized() const override { return initialized_; }
    [[nodiscard]] uint32_t GetWidth() const override { return width_; }
    [[nodiscard]] uint32_t GetHeight() const override { return height_; }

private:
    bool CreateEncoder();
    bool ConfigureInputType();
    bool ConfigureOutputType();
    bool AllocateInputSample();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    // Media Foundation オブジェクト（前方宣言型のため ComPtr で管理）
    ComPtr<IMFDXGIDeviceManager> mf_device_manager_;
    ComPtr<IMFTransform> encoder_transform_;
    ComPtr<IMFMediaEventGenerator> event_generator_;
    ComPtr<IMFSample> input_sample_;
    ComPtr<IMFMediaType> input_type_;
    ComPtr<IMFMediaType> output_type_;

    uint32_t reset_token_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    EncoderConfig config_{};
    bool initialized_ = false;
    std::mutex api_mutex_;

    struct PendingJob {
        int frame_id = 0;
        ID3D11Texture2D* texture = nullptr;
    };
    std::deque<PendingJob> pending_jobs_;
    std::mutex jobs_mutex_;
    int next_frame_id_ = 1;
};

} // namespace yamanoha
