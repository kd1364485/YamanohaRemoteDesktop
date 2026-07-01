// ============================================================================
// cursor_cpu_compositor.cpp
// マウスカーソルを GDI から取得し、BGRA フレームバッファにアルファブレンドする
// ============================================================================

#include "capture/cursor_cpu_compositor.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace yamanoha {

CursorCpuCompositor::~CursorCpuCompositor() = default;

bool CursorCpuCompositor::Composite(
    uint8_t* pixels,
    uint32_t rowPitch,
    uint32_t width,
    uint32_t height,
    const RECT& displayBounds) {
    if (!pixels || width == 0 || height == 0) {
        return false;
    }

    CURSORINFO cursorInfo{};
    cursorInfo.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&cursorInfo)) {
        return false;
    }

    // カーソルが非表示なら何も描画しない
    if ((cursorInfo.flags & CURSOR_SHOWING) == 0) {
        return true;
    }

    // カーソル形状が変わったら再取得
    if (cursorInfo.hCursor != last_cursor_) {
        if (!CaptureCursorBitmap(
                cursorInfo.hCursor,
                cursor_width_,
                cursor_height_,
                cursor_hotspot_x_,
                cursor_hotspot_y_)) {
            return false;
        }
        last_cursor_ = cursorInfo.hCursor;
    }

    if (cursor_pixels_.empty() || cursor_width_ == 0 || cursor_height_ == 0) {
        return true;
    }

    // キャプチャ対象ディスプレイ内のローカル座標を計算（ホットスポットを考慮）
    int localX = cursorInfo.ptScreenPos.x - displayBounds.left - cursor_hotspot_x_;
    int localY = cursorInfo.ptScreenPos.y - displayBounds.top - cursor_hotspot_y_;

    int srcX = 0;
    int srcY = 0;
    if (localX < 0) {
        srcX = -localX;
        localX = 0;
    }
    if (localY < 0) {
        srcY = -localY;
        localY = 0;
    }

    int drawWidth = static_cast<int>(cursor_width_) - srcX;
    int drawHeight = static_cast<int>(cursor_height_) - srcY;
    if (drawWidth <= 0 || drawHeight <= 0) {
        return true;
    }

    if (localX + drawWidth > static_cast<int>(width)) {
        drawWidth = static_cast<int>(width) - localX;
    }
    if (localY + drawHeight > static_cast<int>(height)) {
        drawHeight = static_cast<int>(height) - localY;
    }
    if (drawWidth <= 0 || drawHeight <= 0) {
        return true;
    }

    for (int y = 0; y < drawHeight; ++y) {
        const uint32_t* srcRow =
            cursor_pixels_.data() + (srcY + y) * cursor_width_ + srcX;
        uint8_t* dstRow = pixels + (localY + y) * rowPitch + localX * 4;
        for (int x = 0; x < drawWidth; ++x) {
            uint32_t cursorPixel = srcRow[x];
            uint8_t alpha = static_cast<uint8_t>((cursorPixel >> 24) & 0xFF);
            if (alpha == 0) {
                continue;
            }

            uint8_t b = static_cast<uint8_t>(cursorPixel & 0xFF);
            uint8_t g = static_cast<uint8_t>((cursorPixel >> 8) & 0xFF);
            uint8_t r = static_cast<uint8_t>((cursorPixel >> 16) & 0xFF);

            uint8_t* dst = dstRow + x * 4;
            if (alpha == 255) {
                dst[0] = b;
                dst[1] = g;
                dst[2] = r;
            } else {
                uint16_t inv = 255 - alpha;
                dst[0] = static_cast<uint8_t>((b * alpha + dst[0] * inv) / 255);
                dst[1] = static_cast<uint8_t>((g * alpha + dst[1] * inv) / 255);
                dst[2] = static_cast<uint8_t>((r * alpha + dst[2] * inv) / 255);
            }
        }
    }

    return true;
}

void CursorCpuCompositor::InvalidateCursor() {
    last_cursor_ = nullptr;
    cursor_width_ = 0;
    cursor_height_ = 0;
    cursor_hotspot_x_ = 0;
    cursor_hotspot_y_ = 0;
    cursor_pixels_.clear();
}

bool CursorCpuCompositor::CaptureCursorBitmap(
    HCURSOR hCursor,
    uint32_t& outWidth,
    uint32_t& outHeight,
    int& outHotspotX,
    int& outHotspotY) {
    ICONINFO iconInfo{};
    if (!GetIconInfo(hCursor, &iconInfo)) {
        return false;
    }

    outHotspotX = static_cast<int>(iconInfo.xHotspot);
    outHotspotY = static_cast<int>(iconInfo.yHotspot);

    BITMAP bm{};
    if (iconInfo.hbmColor) {
        GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bm);
        outWidth = static_cast<uint32_t>(bm.bmWidth);
        outHeight = static_cast<uint32_t>(bm.bmHeight);
    } else if (iconInfo.hbmMask) {
        // モノクロームカーソル: マスクは高さが 2 倍（XOR + AND）
        GetObject(iconInfo.hbmMask, sizeof(BITMAP), &bm);
        outWidth = static_cast<uint32_t>(bm.bmWidth);
        outHeight = static_cast<uint32_t>(bm.bmHeight / 2);
    } else {
        DeleteObject(iconInfo.hbmMask);
        DeleteObject(iconInfo.hbmColor);
        return false;
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(nullptr, hdcScreen);
        DeleteObject(iconInfo.hbmMask);
        DeleteObject(iconInfo.hbmColor);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(outWidth);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(outHeight); // 上から下へ
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbmDIB = CreateDIBSection(
        hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmDIB || !bits) {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        DeleteObject(iconInfo.hbmMask);
        DeleteObject(iconInfo.hbmColor);
        return false;
    }

    HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbmDIB));

    DrawIconEx(hdcMem, 0, 0, hCursor, outWidth, outHeight, 0, nullptr, DI_IMAGE);

    const size_t pixelCount = static_cast<size_t>(outWidth) * outHeight;
    cursor_pixels_.resize(pixelCount);
    std::memcpy(cursor_pixels_.data(), bits, pixelCount * 4);

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmDIB);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    DeleteObject(iconInfo.hbmMask);
    DeleteObject(iconInfo.hbmColor);

    return true;
}

} // namespace yamanoha
