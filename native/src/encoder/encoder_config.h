// ============================================================================
// encoder_config.h
// NVENC エンコーダ用設定パラメータ構造体の定義
// ============================================================================
#pragma once

#include <cstdint>

namespace yamanoha {

/// @brief NVENC エンコーダ用設定構造体
struct EncoderConfig {
    uint32_t width = 1920;               // エンコード解像度の幅
    uint32_t height = 1080;              // エンコード解像度の高さ
    uint32_t fps = 60;                   // フレームレート (fps)
    uint32_t bitrate_kbps = 4000;        // 目標ビットレート (kbps) — デフォルトは 4 Mbps
    uint32_t max_bitrate_kbps = 8000;    // 最大ビットレート (kbps) — デフォルトは 8 Mbps
    uint32_t gop_size = 30;              // GOPサイズ (キーフレーム間隔) — 0.5秒
    uint32_t b_frames = 0;               // Bフレーム数 (0 で超低遅延)
    
    // NVENC プリセット: 1 (P1: 最高速/最低遅延) 〜 7 (P7: 最高画質)
    // リモートデスクトップでは超低遅延を最優先するため P1 をデフォルトに設定
    uint32_t preset = 1;                 
    
    bool zero_latency = true;            // ゼロレイテンシ（超低遅延チューニング）
    bool use_cbr = true;                 // 定数ビットレート (CBR) モードの使用
};

} // namespace yamanoha
