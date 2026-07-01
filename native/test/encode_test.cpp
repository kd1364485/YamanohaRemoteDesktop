// ============================================================================
// encode_test.cpp
// キャプチャ → NVENC H.264 エンコードの統合テストプログラム
// ============================================================================
#include "capture/display_manager.h"
#include "capture/desktop_duplicator.h"
#include "encoder/nvenc_encoder.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <d3d11.h>
#include <windows.h>

using Microsoft::WRL::ComPtr;

// D3D11 デバイス作成ヘルパー
static bool CreateD3DDevice(int adapter_index,
                             ComPtr<ID3D11Device>& device,
                             ComPtr<ID3D11DeviceContext>& context) {
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

int main(int argc, char* argv[]) {
    // コンソール出力を UTF-8 に設定
    SetConsoleOutputCP(CP_UTF8);
    // 標準出力のバッファリングを無効化（ハング時のログ消失防止）
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("========================================\n");
    std::printf("  YamanohaRemoteDesktop エンコードテスト\n");
    std::printf("========================================\n\n");

    // 1. COM 初期化
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        std::printf("[Test] COM 初期化に失敗: 0x%08lX\n", hr);
        return 1;
    }

    // 2. ディスプレイ列挙
    auto displays = yamanoha::DisplayManager::EnumerateDisplays();
    
    int selected_adapter = 0;
    int selected_output = 0;

    if (argc > 2) {
        selected_adapter = std::atoi(argv[1]);
        selected_output = std::atoi(argv[2]);
        std::printf("[Test] コマンドライン指定: アダプタ=%d, 出力=%d を使用します\n",
                    selected_adapter, selected_output);
    } else {
        int selected_index = 0;
        if (argc > 1) {
            selected_index = std::atoi(argv[1]);
            if (selected_index < 0 || selected_index >= static_cast<int>(displays.size())) {
                selected_index = 0;
            }
        }
        if (!displays.empty()) {
            const auto& target = displays[selected_index];
            selected_adapter = target.adapter_index;
            selected_output = target.output_index;
            std::printf("[Test] ディスプレイ %d を自動選択: %ls\n", selected_index, target.name.c_str());
        }
    }

    // 3. D3D11 デバイスの作成
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateD3DDevice(selected_adapter, device, context)) {
        CoUninitialize();
        return 1;
    }

    // 4. DesktopDuplicator の初期化（失敗時は自動でデバッグフォールバックになります）
    DesktopDuplicator duplicator;
    if (!duplicator.Initialize(device.Get(), context.Get(), selected_output)) {
        std::printf("[Test] DesktopDuplicator の初期化に失敗しました\n");
        CoUninitialize();
        return 1;
    }

    // 5. NvencEncoder の初期化
    yamanoha::NvencEncoder encoder;
    yamanoha::EncoderConfig config{};
    config.width = duplicator.GetWidth();
    config.height = duplicator.GetHeight();
    config.fps = 60;
    config.bitrate_kbps = 4000;       // 4 Mbps
    config.max_bitrate_kbps = 8000;   // 8 Mbps
    config.gop_size = 30;             // 0.5秒キーフレーム
    config.preset = 1;                // 最低遅延 P1
    config.zero_latency = true;       // 超低遅延

    if (!encoder.Initialize(device.Get(), config)) {
        std::printf("[Test] NvencEncoder の初期化に失敗しました。"
                    "NVIDIA GPUではないか、NVENC非対応の可能性があります。\n");
        duplicator.Shutdown();
        CoUninitialize();
        return 1;
    }

    // 出力ファイルのオープン
    const char* filename = "test_output.h264";
    std::ofstream out_file(filename, std::ios::binary);
    if (!out_file.is_open()) {
        std::printf("[Test] 出力ファイルを開けません: %s\n", filename);
        encoder.Shutdown();
        duplicator.Shutdown();
        CoUninitialize();
        return 1;
    }

    std::printf("\n--- 100フレームのエンコードテストを開始します ---\n");
    std::printf("出力ファイル: %s (%u x %u)\n", filename, config.width, config.height);

    int encoded_frames = 0;
    std::vector<uint8_t> bitstream;
    
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 100; ++i) {
        CapturedFrame frame{};
        if (!duplicator.AcquireFrame(frame, 16)) {
            std::printf("[Test] フレーム %d のキャプチャに失敗しました\n", i);
            continue;
        }

        if (frame.has_new_frame && frame.texture) {
            auto enc_start = std::chrono::high_resolution_clock::now();
            
            // エンコードの実行 (強制IDRなし)
            bool ok = encoder.EncodeFrame(frame.texture.Get(), false, bitstream);
            
            auto enc_end = std::chrono::high_resolution_clock::now();
            auto enc_duration = std::chrono::duration_cast<std::chrono::microseconds>(enc_end - enc_start).count();

            if (ok) {
                if (!bitstream.empty()) {
                    out_file.write(reinterpret_cast<const char*>(bitstream.data()), bitstream.size());
                    encoded_frames++;
                    
                    if (encoded_frames % 10 == 0) {
                        std::printf("[Test] %d フレームエンコード完了 (サイズ: %zu バイト, レイテンシ: %lld us)\n",
                                    encoded_frames, bitstream.size(), enc_duration);
                    }
                }
            } else {
                std::printf("[Test] フレーム %d のエンコードに失敗しました\n", i);
            }
        }

        duplicator.ReleaseFrame();
        
        // 60fps に近づけるため 16ms 待機
        Sleep(16);
    }

    // ストリームのフラッシュ
    std::printf("[Test] ストリームをフラッシュ中...\n");
    if (encoder.Flush(bitstream)) {
        if (!bitstream.empty()) {
            out_file.write(reinterpret_cast<const char*>(bitstream.data()), bitstream.size());
            std::printf("[Test] フラッシュデータ書き込み完了 (%zu バイト)\n", bitstream.size());
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    out_file.close();

    std::printf("\n--- テスト結果 ---\n");
    std::printf("  総フレーム数     : 100\n");
    std::printf("  エンコード完了   : %d\n", encoded_frames);
    std::printf("  総所要時間       : %lld ms\n", total_duration);
    std::printf("  平均 FPS         : %.1f\n", (encoded_frames * 1000.0f) / total_duration);

    // 6. クリーンアップ
    std::printf("\n--- クリーンアップ ---\n");
    encoder.Shutdown();
    duplicator.Shutdown();
    CoUninitialize();

    std::printf("[Test] 終了しました\n");
    return 0;
}
