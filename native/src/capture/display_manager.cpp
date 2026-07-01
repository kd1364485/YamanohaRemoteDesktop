// ============================================================================
// display_manager.cpp
// ディスプレイ列挙・管理の実装
//
// 設計方針:
// - DisplayManager は DXGI ファクトリを保持し、Refresh() で最新状態を取得する。
// - 各 Display には DisplayID と GPU ベンダー情報を付与し、将来のモニター切替や
//   マルチモニター対応の基盤とする。
// - ベンダーIDからの GpuVendor 判定は DisplayManager が一元管理し、CaptureEngine は
//   必要に応じて Display::vendor を参照するのみとする。
// ============================================================================

#include "display_manager.h"

#include <cstdio>

#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace yamanoha {

// ---------------------------------------------------------------------------
// コンストラクタ / デストラクタ
// ---------------------------------------------------------------------------
DisplayManager::DisplayManager() {
    // DXGI ファクトリを作成して保持する。
    // これにより Refresh() 呼び出し時にファクトリを再作成するオーバーヘッドを削減する。
    CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
}

DisplayManager::~DisplayManager() = default;

// ---------------------------------------------------------------------------
// ベンダーID から GpuVendor を判定
// ---------------------------------------------------------------------------
GpuVendor DisplayManager::GetGpuVendor(uint32_t vendor_id) {
    switch (vendor_id) {
        case 0x8086:
            return GpuVendor::Intel;
        case 0x10DE:
            return GpuVendor::Nvidia;
        case 0x1002:
        case 0x1022:
            return GpuVendor::Amd;
        default:
            return GpuVendor::Unknown;
    }
}

