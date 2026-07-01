// ============================================================================
// nvenc_encoder.h
// NVIDIA NVENC SDK を使用したハードウェア H.264 エンコーダクラスの定義
// ============================================================================
#pragma once

#include "encoder_config.h"
#include "third_party/nvcodec/nvEncodeAPI.h"

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace yamanoha {

/// @brief NVENC を使用して GPU 上の ID3D11Texture2D から直接 H.264 ストリームを生成するクラス
class NvencEncoder {
public:
    NvencEncoder();
    ~NvencEncoder();

    // コピー禁止・ムーブ許可
    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;
    NvencEncoder(NvencEncoder&&) noexcept = default;
    NvencEncoder& operator=(NvencEncoder&&) noexcept = default;

    /// @brief エンコーダの初期化
    /// @param device D3D11デバイス。NVENC セッションはこのデバイスに関連付けられます。
    /// @param config エンコード設定
    /// @return 成功時 true
    bool Initialize(ID3D11Device* device, const EncoderConfig& config);

    /// @brief フレームをエンコーダに投入する（非同期）
    /// @param texture エンコード対象の D3D11テクスチャ (BGRA8)
    /// @param force_idr キーフレーム生成を強制するかどうか
    /// @return 成功時はフレームID（正の値）、失敗時は -1
    int SubmitFrame(ID3D11Texture2D* texture, bool force_idr);

    /// @brief 次に完了したエンコードフレームを取得する（非同期）
    /// @param out_bitstream エンコード結果の NAL ユニットデータ格納先
    /// @param out_texture 入力テクスチャを返す（呼び出し側でプールに戻す）
    /// @param out_frame_id 完了したフレームIDを返す
    /// @param timeout_us 待機タイムアウト（0はポーリング、UINT64_MAXは無限）
    /// @return 完了フレームが取得できた場合 true
    bool GetCompletedFrame(std::vector<uint8_t>& out_bitstream, ID3D11Texture2D** out_texture, int* out_frame_id, uint64_t timeout_us);

    /// @brief フレームをエンコードする（同期ラッパー、後方互換）
    /// @param texture エンコード対象の D3D11テクスチャ (BGRA8)
    /// @param force_idr キーフレーム生成を強制するかどうか
    /// @param out_bitstream エンコード結果の NAL ユニットデータ格納先
    /// @return 成功時 true
    bool EncodeFrame(ID3D11Texture2D* texture, bool force_idr, std::vector<uint8_t>& out_bitstream);

    /// @brief バッファに残っているフレームをフラッシュする（ストリーミング終了時）
    /// @param out_bitstream フラッシュされたビットストリームデータ
    /// @return 成功時 true
    bool Flush(std::vector<uint8_t>& out_bitstream);

    /// @brief 全リソースを解放する
    void Shutdown();

    [[nodiscard]] bool IsInitialized() const { return initialized_; }
    [[nodiscard]] uint32_t GetWidth() const { return width_; }
    [[nodiscard]] uint32_t GetHeight() const { return height_; }

private:
    // NVENC APIテーブルを DLL から動的ロードする
    bool LoadNvencApi();

    // 出力ビットストリーム用バッファを作成
    bool CreateOutputBuffers();

    // D3D11 テクスチャリソースを NVENC 入力リソースとして登録・マップする
    void* RegisterResource(ID3D11Texture2D* texture);
    void UnregisterResource(void* registered_resource);

private:
    // D3D11 デバイス
    ComPtr<ID3D11Device> device_;

    // DLL ハンドルと API ファンクションリスト
    HMODULE nvenc_dll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST nv_api_{};

    // NVENC エンコーダセッションハンドル
    void* encoder_handle_ = nullptr;

    // 現在の設定
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    EncoderConfig config_{};
    bool initialized_ = false;

    // 非同期エンコード用リソースプール管理
    bool CreateAsyncResources();
    void ReleaseAsyncResources();

    struct AsyncJob {
        int frame_id = 0;
        ID3D11Texture2D* texture = nullptr;
        void* mapped_resource = nullptr;
        NV_ENC_OUTPUT_PTR output_buffer = nullptr;
        HANDLE completion_event = nullptr;
    };

    std::vector<NV_ENC_OUTPUT_PTR> free_output_buffers_;
    std::vector<HANDLE> free_events_;
    std::deque<AsyncJob> pending_jobs_;
    std::mutex jobs_mutex_;
    int next_frame_id_ = 1;

    // 登録済み D3D11 テクスチャのキャッシュ（毎フレームの登録・解除のオーバーヘッドを避けるため）
    struct RegisteredTexture {
        ID3D11Texture2D* raw_texture = nullptr;
        void* nvenc_registered_resource = nullptr;
    };
    std::vector<RegisteredTexture> registered_textures_;
};

} // namespace yamanoha
