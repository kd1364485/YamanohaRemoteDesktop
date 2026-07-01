#pragma once
// ============================================================================
// d3d11_device.h — D3D11 デバイス管理
// DXGIアダプタの列挙とD3D11デバイスの作成を行う
// ============================================================================

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace yamanoha {

// アダプタ情報構造体
struct AdapterInfo {
    std::wstring name;           // アダプタ名（例: "NVIDIA GeForce RTX 4090"）
    uint64_t dedicated_vram;     // 専用VRAM（バイト）
    uint64_t shared_vram;        // 共有メモリ（バイト）
    LUID luid;                   // ローカルユニークID
    uint32_t vendor_id;          // ベンダーID: 0x8086=Intel, 0x10DE=NVIDIA, 0x1002=AMD
    int index;                   // アダプタインデックス
};

// D3D11デバイスの作成と管理を行うクラス
class D3D11DeviceManager {
public:
    D3D11DeviceManager() = default;
    ~D3D11DeviceManager() = default;

    // コピー禁止・ムーブ許可
    D3D11DeviceManager(const D3D11DeviceManager&) = delete;
    D3D11DeviceManager& operator=(const D3D11DeviceManager&) = delete;
    D3D11DeviceManager(D3D11DeviceManager&&) noexcept = default;
    D3D11DeviceManager& operator=(D3D11DeviceManager&&) noexcept = default;

    // 全アダプタの情報を列挙して返す
    static std::vector<AdapterInfo> EnumerateAdapters();

    // 指定インデックスのアダプタでD3D11デバイスを作成する
    // adapterIndex: 0がデフォルト（プライマリGPU）
    bool Initialize(int adapterIndex = 0);

    // 外部から渡されたアダプタでD3D11デバイスを作成する
    bool InitializeForAdapter(IDXGIAdapter1* adapter);

    // アクセサ（生ポインタを返す。所有権は移譲しない）
    [[nodiscard]] ID3D11Device* GetDevice() const { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* GetContext() const { return context_.Get(); }
    [[nodiscard]] IDXGIAdapter1* GetAdapter() const { return adapter_.Get(); }

    // 使用中アダプタの LUID を取得する（アダプタ比較用）
    [[nodiscard]] LUID GetAdapterLuid() const {
        LUID luid{};
        if (adapter_) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter_->GetDesc1(&desc))) {
                luid = desc.AdapterLuid;
            }
        }
        return luid;
    }

    // デバイスが有効かどうか
    [[nodiscard]] bool IsValid() const { return device_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;           // D3D11デバイス
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;    // デバイスコンテキスト
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;          // 使用中のDXGIアダプタ
};

} // namespace yamanoha
