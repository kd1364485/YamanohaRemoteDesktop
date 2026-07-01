// ============================================================================
// desktop_duplicator.cpp
// Desktop Duplication API ラッパーの実装
// ============================================================================

#include "desktop_duplicator.h"

#include <cstdio>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// デストラクタ
// ---------------------------------------------------------------------------
DesktopDuplicator::~DesktopDuplicator() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// 初期化: DisplayManager から受け取った Display 情報を使用
// ---------------------------------------------------------------------------
bool DesktopDuplicator::Initialize(const yamanoha::Display& display) {
    // DisplayManager から受け取った Display 情報を使用し、指定アダプタ上で
    // D3D11 デバイスを作成して Desktop Duplication を開始する。
    // これにより、CaptureEngine は GPU 種類を意識せずに Display だけを
    // 渡すことでキャプチャを開始できる。

    // 既存リソースがあれば解放
    Shutdown();

    // 内部で D3D11 デバイスを作成
    owned_device_manager_ = std::make_unique<yamanoha::D3D11DeviceManager>();
    if (!owned_device_manager_->Initialize(display.adapter_index)) {
        std::printf("[DesktopDuplicator] Display %ls の D3D11 デバイス作成に失敗\n",
                    display.display_id.c_str());
        owned_device_manager_.reset();
        return false;
    }

    return Initialize(
        owned_device_manager_->GetDevice(),
        owned_device_manager_->GetContext(),
        display.output_index);
}

// ---------------------------------------------------------------------------
// 初期化: D3D11 デバイスから DXGI 経由で OutputDuplication を取得
// ---------------------------------------------------------------------------
bool DesktopDuplicator::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                    int output_index) {
    if (!device || !context) {
        std::printf("[DesktopDuplicator] デバイスまたはコンテキストが null\n");
        return false;
    }

    // 既存リソースがあれば解放
    Shutdown();

    // デバイス・コンテキストの参照を保持
    device_ = device;
    context_ = context;
    output_index_ = output_index;

    // デバイスから DXGI デバイスを取得
    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (FAILED(hr)) {
        std::printf("[DesktopDuplicator] IDXGIDevice の取得に失敗: 0x%08lX\n", hr);
        return false;
    }

    // DXGI アダプタを取得
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        std::printf("[DesktopDuplicator] IDXGIAdapter の取得に失敗: 0x%08lX\n", hr);
        return false;
    }

    // 指定インデックスのアウトプットを取得
    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(static_cast<UINT>(output_index), &output);
    if (FAILED(hr)) {
        std::printf("[DesktopDuplicator] 出力 %d の列挙に失敗: 0x%08lX\n", output_index, hr);
        return false;
    }

    // IDXGIOutput1 にアップグレード（DuplicateOutput に必要）
    hr = output.As(&output_);
    if (FAILED(hr)) {
        std::printf("[DesktopDuplicator] IDXGIOutput1 へのアップグレードに失敗: 0x%08lX\n", hr);
        return false;
    }

    // 出力の情報を取得して解像度を保存
    DXGI_OUTPUT_DESC output_desc{};
    hr = output_->GetDesc(&output_desc);
    if (FAILED(hr)) {
        std::printf("[DesktopDuplicator] 出力情報の取得に失敗: 0x%08lX\n", hr);
        return false;
    }

    width_ = static_cast<uint32_t>(
        output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
    height_ = static_cast<uint32_t>(
        output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);

    // 使用アダプタ情報を取得してログ出力
    Microsoft::WRL::ComPtr<IDXGIAdapter> output_adapter;
    if (SUCCEEDED(output_->GetParent(IID_PPV_ARGS(&output_adapter)))) {
        DXGI_ADAPTER_DESC adapter_desc{};
        if (SUCCEEDED(output_adapter->GetDesc(&adapter_desc))) {
            std::printf("[DesktopDuplicator] 対象出力: %ls (アダプタ: %ls)\n",
                        output_desc.DeviceName, adapter_desc.Description);
        }
    }

    // Desktop Duplication を開始
    hr = output_->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        // よくあるエラーの診断メッセージ
        if (hr == E_ACCESSDENIED) {
            std::printf("[DesktopDuplicator] アクセス拒否: 管理者権限またはセッション0が必要\n");
        } else if (hr == DXGI_ERROR_UNSUPPORTED) {
            std::printf("[DesktopDuplicator] Desktop Duplication 非サポート環境 (GPU=%p, 出力アダプタとデバイスが一致しない可能性)\n", device_.Get());
        } else {
            std::printf("[DesktopDuplicator] DuplicateOutput に失敗: 0x%08lX\n", hr);
        }

        std::printf("[DesktopDuplicator] デバッグ用フォールバックモード（ダミーテクスチャ）で起動します。\n");
        
        // フォールバック用のテクスチャを作成
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = (width_ > 0) ? width_ : 1920;
        desc.Height = (height_ > 0) ? height_ : 1080;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        hr = device_->CreateTexture2D(&desc, nullptr, &fallback_texture_);
        if (FAILED(hr)) {
            std::printf("[DesktopDuplicator] フォールバックテクスチャの作成に失敗: 0x%08lX\n", hr);
            return false;
        }

        width_ = desc.Width;
        height_ = desc.Height;
        use_fallback_ = true;
        initialized_ = true;
        desc_cached_ = true;
        cached_desc_ = desc;

        std::printf("[DesktopDuplicator] フォールバックモード初期化成功: %u x %u\n", width_, height_);
        return true;
    }

    use_fallback_ = false;
    initialized_ = true;
    desc_cached_ = false;  // テクスチャデスクリプタキャッシュをリセット
    std::printf("[DesktopDuplicator] 初期化成功: %u x %u (出力 %d)\n",
                width_, height_, output_index);
    return true;
}

