// ============================================================================
// video_encoder.h
// エンコーダ抽象インターフェース — NVENC / Media Foundation 等の実装を透過的に切り替え
// ============================================================================
#pragma once

#include "encoder_config.h"

#include <d3d11.h>
#include <cstdint>
#include <vector>

namespace yamanoha {

/// @brief H.264エンコーダの抽象インターフェース
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    virtual bool Initialize(ID3D11Device* device, const EncoderConfig& config) = 0;

    /// @brief フレームをエンコーダに投入する（非同期）
    /// @return 成功時はフレームID（正の値）、失敗時は -1
    virtual int SubmitFrame(ID3D11Texture2D* texture, bool force_idr) = 0;

    /// @brief 次に完了したエンコードフレームを取得する（非同期）
    virtual bool GetCompletedFrame(
        std::vector<uint8_t>& out_bitstream,
        ID3D11Texture2D** out_texture,
        int* out_frame_id,
        uint64_t timeout_us) = 0;

    /// @brief バッファに残っているフレームをフラッシュする
    virtual bool Flush(std::vector<uint8_t>& out_bitstream) = 0;

    /// @brief 実行中のセッションのビットレートを変更する
    virtual bool ReconfigureBitrate(uint32_t bitrate_kbps) = 0;

    virtual void Shutdown() = 0;

    [[nodiscard]] virtual bool IsInitialized() const = 0;
    [[nodiscard]] virtual uint32_t GetWidth() const = 0;
    [[nodiscard]] virtual uint32_t GetHeight() const = 0;
};

} // namespace yamanoha
