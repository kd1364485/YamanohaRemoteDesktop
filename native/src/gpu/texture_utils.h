#pragma once
// ============================================================================
// texture_utils.h — テクスチャユーティリティ
// ステージングテクスチャの作成・コピー・マップ操作、BMP保存を提供する
// ============================================================================

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace yamanoha::TextureUtils {

// ---------------------------------------------------------------------------
// ステージングテクスチャを作成する
// CPU読み取り用のテクスチャ。GPUからCPUへのデータ転送に使用する
// ---------------------------------------------------------------------------
Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateStagingTexture(
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format);

// ---------------------------------------------------------------------------
// ソーステクスチャの内容をステージングテクスチャにコピーする
// srcとstagingは同じサイズ・フォーマットである必要がある
// ---------------------------------------------------------------------------
bool CopyTextureToStaging(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* src,
    ID3D11Texture2D* staging);

// ---------------------------------------------------------------------------
// ステージングテクスチャをCPUからアクセス可能にマップする
// 使用後は必ず UnmapStagingTexture を呼ぶこと
// ---------------------------------------------------------------------------
bool MapStagingTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* staging,
    D3D11_MAPPED_SUBRESOURCE& mapped);

// ---------------------------------------------------------------------------
// マップ済みステージングテクスチャのマップを解除する
// ---------------------------------------------------------------------------
void UnmapStagingTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* staging);

// ---------------------------------------------------------------------------
// BGRAピクセルデータをBMPファイルとして保存する
// Desktop Duplication API の出力形式(BGRA)を前提としている
// 外部ライブラリ不要でBMPヘッダーを直接書き込む
// ---------------------------------------------------------------------------
bool SaveToBMP(
    const char* filename,
    const uint8_t* data,
    uint32_t width,
    uint32_t height,
    uint32_t rowPitch);

} // namespace yamanoha::TextureUtils
