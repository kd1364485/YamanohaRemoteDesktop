// ============================================================================
// cursor_cpu_compositor.h
// ホスト側でマウスカーソルを BGRA フレームバッファに合成する
// ============================================================================

#pragma once

#include <cstdint>
#include <vector>
#include <windows.h>

namespace yamanoha {

class CursorCpuCompositor {
public:
    CursorCpuCompositor() = default;
    ~CursorCpuCompositor();

    // 現在のマウスカーソルを BGRA ピクセルバッファに合成する。
    // pixels: 書き込み可能な BGRA バッファ（上から下へ）
    // rowPitch: 1 行のバイト数
    // width/height: バッファのピクセルサイズ
    // displayBounds: キャプチャ対象ディスプレイのデスクトップ座標
    bool Composite(
        uint8_t* pixels,
        uint32_t rowPitch,
        uint32_t width,
        uint32_t height,
        const RECT& displayBounds);

    // 次回 Composite でカーソル形状を再取得する
    void InvalidateCursor();

private:
    bool CaptureCursorBitmap(
        HCURSOR hCursor,
        uint32_t& outWidth,
        uint32_t& outHeight,
        int& outHotspotX,
        int& outHotspotY);

    HCURSOR last_cursor_ = nullptr;
    uint32_t cursor_width_ = 0;
    uint32_t cursor_height_ = 0;
    int cursor_hotspot_x_ = 0;
    int cursor_hotspot_y_ = 0;
    std::vector<uint32_t> cursor_pixels_;
};

} // namespace yamanoha
