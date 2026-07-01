// ============================================================================
// desktop_duplicator.h
// Desktop Duplication API ラッパークラス
// GPU メモリ上でデスクトップ画面をキャプチャする
//
// 設計方針:
// DesktopDuplicator は、DisplayManager から受け取った Display 情報のみを使用して
// 初期化される。これにより、GPU 種類を意識せずにキャプチャを行える。
// 既存の Initialize(ID3D11Device*, ID3D11DeviceContext*, int) も維持し、
// 段階的な移行を可能にする。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include "capture/display_manager.h"
#include "gpu/d3d11_device.h"

using Microsoft::WRL::ComPtr;

/// @brief キャプチャされたフレーム情報
struct CapturedFrame {
    ComPtr<ID3D11Texture2D> texture;       // GPUメモリ上のテクスチャ
    DXGI_OUTDUPL_FRAME_INFO frame_info;    // Desktop Duplication フレーム情報
    uint32_t width;                        // フレーム幅
    uint32_t height;                       // フレーム高さ
    bool has_new_frame;                    // 新しいフレームがあるか（画面変化検出）
};

/// @brief Desktop Duplication API ラッパークラス
/// Windows 8 以降の DXGI Output Duplication を使用して
/// デスクトップ画面をGPUメモリ上で効率的にキャプチャする
class DesktopDuplicator {
public:
    DesktopDuplicator() = default;
    ~DesktopDuplicator();

    // コピー禁止・ムーブ許可
    DesktopDuplicator(const DesktopDuplicator&) = delete;
    DesktopDuplicator& operator=(const DesktopDuplicator&) = delete;
    DesktopDuplicator(DesktopDuplicator&&) noexcept = default;
    DesktopDuplicator& operator=(DesktopDuplicator&&) noexcept = default;

    /// @brief DisplayManager から受け取った Display 情報で Desktop Duplication を初期化する
    /// @param display DisplayManager から取得した Display 情報
    /// @return 成功時 true
    /// @details 内部で D3D11DeviceManager を作成し、指定アダプタのデバイスを生成して
    ///          Desktop Duplication を開始する。将来的なモニター切替に対応するため、
    ///          Display 情報を保持する。
    bool Initialize(const yamanoha::Display& display);

    /// @brief Desktop Duplication を初期化する（後方互換用）
    /// @param device D3D11デバイス（外部所有、呼び出し側がライフタイム管理）
    /// @param context D3D11デバイスコンテキスト（外部所有）
    /// @param output_index キャプチャ対象の出力インデックス（デフォルト: 0）
    /// @return 成功時 true
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int output_index = 0);

    /// @brief 次のフレームを取得する
    /// @param frame 取得結果の格納先
    /// @param timeout_ms タイムアウト（ミリ秒、デフォルト: 16ms ≒ 60fps）
    /// @return 成功時 true（タイムアウト時も has_new_frame=false で true を返す）
    bool AcquireFrame(CapturedFrame& frame, uint32_t timeout_ms = 16);

    /// @brief 取得中のフレームを解放する
    /// AcquireFrame 成功後、次の AcquireFrame 前に必ず呼ぶこと
    void ReleaseFrame();

    /// @brief Desktop Duplication を再初期化する
    /// ACCESS_LOST 等のエラー発生時に呼び出す
    /// @return 成功時 true
    bool Reinitialize();

    /// @brief 全リソースを解放する
    void Shutdown();

    /// @brief 初期化済みかどうか
    bool IsInitialized() const { return initialized_; }

    /// @brief フォールバックモード（ダミーテクスチャ）かどうか
    bool IsFallbackMode() const { return use_fallback_; }

    /// @brief キャプチャ対象の幅
    uint32_t GetWidth() const { return width_; }

    /// @brief キャプチャ対象の高さ
    uint32_t GetHeight() const { return height_; }

    /// @brief キャプチャ用 D3D11 デバイスを取得する
    /// @return 内部で作成したデバイス。外部所有のデバイスを使用した場合は nullptr
    ID3D11Device* GetDevice() const { return device_.Get(); }

    /// @brief キャプチャ用 D3D11 デバイスコンテキストを取得する
    /// @return 内部で作成したコンテキスト。外部所有のデバイスを使用した場合は nullptr
    ID3D11DeviceContext* GetContext() const { return context_.Get(); }

private:
    // D3D11 デバイス・コンテキスト
    // Initialize(const Display&) 使用時は内部で作成した D3D11DeviceManager が所有する。
    // Initialize(ID3D11Device*, ID3D11DeviceContext*, int) 使用時は外部所有の参照を保持する。
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    // 内部でデバイスを所有する場合の D3D11DeviceManager
    // 外部所有の場合は nullptr
    std::unique_ptr<yamanoha::D3D11DeviceManager> owned_device_manager_;

    // Desktop Duplication API リソース
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<IDXGIOutput1> output_;

    // キャプチャ対象の解像度
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // 初期化時に保存する出力インデックス（再初期化用）
    int output_index_ = 0;

    // 初期化状態フラグ
    bool initialized_ = false;

    // フレームが取得中かどうか（ReleaseFrame 呼び忘れ防止）
    bool frame_acquired_ = false;

    // テクスチャデスクリプタのキャッシュ（初回フレームで取得、以降再利用）
    D3D11_TEXTURE2D_DESC cached_desc_{};
    bool desc_cached_ = false;

    // デバッグ・フォールバック関連
    bool use_fallback_ = false;                       // フォールバック（ダミーテクスチャ）モードフラグ
    ComPtr<ID3D11Texture2D> fallback_texture_;        // デバッグ用ダミーテクスチャ
    uint32_t fallback_frame_count_ = 0;               // アニメーション用のフレームカウンタ
};

