// dd_probe.cpp
// Desktop Duplication(画面の複製)が、この PC で使えるかを調べる診断用プログラム。
// 全アダプタ x 全出力の組み合わせで DuplicateOutput / DuplicateOutput1 を試し、結果の HRESULT と、
// 出力の色空間(HDR かどうか)を表示する。FramePipeline が「ダミー画像のモード」になったときの原因調べに使う。
// 使い方: dd_probe.exe(引数なし)
#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
using Microsoft::WRL::ComPtr;

static const char* HrName(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case E_ACCESSDENIED: return "E_ACCESSDENIED";
        case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        case DXGI_ERROR_SESSION_DISCONNECTED: return "DXGI_ERROR_SESSION_DISCONNECTED";
        case E_INVALIDARG: return "E_INVALIDARG";
        case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
        default: return "(other)";
    }
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    struct Dev { ComPtr<ID3D11Device> dev; wchar_t name[128]; };
    std::vector<Dev> devs;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        std::printf("アダプタ[%u] %ls  (LUID %08lX-%08lX, flags=0x%X)\n", i, d.Description,
                    d.AdapterLuid.HighPart, d.AdapterLuid.LowPart, d.Flags);
        ComPtr<ID3D11Device> dev;
        HRESULT hr = D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        std::printf("   D3D11 デバイス作成: 0x%08lX\n", hr);
        Dev x; x.dev = dev; wcsncpy_s(x.name, d.Description, _TRUNCATE);
        devs.push_back(x);

        for (UINT j = 0;; ++j) {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(j, &o) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            o->GetDesc(&od);
            std::printf("   出力[%u] %ls  %ldx%ld  attached=%d  rotation=%d\n", j, od.DeviceName,
                        od.DesktopCoordinates.right - od.DesktopCoordinates.left,
                        od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
                        od.AttachedToDesktop, od.Rotation);
            ComPtr<IDXGIOutput6> o6;
            if (SUCCEEDED(o.As(&o6))) {
                DXGI_OUTPUT_DESC1 od1{};
                if (SUCCEEDED(o6->GetDesc1(&od1))) {
                    std::printf("      ColorSpace=%d  BitsPerColor=%u  (HDR なら ColorSpace=12 など)\n",
                                static_cast<int>(od1.ColorSpace), od1.BitsPerColor);
                }
            }
        }
    }

    std::printf("\n--- DuplicateOutput の総当たり(デバイスのアダプタ × 出力のアダプタ) ---\n");
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT j = 0;; ++j) {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(j, &o) == DXGI_ERROR_NOT_FOUND) break;
            ComPtr<IDXGIOutput1> o1;
            o.As(&o1);
            for (size_t k = 0; k < devs.size(); ++k) {
                if (!devs[k].dev) continue;
                ComPtr<IDXGIOutputDuplication> dup;
                HRESULT hr = o1->DuplicateOutput(devs[k].dev.Get(), &dup);
                std::printf("出力(adapter %u, output %u) × デバイス[%zu] %ls\n   DuplicateOutput  → 0x%08lX %s\n",
                            i, j, k, devs[k].name, hr, HrName(hr));
                ComPtr<IDXGIOutput5> o5;
                if (SUCCEEDED(o.As(&o5))) {
                    DXGI_FORMAT fmts[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          DXGI_FORMAT_R10G10B10A2_UNORM};
                    ComPtr<IDXGIOutputDuplication> dup1;
                    HRESULT hr1 = o5->DuplicateOutput1(devs[k].dev.Get(), 0, 3, fmts, &dup1);
                    std::printf("   DuplicateOutput1 → 0x%08lX %s\n", hr1, HrName(hr1));
                }
            }
        }
    }
    return 0;
}
