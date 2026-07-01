// ============================================================================
// capture_engine.cpp
// キャプチャエンジンクラスの実装
//
// 設計方針:
// - CaptureEngine は DisplayManager から受け取った Display 情報のみに基づいて動作する。
// - 指定ディスプレイのキャプチャのみを担当し、エンコードや送信は行わない。
// - 取得したフレームは呼び出し側で ReleaseFrame() により解放する。
// ============================================================================

#include "capture/capture_engine.h"

#include <cstdio>

namespace yamanoha {

// ---------------------------------------------------------------------------
// コンストラクタ / デストラクタ
// ---------------------------------------------------------------------------
CaptureEngine::CaptureEngine() = default;

CaptureEngine::~CaptureEngine() {
    Stop();
}

// ---------------------------------------------------------------------------
// 指定ディスプレイでキャプチャを開始
// ---------------------------------------------------------------------------
bool CaptureEngine::Start(const Display& display) {
    Stop();

    display_ = display;
    width_ = display.width;
    height_ = display.height;

    std::printf("[CaptureEngine] 開始: DisplayID=%ls, アダプタ=%ls (index=%d), 出力=%d, %u x %u\n",
                display.display_id.c_str(),
                display.adapter_name.c_str(),
                display.adapter_index,
                display.output_index,
                width_, height_);

    // 1. キャプチャ用 D3D11 デバイスと Desktop Duplicator の初期化
    capture_device_manager_ = std::make_unique<D3D11DeviceManager>();
    if (!capture_device_manager_->Initialize(display.adapter_index)) {
        std::printf("[CaptureEngine] キャプチャ用 D3D11 デバイス初期化失敗\n");
        capture_device_manager_.reset();
        return false;
    }

    duplicator_ = std::make_unique<DesktopDuplicator>();
    if (!duplicator_->Initialize(capture_device_manager_->GetDevice(),
                                 capture_device_manager_->GetContext(),
                                 display.output_index)) {
        std::printf("[CaptureEngine] Desktop Duplication 初期化失敗\n");
        duplicator_.reset();
        capture_device_manager_.reset();
        return false;
    }

    if (duplicator_->IsFallbackMode()) {
        std::printf("[CaptureEngine] Desktop Duplication がフォールバックモードで動作中\n");
    }

    // Desktop Duplication の解像度を優先（ネイティブ解像度）
    width_ = duplicator_->GetWidth();
    height_ = duplicator_->GetHeight();

    running_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// 配信停止とリソース解放
// ---------------------------------------------------------------------------
void CaptureEngine::Stop() {
    if (duplicator_) {
        duplicator_->Shutdown();
        duplicator_.reset();
    }

    capture_device_manager_.reset();

    running_ = false;
    width_ = 0;
    height_ = 0;

    std::printf("[CaptureEngine] 停止しました\n");
}

// ---------------------------------------------------------------------------
// 1フレームを Desktop Duplication から取得
// ---------------------------------------------------------------------------
bool CaptureEngine::AcquireFrame(CapturedFrame& out_frame, uint32_t timeout_ms) {
    if (!running_ || !duplicator_) {
        return false;
    }

    return duplicator_->AcquireFrame(out_frame, timeout_ms);
}

// ---------------------------------------------------------------------------
// 取得中のフレームを解放
// ---------------------------------------------------------------------------
void CaptureEngine::ReleaseFrame() {
    if (duplicator_) {
        duplicator_->ReleaseFrame();
    }
}

// ---------------------------------------------------------------------------
// キャプチャ用 D3D11 デバイスを取得
// ---------------------------------------------------------------------------
ID3D11Device* CaptureEngine::GetDevice() const {
    if (capture_device_manager_) {
        return capture_device_manager_->GetDevice();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// キャプチャ用 D3D11 デバイスコンテキストを取得
// ---------------------------------------------------------------------------
ID3D11DeviceContext* CaptureEngine::GetContext() const {
    if (capture_device_manager_) {
        return capture_device_manager_->GetContext();
    }
    return nullptr;
}

} // namespace yamanoha
