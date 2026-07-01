// ============================================================================
// display_manager.h
// ディスプレイ列挙・管理クラス
//
// 設計方針:
// 本クラスは Google Remote Desktop のような「ディスプレイ単位で最適な
// キャプチャ経路を選択する」設計の基盤となる。DXGI を使用してシステム上の
// 全ディスプレイとその接続先 GPU を列挙し、Display 単位の情報を提供する。
//
// 責務:
// - ディスプレイ列挙
// - GPU ベンダー判定 (Intel / NVIDIA / AMD / Unknown)
// - DXGI Adapter / Output 取得
// - DisplayID 管理
// - プライマリディスプレイ判定
//
// CaptureEngine は DisplayManager から受け取った Display 情報のみを使用し、
// GPU 種類を直接意識しない。これにより、将来 GPU ごとに別 CaptureEngine へ
// 差し替えたり、Windows Graphics Capture へ切り替えたりする拡張が容易になる。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace yamanoha {

/// @brief GPU ベンダー種別
/// CaptureEngine が GPU 種類を意識しない設計にするため、ベンダー判定は
/// DisplayManager が一元管理する。
enum class GpuVendor {
    Unknown,
    Intel,
    Nvidia,
    Amd,
};

/// @brief ディスプレイ情報構造体
/// DisplayManager から CaptureEngine へ渡される唯一のディスプレイ情報。
/// 将来的なモニター切替・マルチモニター対応のために DisplayID を保持する。
struct Display {
    std::wstring display_id;       // DisplayID (例: "DISPLAY1")
    std::wstring name;             // ディスプレイ名（例: \\.\DISPLAY1）
    std::wstring adapter_name;     // GPUアダプタ名
    GpuVendor vendor;              // GPU ベンダー種別
    int adapter_index;             // DXGIアダプタインデックス
    int output_index;              // DXGIアウトプットインデックス
    LUID adapter_luid;             // アダプタLUID（安定した識別子）
    RECT bounds;                   // 画面領域（デスクトップ座標、ピクセル単位）
    uint32_t width;                // 水平解像度
    uint32_t height;               // 垂直解像度
    DXGI_MODE_ROTATION rotation;   // 回転状態（0°/90°/180°/270°）
    float refresh_rate;            // リフレッシュレート（Hz）
    bool is_primary;               // プライマリディスプレイかどうか
    bool is_attached;              // デスクトップにアタッチされているか
};

/// @brief ディスプレイ列挙・管理クラス
class DisplayManager {
public:
    DisplayManager();
    ~DisplayManager();

    // コピー禁止・ムーブ許可
    DisplayManager(const DisplayManager&) = delete;
    DisplayManager& operator=(const DisplayManager&) = delete;
    DisplayManager(DisplayManager&&) noexcept = default;
    DisplayManager& operator=(DisplayManager&&) noexcept = default;

    /// @brief システム上の全ディスプレイを再列挙する
    /// @return 成功時 true
    /// @details DXGI ファクトリを保持し、呼び出しごとに最新情報を取得する。
    ///          モニター切替・接続変更時に Refresh() を呼び出すことで状態を更新する。
    bool Refresh();

    /// @brief 現在保持しているディスプレイ一覧を取得する
    const std::vector<Display>& GetDisplays() const { return displays_; }

    /// @brief DisplayID からディスプレイを検索する
    /// @param display_id 検索する DisplayID (例: "DISPLAY1")
    /// @return 見つかった場合は Display へのポインタ。見つからない場合は nullptr
    const Display* FindDisplayById(const std::wstring& display_id) const;

    /// @brief プライマリディスプレイを取得する
    /// @return プライマリディスプレイへのポインタ。見つからない場合は nullptr
    const Display* GetPrimaryDisplay() const;

    /// @brief 指定アダプタインデックスの DXGIAdapter1 を取得する
    /// @param adapter_index DXGI アダプタインデックス
    /// @return 取得したアダプタ。失敗時は nullptr
    ComPtr<IDXGIAdapter1> GetAdapter(int adapter_index) const;

    /// @brief 指定ディスプレイの IDXGIOutput1 を取得する
    /// @param display 対象ディスプレイ
    /// @return 取得した出力。失敗時は nullptr
    ComPtr<IDXGIOutput1> GetOutput(const Display& display) const;

    /// @brief ディスプレイ情報をデバッグ表示する
    void PrintDisplayInfo() const;

    /// @brief システム上の全ディスプレイを列挙する (static ヘルパー)
    /// @return ディスプレイ情報のリスト
    /// @note 後方互換のため残しておく。新規コードは DisplayManager インスタンスを使用する。
    static std::vector<Display> EnumerateDisplays();

    /// @brief ベンダーID から GpuVendor を判定する
    /// @param vendor_id DXGI_ADAPTER_DESC1::VendorId
    /// @return 対応する GpuVendor
    static GpuVendor GetGpuVendor(uint32_t vendor_id);

private:
    /// @brief DXGI ファクトリ
    /// Refresh() ごとに再作成せず、インスタンスを保持することで効率化する。
    ComPtr<IDXGIFactory1> factory_;

    /// @brief 現在保持しているディスプレイ一覧
    std::vector<Display> displays_;

    /// @brief EnumDisplaySettingsW を使用してリフレッシュレートを取得する
    static float GetRefreshRate(const wchar_t* device_name);
};

} // namespace yamanoha
