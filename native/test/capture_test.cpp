// ============================================================================
// capture_test.cpp
// キャプチャ機能のテストプログラム
// ディスプレイ列挙 → 選択 → キャプチャ → BMP保存 → FPS計測
// ============================================================================

#include "../src/capture/display_manager.h"
#include "../src/capture/desktop_duplicator.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// BMP ファイル保存（BGRA → BMP）
// ---------------------------------------------------------------------------
static bool SaveBMP(const char* filename, const uint8_t* data,
                    uint32_t width, uint32_t height, uint32_t src_stride) {
    // BMP は下から上に格納するため、上下反転してコピー
    const uint32_t bmp_stride = ((width * 3 + 3) & ~3u);  // 24bit BMP の行幅（4バイト境界）
    const uint32_t image_size = bmp_stride * height;
    const uint32_t file_size = 54 + image_size;

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::printf("[Test] BMP ファイルを開けません: %s\n", filename);
        return false;
    }

    // BMP ファイルヘッダ（14バイト）
    uint8_t bfh[14] = {};
    bfh[0] = 'B'; bfh[1] = 'M';
    *reinterpret_cast<uint32_t*>(&bfh[2]) = file_size;
    *reinterpret_cast<uint32_t*>(&bfh[10]) = 54;
    file.write(reinterpret_cast<const char*>(bfh), 14);

    // BMP 情報ヘッダ（40バイト）
    uint8_t bih[40] = {};
    *reinterpret_cast<uint32_t*>(&bih[0]) = 40;
    *reinterpret_cast<int32_t*>(&bih[4]) = static_cast<int32_t>(width);
    *reinterpret_cast<int32_t*>(&bih[8]) = static_cast<int32_t>(height);
    *reinterpret_cast<uint16_t*>(&bih[12]) = 1;     // プレーン数
    *reinterpret_cast<uint16_t*>(&bih[14]) = 24;    // ビット深度
    *reinterpret_cast<uint32_t*>(&bih[20]) = image_size;
    file.write(reinterpret_cast<const char*>(bih), 40);

    // ピクセルデータ（BMP は下→上の順序）
    std::vector<uint8_t> row_buf(bmp_stride, 0);
    for (int32_t y = static_cast<int32_t>(height) - 1; y >= 0; --y) {
        const uint8_t* src_row = data + y * src_stride;
        for (uint32_t x = 0; x < width; ++x) {
            // BGRA → BGR（アルファは捨てる）
            row_buf[x * 3 + 0] = src_row[x * 4 + 0];  // B
            row_buf[x * 3 + 1] = src_row[x * 4 + 1];  // G
            row_buf[x * 3 + 2] = src_row[x * 4 + 2];  // R
        }
        file.write(reinterpret_cast<const char*>(row_buf.data()),
                   static_cast<std::streamsize>(bmp_stride));
    }

    file.close();
    std::printf("[Test] BMP 保存完了: %s (%u x %u)\n", filename, width, height);
    return true;
}

// ---------------------------------------------------------------------------
// D3D11 デバイス作成ヘルパー
// ---------------------------------------------------------------------------
static bool CreateDevice(int adapter_index,
                          ComPtr<ID3D11Device>& device,
                          ComPtr<ID3D11DeviceContext>& context) {
    // アダプタを取得
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::printf("[Test] DXGIFactory1 の作成に失敗: 0x%08lX\n", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(static_cast<UINT>(adapter_index), &adapter);
    if (FAILED(hr)) {
        std::printf("[Test] アダプタ %d の取得に失敗: 0x%08lX\n", adapter_index, hr);
        return false;
    }

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL achieved_level;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        feature_levels,
        _countof(feature_levels),
        D3D11_SDK_VERSION,
        &device,
        &achieved_level,
        &context
    );

    if (FAILED(hr)) {
        std::printf("[Test] D3D11デバイスの作成に失敗: 0x%08lX\n", hr);
        return false;
    }

    std::printf("[Test] D3D11デバイス作成成功 (FeatureLevel: 0x%X)\n",
                static_cast<int>(achieved_level));
    return true;
}

// ---------------------------------------------------------------------------
// GPU テクスチャから CPU バッファにコピー
// ---------------------------------------------------------------------------
static bool CopyTextureToCPU(ID3D11Device* device, ID3D11DeviceContext* context,
                              ID3D11Texture2D* src_texture,
                              std::vector<uint8_t>& out_buffer,
                              uint32_t& out_width, uint32_t& out_height,
                              uint32_t& out_stride) {
    // ソーステクスチャの情報を取得
    D3D11_TEXTURE2D_DESC desc{};
    src_texture->GetDesc(&desc);

    out_width = desc.Width;
    out_height = desc.Height;

    // ステージングテクスチャを作成
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr)) {
        std::printf("[Test] ステージングテクスチャ作成失敗: 0x%08lX\n", hr);
        return false;
    }

    // GPU → ステージング にコピー
    context->CopyResource(staging.Get(), src_texture);

    // CPU 側にマップ
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::printf("[Test] テクスチャのマップに失敗: 0x%08lX\n", hr);
        return false;
    }

    // バッファにコピー
    out_stride = out_width * 4;  // BGRA
    out_buffer.resize(static_cast<size_t>(out_stride) * out_height);

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = out_buffer.data();

    if (mapped.RowPitch == out_stride) {
        std::memcpy(dst, src, out_buffer.size());
    } else {
        for (uint32_t row = 0; row < out_height; ++row) {
            std::memcpy(dst + row * out_stride, src + row * mapped.RowPitch, out_stride);
        }
    }

    context->Unmap(staging.Get(), 0);
    return true;
}

