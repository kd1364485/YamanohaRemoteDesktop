// ============================================================================
// d3d11_device.cpp — D3D11 デバイス管理の実装
// ============================================================================

#include "gpu/d3d11_device.h"

#include <cstdio>

// デバッグ出力用マクロ
#define LOG_ERROR(fmt, ...) \
    do { \
        char buf[512]; \
        std::snprintf(buf, sizeof(buf), "[yamanoha][ERROR] " fmt "\n", ##__VA_ARGS__); \
        OutputDebugStringA(buf); \
        std::fprintf(stderr, "%s", buf); \
    } while (0)

#define LOG_INFO(fmt, ...) \
    do { \
        char buf[512]; \
        std::snprintf(buf, sizeof(buf), "[yamanoha][INFO] " fmt "\n", ##__VA_ARGS__); \
        OutputDebugStringA(buf); \
        std::fprintf(stdout, "%s", buf); \
    } while (0)

namespace yamanoha {

// ---------------------------------------------------------------------------
// 全アダプタの情報を列挙する
// ---------------------------------------------------------------------------
std::vector<AdapterInfo> D3D11DeviceManager::EnumerateAdapters() {
    std::vector<AdapterInfo> result;

    // DXGIファクトリの作成
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LOG_ERROR("CreateDXGIFactory1 に失敗しました: 0x%08lX", hr);
        return result;
    }

    // アダプタを順番に列挙
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        hr = adapter->GetDesc1(&desc);
        if (FAILED(hr)) {
            LOG_ERROR("アダプタ %u の GetDesc1 に失敗しました: 0x%08lX", i, hr);
            adapter.Reset();
            continue;
        }

        // ソフトウェアアダプタ (WARP) はスキップ
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter.Reset();
            continue;
        }

        AdapterInfo info{};
        info.name = desc.Description;
        info.dedicated_vram = desc.DedicatedVideoMemory;
        info.shared_vram = desc.SharedSystemMemory;
        info.luid = desc.AdapterLuid;
        info.vendor_id = desc.VendorId;
        info.index = static_cast<int>(i); // EnumAdapters1 の raw インデックス

        LOG_INFO("アダプタ[%d]: %ls (VRAM: %llu MB, ベンダー: 0x%04X)",
                 info.index, info.name.c_str(),
                 info.dedicated_vram / (1024 * 1024),
                 info.vendor_id);

        result.push_back(std::move(info));
        adapter.Reset();
    }

    if (result.empty()) {
        LOG_ERROR("ハードウェアアダプタが見つかりませんでした");
    }

    return result;
}

// ---------------------------------------------------------------------------
// 指定インデックスのアダプタでデバイスを初期化する
// ---------------------------------------------------------------------------
bool D3D11DeviceManager::Initialize(int adapterIndex) {
    // DXGIファクトリの作成
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LOG_ERROR("CreateDXGIFactory1 に失敗しました: 0x%08lX", hr);
        return false;
    }

    // DisplayManager と同じ raw アダプタインデックスを使用してアダプタを取得
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        if (i == static_cast<UINT>(adapterIndex)) {
            break;
        }
        adapter.Reset();
    }

    if (!adapter) {
        LOG_ERROR("アダプタインデックス %d が見つかりませんでした", adapterIndex);
        return false;
    }

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        LOG_ERROR("アダプタインデックス %d はソフトウェアアダプタです", adapterIndex);
        adapter.Reset();
        return false;
    }

    return InitializeForAdapter(adapter.Get());
}

// ---------------------------------------------------------------------------
// 外部から渡されたアダプタでデバイスを初期化する
// ---------------------------------------------------------------------------
bool D3D11DeviceManager::InitializeForAdapter(IDXGIAdapter1* adapter) {
    if (!adapter) {
        LOG_ERROR("アダプタがnullです");
        return false;
    }

    // アダプタ情報のログ出力
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    LOG_INFO("デバイス作成中: %ls", desc.Description);

    // デバイス作成フラグ
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    // デバッグビルドではD3Dデバッグ層を有効化
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
    LOG_INFO("D3D11デバッグ層を有効にしました");
#endif

    // 要求するフィーチャーレベル（11.0以上）
    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL achievedLevel = D3D_FEATURE_LEVEL_11_0;

    // D3D11デバイスの作成
    HRESULT hr = D3D11CreateDevice(
        adapter,                                    // 使用するアダプタ
        D3D_DRIVER_TYPE_UNKNOWN,                    // アダプタ指定時はUNKNOWN
        nullptr,                                    // ソフトウェアラスタライザ不使用
        createFlags,                                // 作成フラグ
        featureLevels,                              // 要求フィーチャーレベル
        static_cast<UINT>(std::size(featureLevels)),// フィーチャーレベル数
        D3D11_SDK_VERSION,                          // SDKバージョン
        &device_,                                   // [out] デバイス
        &achievedLevel,                             // [out] 取得したフィーチャーレベル
        &context_                                   // [out] デバイスコンテキスト
    );

    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDevice に失敗しました: 0x%08lX", hr);
        return false;
    }

    // アダプタの参照を保持
    adapter_.Reset();
    adapter->QueryInterface(IID_PPV_ARGS(&adapter_));

    LOG_INFO("D3D11デバイス作成成功 (フィーチャーレベル: 0x%X)", static_cast<int>(achievedLevel));
    return true;
}

} // namespace yamanoha