// ---------------------------------------------------------------------------
// フレーム取得
// ---------------------------------------------------------------------------
bool DesktopDuplicator::AcquireFrame(CapturedFrame& frame, uint32_t timeout_ms) {
    if (!initialized_) {
        std::printf("[DesktopDuplicator] 未初期化状態でフレーム取得が呼ばれた\n");
        return false;
    }

    if (use_fallback_) {
        if (frame_acquired_) {
            ReleaseFrame();
        }

        // 動的なダミーピクセルデータを生成 (BGRA8)
        std::vector<uint32_t> pixels(width_ * height_);
        uint32_t frame_idx = fallback_frame_count_++;

        for (uint32_t y = 0; y < height_; ++y) {
            for (uint32_t x = 0; x < width_; ++x) {
                // グラデーションの計算
                uint8_t r = static_cast<uint8_t>(x + frame_idx);
                uint8_t g = static_cast<uint8_t>(y - frame_idx);
                uint8_t b = static_cast<uint8_t>(x + y + frame_idx * 2);

                // アニメーション表示用ホワイトボックス (100x100)
                uint32_t box_x = (frame_idx * 5) % (width_ + 100) - 100;
                uint32_t box_y = (frame_idx * 3) % (height_ + 100) - 100;
                if (x >= box_x && x < box_x + 100 && y >= box_y && y < box_y + 100) {
                    r = 255; g = 255; b = 255;
                }

                // BGRAパック
                pixels[y * width_ + x] = (r << 16) | (g << 8) | b | 0xFF000000;
            }
        }

        // テクスチャへの転送
        context_->UpdateSubresource(fallback_texture_.Get(), 0, nullptr, pixels.data(), width_ * 4, 0);

        frame.texture = fallback_texture_;
        frame.width = width_;
        frame.height = height_;
        frame.has_new_frame = true;

        // ダミーのフレーム情報を設定
        frame.frame_info = {};
        frame.frame_info.AccumulatedFrames = 1;
        frame.frame_info.LastPresentTime.QuadPart = 1; // 常に新フレーム扱い

        frame_acquired_ = true;
        return true;
    }

    if (!duplication_) {
        std::printf("[DesktopDuplicator] duplication_ が null です\n");
        return false;
    }

    // 前回のフレームが解放されていなければ解放
    if (frame_acquired_) {
        ReleaseFrame();
    }

    // フレーム情報とリソースを取得
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    ComPtr<IDXGIResource> resource;

    HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &frame_info, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // タイムアウト: 画面に変化がなかった（正常）
        frame.has_new_frame = false;
        frame.width = width_;
        frame.height = height_;
        frame.texture.Reset();
        frame.frame_info = {};
        return true;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // デスクトップ切替（UAC、ロック画面等）→ 再初期化が必要
        std::printf("[DesktopDuplicator] ACCESS_LOST: 再初期化を試行\n");
        if (!Reinitialize()) {
            std::printf("[DesktopDuplicator] 再初期化に失敗\n");
            return false;
        }
        // 再初期化後は次回フレームで取得
        frame.has_new_frame = false;
        return true;
    }

    if (FAILED(hr)) {
        std::printf("[DesktopDuplicator] AcquireNextFrame に失敗: 0x%08lX\n", hr);
        return false;
    }

    // フレーム取得成功 — IDXGIResource から ID3D11Texture2D を取得
    // ※ Desktop Duplication API は毎回異なるリソースを返すため
    //   QueryInterface は毎フレーム必要。ただしテクスチャの DESC は
    //   初回にキャッシュして以降は再利用する。
    ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);
    if (FAILED(hr)) {
        std::printf("[DesktopDuplicator] ID3D11Texture2D の取得に失敗: 0x%08lX\n", hr);
        duplication_->ReleaseFrame();
        return false;
    }

    // テクスチャデスクリプタを初回のみキャッシュ
    if (!desc_cached_) {
        texture->GetDesc(&cached_desc_);
        desc_cached_ = true;
        std::printf("[DesktopDuplicator] テクスチャ DESC キャッシュ: %u x %u, Format=%d\n",
                    cached_desc_.Width, cached_desc_.Height, cached_desc_.Format);
    }

    // フレーム情報を保存
    last_frame_info_ = frame_info;
    frame_acquired_ = true;

    // 結果を格納
    frame.texture = std::move(texture);
    frame.frame_info = frame_info;
    frame.width = cached_desc_.Width;
    frame.height = cached_desc_.Height;
    // 新しいフレームかどうかは LastPresentTime で判定
    frame.has_new_frame = (frame_info.LastPresentTime.QuadPart != 0);

    return true;
}

