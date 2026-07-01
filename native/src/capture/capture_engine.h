// ============================================================================
// capture_engine.h
// キャプチャエンジンクラス
//
// 設計方針:
// CaptureEngine は DisplayManager から受け取った Display 情報のみを使用し、
// 指定ディスプレイのキャプチャのみを担当する。
// エンコードや送信は FramePipeline 側で行う。
//
// 将来的な拡張:
// - Windows Graphics Capture 対応
// - マルチモニター同時キャプチャ
// - HDR / AV1 対応
//
// 責務:
// - DisplayManager から Display 情報を取得
// - キャプチャ用 D3D11 デバイスと DesktopDuplicator の初期化
// - フレームのキャプチャ（AcquireFrame / ReleaseFrame）
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "capture/desktop_duplicator.h"
#include "capture/display_manager.h"
#include "gpu/d3d11_device.h"

using Microsoft::WRL::ComPtr;

namespace yamanoha {

class CaptureEngine {
public:
    CaptureEngine();
    ~CaptureEngine();

    // コピー禁止・ムーブ許可
    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;
    CaptureEngine(CaptureEngine&&) noexcept = default;
    CaptureEngine& operator=(CaptureEngine&&) noexcept = default;

    /// @brief 指定ディスプレイでキャプチャを開始する
    /// @param display キャプチャ対象の Display 情報
    /// @return 成功時 true
    bool Start(const Display& display);

    /// @brief 配信を停止し、リソースを解放する
    void Stop();

    /// @brief 1フレームを Desktop Duplication から取得する
    /// @param out_frame 取得されたフレーム情報の格納先
    /// @param timeout_ms 取得タイムアウト（ミリ秒）
    /// @return フレーム取得に成功した場合は true
    /// @details 取得したフレームは ReleaseFrame() で明示的に解放する必要がある
    bool AcquireFrame(CapturedFrame& out_frame, uint32_t timeout_ms);

    /// @brief 取得中のフレームを解放する
    void ReleaseFrame();

    /// @brief キャプチャが実行中かどうか
    bool IsRunning() const { return running_; }

    /// @brief 現在のキャプチャ対象ディスプレイ情報を取得する
    const Display& GetDisplay() const { return display_; }

    /// @brief キャプチャ対象の幅を取得する
    uint32_t GetWidth() const { return width_; }

    /// @brief キャプチャ対象の高さを取得する
    uint32_t GetHeight() const { return height_; }

    /// @brief キャプチャ用 D3D11 デバイスを取得する
    ID3D11Device* GetDevice() const;

    /// @brief キャプチャ用 D3D11 デバイスコンテキストを取得する
    ID3D11DeviceContext* GetContext() const;

private:
    Display display_{};                 // 現在のキャプチャ対象ディスプレイ
    bool running_ = false;              // 実行中フラグ

    uint32_t width_ = 0;                // キャプチャ幅
    uint32_t height_ = 0;               // キャプチャ高さ

    // キャプチャパイプライン
    std::unique_ptr<D3D11DeviceManager> capture_device_manager_;
    std::unique_ptr<DesktopDuplicator> duplicator_;
};

} // namespace yamanoha