// ---------------------------------------------------------------------------
// ベンダー名を取得（デバッグ用）
// ---------------------------------------------------------------------------
static const char* VendorToString(GpuVendor vendor) {
    switch (vendor) {
        case GpuVendor::Intel:  return "Intel";
        case GpuVendor::Nvidia: return "NVIDIA";
        case GpuVendor::Amd:    return "AMD";
        default:                return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// 全ディスプレイの列挙 (static ヘルパー)
// ---------------------------------------------------------------------------
std::vector<Display> DisplayManager::EnumerateDisplays() {
    DisplayManager mgr;
    mgr.Refresh();
    return mgr.GetDisplays();
}

// ---------------------------------------------------------------------------
// 全ディスプレイの再列挙
// ---------------------------------------------------------------------------
bool DisplayManager::Refresh() {
    displays_.clear();

    if (!factory_) {
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) {
            std::printf("[DisplayManager] DXGIFactory1 の作成に失敗: 0x%08lX\n", hr);
            return false;
        }
    }

    std::printf("[DisplayManager] --- アダプタと出力の列挙開始 ---\n");

    // 全アダプタを列挙
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapter_idx = 0;
         factory_->EnumAdapters1(adapter_idx, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++adapter_idx) {

        // アダプタ情報を取得
        DXGI_ADAPTER_DESC1 adapter_desc{};
        HRESULT hr = adapter->GetDesc1(&adapter_desc);
        if (FAILED(hr)) {
            std::printf("[DisplayManager] アダプタ %u の情報取得に失敗: 0x%08lX\n",
                        adapter_idx, hr);
            adapter.Reset();
            continue;
        }

        // ソフトウェアアダプタ (WARP) はスキップ
        if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter.Reset();
            continue;
        }

        const GpuVendor vendor = GetGpuVendor(adapter_desc.VendorId);

        std::printf("アダプタ [%u] : %ls (Vendor: %s, LUID: %08lX-%08lX)\n",
                    adapter_idx, adapter_desc.Description,
                    VendorToString(vendor),
                    adapter_desc.AdapterLuid.HighPart, adapter_desc.AdapterLuid.LowPart);

        // このアダプタの全出力を列挙
        ComPtr<IDXGIOutput> output;
        for (UINT output_idx = 0;
             adapter->EnumOutputs(output_idx, &output) != DXGI_ERROR_NOT_FOUND;
             ++output_idx) {

            // 出力の詳細情報を取得
            DXGI_OUTPUT_DESC output_desc{};
            hr = output->GetDesc(&output_desc);
            if (FAILED(hr)) {
                std::printf("  出力 [%u] : 情報取得に失敗: 0x%08lX\n", output_idx, hr);
                output.Reset();
                continue;
            }

            std::printf("  出力 [%u] : %ls (Attached: %s, Coordinates: %ld,%ld to %ld,%ld)\n",
                        output_idx, output_desc.DeviceName,
                        output_desc.AttachedToDesktop ? "Yes" : "No",
                        output_desc.DesktopCoordinates.left, output_desc.DesktopCoordinates.top,
                        output_desc.DesktopCoordinates.right, output_desc.DesktopCoordinates.bottom);

            // デスクトップにアタッチされていない出力はスキップ
            // 将来的に Windows.Graphics.Capture 対応時には AttachedToDesktop=false も
            // 対象にする可能性があるため、ここでは明示的に除外している。
            if (!output_desc.AttachedToDesktop) {
                output.Reset();
                continue;
            }

            // DisplayID を生成
            // 例: \\.\DISPLAY1 -> "DISPLAY1"
            std::wstring display_id = output_desc.DeviceName;
            const size_t last_backslash = display_id.find_last_of(L'\\');
            if (last_backslash != std::wstring::npos && last_backslash + 1 < display_id.size()) {
                display_id = display_id.substr(last_backslash + 1);
            }

            // ディスプレイ情報を構築
            Display info{};
            info.display_id = display_id;
            info.name = output_desc.DeviceName;
            info.adapter_name = adapter_desc.Description;
            info.vendor = vendor;
            info.adapter_index = static_cast<int>(adapter_idx);
            info.output_index = static_cast<int>(output_idx);
            info.adapter_luid = adapter_desc.AdapterLuid;
            info.bounds = output_desc.DesktopCoordinates;
            info.width = static_cast<uint32_t>(
                output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
            info.height = static_cast<uint32_t>(
                output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);
            info.rotation = output_desc.Rotation;
            info.is_attached = (output_desc.AttachedToDesktop != FALSE);

            // リフレッシュレートは Win32 API で取得
            info.refresh_rate = GetRefreshRate(output_desc.DeviceName);

            // プライマリディスプレイの判定: 左上が (0,0) のものがプライマリ
            // 将来は DXGI/Win32 API からより確実な方法を使用する可能性がある。
            info.is_primary = (output_desc.DesktopCoordinates.left == 0 &&
                               output_desc.DesktopCoordinates.top == 0);

            displays_.push_back(std::move(info));
            output.Reset();
        }

        adapter.Reset();
    }

    std::printf("[DisplayManager] --- アダプタと出力の列挙終了 (%zu 台) ---\n\n",
                displays_.size());
    return true;
}

// ---------------------------------------------------------------------------
// DisplayID からディスプレイを検索
// ---------------------------------------------------------------------------
const Display* DisplayManager::FindDisplayById(const std::wstring& display_id) const {
    for (const auto& display : displays_) {
        if (display.display_id == display_id) {
            return &display;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// プライマリディスプレイを取得
// ---------------------------------------------------------------------------
const Display* DisplayManager::GetPrimaryDisplay() const {
    for (const auto& display : displays_) {
        if (display.is_primary) {
            return &display;
        }
    }
    // プライマリが見つからない場合は先頭を返す
    return displays_.empty() ? nullptr : &displays_.front();
}

// ---------------------------------------------------------------------------
// 指定アダプタインデックスの DXGIAdapter1 を取得
// ---------------------------------------------------------------------------
ComPtr<IDXGIAdapter1> DisplayManager::GetAdapter(int adapter_index) const {
    if (!factory_) {
        return nullptr;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        if (i == static_cast<UINT>(adapter_index)) {
            return adapter;
        }
        adapter.Reset();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 指定ディスプレイの IDXGIOutput1 を取得
// ---------------------------------------------------------------------------
ComPtr<IDXGIOutput1> DisplayManager::GetOutput(const Display& display) const {
    ComPtr<IDXGIAdapter1> adapter = GetAdapter(display.adapter_index);
    if (!adapter) {
        return nullptr;
    }

    ComPtr<IDXGIOutput> output;
    for (UINT i = 0;
         adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        if (i == static_cast<UINT>(display.output_index)) {
            ComPtr<IDXGIOutput1> output1;
            if (SUCCEEDED(output.As(&output1))) {
                return output1;
            }
            return nullptr;
        }
        output.Reset();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// ディスプレイ情報のデバッグ表示
// ---------------------------------------------------------------------------
void DisplayManager::PrintDisplayInfo() const {
    std::printf("========================================\n");
    std::printf("  検出されたディスプレイ一覧 (%zu 台)\n", displays_.size());
    std::printf("========================================\n");

    for (size_t i = 0; i < displays_.size(); ++i) {
        const auto& d = displays_[i];
        std::printf("\n[%zu] DisplayID: %ls\n", i, d.display_id.c_str());
        std::printf("    名前     : %ls\n", d.name.c_str());
        std::printf("    アダプタ : %ls (index=%d, vendor=%s)\n",
                    d.adapter_name.c_str(), d.adapter_index, VendorToString(d.vendor));
        std::printf("    出力     : index=%d\n", d.output_index);
        std::printf("    解像度   : %u x %u\n", d.width, d.height);
        std::printf("    座標     : (%ld, %ld) - (%ld, %ld)\n",
                    d.bounds.left, d.bounds.top, d.bounds.right, d.bounds.bottom);
        std::printf("    回転     : %d\n", static_cast<int>(d.rotation));
        std::printf("    FPS      : %.1f Hz\n", d.refresh_rate);
        std::printf("    プライマリ: %s\n", d.is_primary ? "はい" : "いいえ");
    }

    std::printf("\n========================================\n");
}

// ---------------------------------------------------------------------------
// リフレッシュレートの取得 (Win32 API)
// ---------------------------------------------------------------------------
float DisplayManager::GetRefreshRate(const wchar_t* device_name) {
    DEVMODEW devmode{};
    devmode.dmSize = sizeof(DEVMODEW);

    // 現在のディスプレイ設定を取得
    if (EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, &devmode)) {
        if (devmode.dmDisplayFrequency > 0 && devmode.dmDisplayFrequency != 1) {
            return static_cast<float>(devmode.dmDisplayFrequency);
        }
    }

    // 取得できなかった場合はデフォルト値
    std::printf("[DisplayManager] リフレッシュレート取得失敗: %ls (60Hz をデフォルト使用)\n",
                device_name);
    return 60.0f;
}

} // namespace yamanoha