// ---------------------------------------------------------------------------
// フレーム解放
// ---------------------------------------------------------------------------
void DesktopDuplicator::ReleaseFrame() {
    if (use_fallback_) {
        frame_acquired_ = false;
        return;
    }
    if (frame_acquired_ && duplication_) {
        HRESULT hr = duplication_->ReleaseFrame();
        if (FAILED(hr) && hr != DXGI_ERROR_ACCESS_LOST) {
            std::printf("[DesktopDuplicator] ReleaseFrame に失敗: 0x%08lX\n", hr);
        }
        frame_acquired_ = false;
    }
}

// ---------------------------------------------------------------------------
// カーソル情報の取得
// ---------------------------------------------------------------------------
CursorInfo DesktopDuplicator::GetCursorInfo() const {
    CursorInfo info{};
    info.position.x = last_frame_info_.PointerPosition.Position.x;
    info.position.y = last_frame_info_.PointerPosition.Position.y;
    info.visible = (last_frame_info_.PointerPosition.Visible != FALSE);
    return info;
}

// ---------------------------------------------------------------------------
// 再初期化
// ---------------------------------------------------------------------------
bool DesktopDuplicator::Reinitialize() {
    if (use_fallback_) {
        return true;
    }
    std::printf("[DesktopDuplicator] 再初期化中...\n");

    // 取得中のフレームがあれば解放
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }

    // Duplication リソースを解放
    duplication_.Reset();
    desc_cached_ = false;
    initialized_ = false;

    // デバイスとコンテキストが有効であることを確認
    if (!device_ || !context_) {
        std::printf("[DesktopDuplicator] デバイスが無効: 再初期化不可\n");
        return false;
    }

    // 少し待機してから再試行（デスクトップ切替の完了を待つ）
    Sleep(100);

    // 再度 DuplicateOutput を試行
    if (!output_) {
        // output_ が失われている場合はデバイスから再取得
        ComPtr<IDXGIDevice> dxgi_device;
        HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        if (FAILED(hr)) return false;

        ComPtr<IDXGIAdapter> adapter;
        hr = dxgi_device->GetAdapter(&adapter);
        if (FAILED(hr)) return false;

        ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(static_cast<UINT>(output_index_), &output);
        if (FAILED(hr)) return false;

        hr = output.As(&output_);
        if (FAILED(hr)) return false;
    }

    // 出力情報を再取得（解像度が変わっている可能性がある）
    DXGI_OUTPUT_DESC output_desc{};
    HRESULT hr = output_->GetDesc(&output_desc);
    if (SUCCEEDED(hr)) {
        width_ = static_cast<uint32_t>(
            output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
        height_ = static_cast<uint32_t>(
            output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);
    }

    hr = output_->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        std::printf("[DesktopDuplicator] 再初期化の DuplicateOutput に失敗: 0x%08lX\n", hr);
        return false;
    }

    initialized_ = true;
    std::printf("[DesktopDuplicator] 再初期化成功: %u x %u\n", width_, height_);
    return true;
}

// ---------------------------------------------------------------------------
// シャットダウン
// ---------------------------------------------------------------------------
void DesktopDuplicator::Shutdown() {
    if (use_fallback_) {
        fallback_texture_.Reset();
        use_fallback_ = false;
        fallback_frame_count_ = 0;
        frame_acquired_ = false;
    }

    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }

    duplication_.Reset();
    output_.Reset();
    context_.Reset();
    device_.Reset();

    // 内部で作成した D3D11DeviceManager があれば解放する
    owned_device_manager_.reset();

    width_ = 0;
    height_ = 0;
    initialized_ = false;
    desc_cached_ = false;
    last_frame_info_ = {};

    std::printf("[DesktopDuplicator] シャットダウン完了\n");
}