// ---------------------------------------------------------------------------
// メイン関数
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // コンソール出力を UTF-8 に設定
    SetConsoleOutputCP(CP_UTF8);

    std::printf("========================================\n");
    std::printf("  YamanohaRemoteDesktop キャプチャテスト\n");
    std::printf("========================================\n\n");

    // ステップ 1: COM 初期化
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        std::printf("[Test] COM 初期化に失敗: 0x%08lX\n", hr);
        return 1;
    }
    std::printf("[Test] COM 初期化完了\n\n");

    // ステップ 2: ディスプレイ列挙
    auto displays = DisplayManager::EnumerateDisplays();
    if (displays.empty()) {
        std::printf("[Test] ディスプレイが見つかりません\n");
        CoUninitialize();
        return 1;
    }

    DisplayManager::PrintDisplayInfo(displays);

    // ステップ 3: アダプタと出力の選択
    int selected_adapter = 0;
    int selected_output = 0;
    std::wstring selected_name = L"Manual Target";

    if (argc > 2) {
        selected_adapter = std::atoi(argv[1]);
        selected_output = std::atoi(argv[2]);
        std::printf("[Test] コマンドライン指定: アダプタ=%d, 出力=%d を使用します\n",
                    selected_adapter, selected_output);
    } else {
        int selected_index = -1;
        if (argc > 1) {
            selected_index = std::atoi(argv[1]);
            if (selected_index < 0 || selected_index >= static_cast<int>(displays.size())) {
                std::printf("[Test] 引数で指定されたインデックスは無効です: %d\n", selected_index);
                selected_index = -1;
            }
        }

        if (selected_index == -1 && displays.size() > 1) {
            std::printf("\nキャプチャするディスプレイ番号を入力してください [0-%zu]: ",
                        displays.size() - 1);
            if (std::scanf("%d", &selected_index) != 1 ||
                selected_index < 0 ||
                selected_index >= static_cast<int>(displays.size())) {
                std::printf("[Test] 無効な選択。デフォルト (0) を使用します\n");
                selected_index = 0;
            }
        } else if (selected_index == -1) {
            selected_index = 0;
        }

        const auto& target = displays[selected_index];
        selected_adapter = target.adapter_index;
        selected_output = target.output_index;
        selected_name = target.name;
        std::printf("\n[Test] ディスプレイ %d を選択: %ls (%u x %u)\n",
                    selected_index, target.name.c_str(), target.width, target.height);
    }

    // ステップ 4: D3D11 デバイス作成
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateDevice(selected_adapter, device, context)) {
        CoUninitialize();
        return 1;
    }

    // ステップ 5: DesktopDuplicator 初期化
    DesktopDuplicator duplicator;
    if (!duplicator.Initialize(device.Get(), context.Get(), selected_output)) {
        std::printf("[Test] DesktopDuplicator の初期化に失敗\n");
        CoUninitialize();
        return 1;
    }

    // ステップ 6: 10フレームキャプチャ、最初のフレームを BMP 保存
    std::printf("\n--- 10フレーム キャプチャテスト ---\n");
    bool first_frame_saved = false;

    for (int i = 0; i < 10; ++i) {
        CapturedFrame frame{};
        if (!duplicator.AcquireFrame(frame, 100)) {
            std::printf("[Test] フレーム %d: 取得失敗\n", i);
            continue;
        }

        if (frame.has_new_frame && frame.texture) {
            std::printf("[Test] フレーム %d: 新しいフレーム取得 (%u x %u)\n",
                        i, frame.width, frame.height);

            // 最初のフレームを BMP 保存
            if (!first_frame_saved) {
                std::vector<uint8_t> pixel_data;
                uint32_t w, h, stride;
                if (CopyTextureToCPU(device.Get(), context.Get(),
                                      frame.texture.Get(), pixel_data, w, h, stride)) {
                    SaveBMP("capture_test.bmp", pixel_data.data(), w, h, stride);
                    first_frame_saved = true;
                }
            }
        } else {
            std::printf("[Test] フレーム %d: 画面変化なし\n", i);
        }

        duplicator.ReleaseFrame();
        Sleep(16);  // 約 60fps 相当の待機
    }

    // ステップ 7: 100フレームの FPS 計測
    std::printf("\n--- 100フレーム FPS 計測 ---\n");

    int successful_frames = 0;
    int new_frames = 0;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 100; ++i) {
        CapturedFrame frame{};
        if (duplicator.AcquireFrame(frame, 16)) {
            ++successful_frames;
            if (frame.has_new_frame) {
                ++new_frames;
            }
            duplicator.ReleaseFrame();
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    double fps = (elapsed_ms > 0.0) ? (successful_frames * 1000.0 / elapsed_ms) : 0.0;

    std::printf("[Test] 結果:\n");
    std::printf("  総フレーム数    : 100\n");
    std::printf("  取得成功       : %d\n", successful_frames);
    std::printf("  新フレーム     : %d\n", new_frames);
    std::printf("  経過時間       : %.1f ms\n", elapsed_ms);
    std::printf("  平均 FPS       : %.1f\n", fps);

    // ステップ 8: クリーンアップ
    std::printf("\n--- クリーンアップ ---\n");
    duplicator.Shutdown();
    context.Reset();
    device.Reset();
    CoUninitialize();

    std::printf("[Test] テスト完了\n");
    return 0;
}
