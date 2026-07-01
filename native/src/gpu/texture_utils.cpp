// ============================================================================
// texture_utils.cpp — テクスチャユーティリティの実装
// ============================================================================

#include "gpu/texture_utils.h"

#include <cstdio>
#include <cstring>

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

namespace yamanoha::TextureUtils {

// ---------------------------------------------------------------------------
// ステージングテクスチャを作成する
// D3D11_USAGE_STAGING: GPUからCPUへのデータ読み出し専用
// ---------------------------------------------------------------------------
Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateStagingTexture(
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format)
{
    if (!device) {
        LOG_ERROR("デバイスがnullです");
        return nullptr;
    }

    if (width == 0 || height == 0) {
        LOG_ERROR("無効なテクスチャサイズ: %ux%u", width, height);
        return nullptr;
    }

    // ステージングテクスチャのディスクリプション
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;                         // ミップマップなし
    desc.ArraySize = 1;                         // テクスチャ配列なし
    desc.Format = format;
    desc.SampleDesc.Count = 1;                  // マルチサンプリングなし
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;           // ステージング用途
    desc.BindFlags = 0;                         // パイプラインにバインドしない
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;// CPU読み取りアクセス
    desc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        LOG_ERROR("ステージングテクスチャの作成に失敗しました: 0x%08lX (%ux%u, format=%d)",
                  hr, width, height, static_cast<int>(format));
        return nullptr;
    }

    LOG_INFO("ステージングテクスチャ作成成功: %ux%u", width, height);
    return texture;
}

// ---------------------------------------------------------------------------
// ソーステクスチャの内容をステージングテクスチャにコピーする
// ---------------------------------------------------------------------------
bool CopyTextureToStaging(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* src,
    ID3D11Texture2D* staging)
{
    if (!context || !src || !staging) {
        LOG_ERROR("CopyTextureToStaging: 引数がnullです");
        return false;
    }

    // GPU間コピー（ソース→ステージング）
    // CopyResource は同じサイズ・フォーマットのテクスチャ間でのみ有効
    context->CopyResource(staging, src);
    return true;
}

// ---------------------------------------------------------------------------
// ステージングテクスチャをCPUからアクセス可能にマップする
// ---------------------------------------------------------------------------
bool MapStagingTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* staging,
    D3D11_MAPPED_SUBRESOURCE& mapped)
{
    if (!context || !staging) {
        LOG_ERROR("MapStagingTexture: 引数がnullです");
        return false;
    }

    // サブリソース0を読み取り専用でマップ
    HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LOG_ERROR("テクスチャのマップに失敗しました: 0x%08lX", hr);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// マップ済みステージングテクスチャのマップを解除する
// ---------------------------------------------------------------------------
void UnmapStagingTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* staging)
{
    if (!context || !staging) {
        LOG_ERROR("UnmapStagingTexture: 引数がnullです");
        return;
    }

    context->Unmap(staging, 0);
}

// ---------------------------------------------------------------------------
// BGRAピクセルデータをBMPファイルとして保存する
// BMPヘッダーを手書きして外部ライブラリに依存しない
// ---------------------------------------------------------------------------
bool SaveToBMP(
    const char* filename,
    const uint8_t* data,
    uint32_t width,
    uint32_t height,
    uint32_t rowPitch)
{
    if (!filename || !data) {
        LOG_ERROR("SaveToBMP: 引数がnullです");
        return false;
    }

    if (width == 0 || height == 0) {
        LOG_ERROR("SaveToBMP: 無効な画像サイズ: %ux%u", width, height);
        return false;
    }

    // BMPの1行は4バイト境界にアラインされる必要がある
    // BGRA(4バイト/ピクセル)なので width*4 は常に4の倍数
    const uint32_t bmpRowSize = width * 4;
    const uint32_t imageSize = bmpRowSize * height;

    // BMPファイルヘッダー (14バイト)
#pragma pack(push, 1)
    struct BMPFileHeader {
        uint16_t type;          // "BM" = 0x4D42
        uint32_t size;          // ファイル全体のサイズ
        uint16_t reserved1;
        uint16_t reserved2;
        uint32_t offset;        // ピクセルデータのオフセット
    };

    // BMP情報ヘッダー (40バイト)
    struct BMPInfoHeader {
        uint32_t size;          // この構造体のサイズ (40)
        int32_t width;          // 幅
        int32_t height;         // 高さ（正:ボトムアップ、負:トップダウン）
        uint16_t planes;        // プレーン数 (常に1)
        uint16_t bitCount;      // ビット深度 (32 = BGRA)
        uint32_t compression;   // 圧縮形式 (0 = BI_RGB)
        uint32_t imageSize;     // ピクセルデータのサイズ
        int32_t xPPM;           // 水平解像度 (ピクセル/メートル)
        int32_t yPPM;           // 垂直解像度 (ピクセル/メートル)
        uint32_t clrUsed;       // カラーテーブルの色数
        uint32_t clrImportant;  // 重要な色の数
    };
#pragma pack(pop)

    constexpr uint32_t headerSize = sizeof(BMPFileHeader) + sizeof(BMPInfoHeader);

    BMPFileHeader fileHeader{};
    fileHeader.type = 0x4D42;                   // "BM"
    fileHeader.size = headerSize + imageSize;
    fileHeader.reserved1 = 0;
    fileHeader.reserved2 = 0;
    fileHeader.offset = headerSize;

    BMPInfoHeader infoHeader{};
    infoHeader.size = sizeof(BMPInfoHeader);
    infoHeader.width = static_cast<int32_t>(width);
    // 負の高さでトップダウン形式（画面座標系と一致）
    infoHeader.height = -static_cast<int32_t>(height);
    infoHeader.planes = 1;
    infoHeader.bitCount = 32;                   // BGRA
    infoHeader.compression = 0;                 // BI_RGB（無圧縮）
    infoHeader.imageSize = imageSize;
    infoHeader.xPPM = 0;
    infoHeader.yPPM = 0;
    infoHeader.clrUsed = 0;
    infoHeader.clrImportant = 0;

    // ファイルを開いて書き込む
    FILE* file = nullptr;
    errno_t err = fopen_s(&file, filename, "wb");
    if (err != 0 || !file) {
        LOG_ERROR("BMPファイルを開けませんでした: %s", filename);
        return false;
    }

    // ヘッダーの書き込み
    std::fwrite(&fileHeader, sizeof(fileHeader), 1, file);
    std::fwrite(&infoHeader, sizeof(infoHeader), 1, file);

    // ピクセルデータの書き込み
    // rowPitch（GPUテクスチャの行ピッチ）とBMPの行サイズが異なる場合があるため
    // 1行ずつコピーする
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * rowPitch;
        std::fwrite(row, bmpRowSize, 1, file);
    }

    std::fclose(file);
    LOG_INFO("BMP保存完了: %s (%ux%u)", filename, width, height);
    return true;
}

} // namespace yamanoha::TextureUtils
