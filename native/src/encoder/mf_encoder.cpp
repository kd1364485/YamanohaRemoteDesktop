// ============================================================================
// mf_encoder.cpp
// Media Foundation H.264 エンコーダの実装
// ============================================================================
#include "mf_encoder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

// Media Foundation ヘッダ
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <codecapi.h>
#include <mferror.h>
#include <mfobjects.h>

// MF_SA_D3D11_DEVICE may not be available in all SDK versions
#ifndef MF_SA_D3D11_DEVICE
// {5A676B8D-9FB9-4256-8C5E-0B4A50F9D3A7}
static const GUID MF_SA_D3D11_DEVICE_GUID =
{ 0x5a676b8d, 0x9fb9, 0x4256, { 0x8c, 0x5e, 0x0b, 0x4a, 0x50, 0xf9, 0xd3, 0xa7 } };
#define MF_SA_D3D11_DEVICE MF_SA_D3D11_DEVICE_GUID
#endif

// CODECAPI_AVEncVideoMaxKeyFrameDistance may not be available in all SDK versions

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "d3d11")

namespace yamanoha {

namespace {

uint64_t NowMicroseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

MfEncoder::MfEncoder() = default;

MfEncoder::~MfEncoder() {
    Shutdown();
}

bool MfEncoder::Initialize(ID3D11Device* device, const EncoderConfig& config) {
    if (!device) {
        std::printf("[MfEncoder] D3D11 device is null\n");
        return false;
    }

    Shutdown();

    device_ = device;
    device->GetImmediateContext(&context_);
    config_ = config;
    width_ = config.width;
    height_ = config.height;

    // Media Foundation 起動
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] MFStartup failed: 0x%08lX\n", hr);
        return false;
    }

    if (!CreateEncoder()) return false;
    if (!ConfigureInputType()) return false;
    if (!ConfigureOutputType()) return false;
    if (!AllocateInputSample()) return false;

    initialized_ = true;
    std::printf("[MfEncoder] initialized: %u x %u (fps: %u, bitrate: %u kbps)\n",
                width_, height_, config.fps, config.bitrate_kbps);
    return true;
}

bool MfEncoder::CreateEncoder() {
    // D3D11 Device Manager を作成してエンコーダにハードウェアアクセラレーションを提供
    HRESULT hr = MFCreateDXGIDeviceManager(&reset_token_, &mf_device_manager_);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] MFCreateDXGIDeviceManager failed: 0x%08lX\n", hr);
        return false;
    }

    hr = mf_device_manager_->ResetDevice(device_.Get(), reset_token_);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] ResetDevice failed: 0x%08lX\n", hr);
        return false;
    }

    // H.264 エンコーダ MFT を列挙して作成
    MFT_REGISTER_TYPE_INFO output_type_info;
    output_type_info.guidMajorType = MFMediaType_Video;
    output_type_info.guidSubtype = MFVideoFormat_H264;

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT,
        nullptr,
        &output_type_info,
        &ppActivate,
        &count);

    ComPtr<IMFActivate> activate;
    if (SUCCEEDED(hr) && count > 0) {
        activate = ppActivate[0];
        for (UINT32 i = 0; i < count; ++i) {
            if (i != 0) ppActivate[i]->Release();
        }
        CoTaskMemFree(ppActivate);
        std::printf("[MfEncoder] Found hardware H.264 encoder\n");
    } else {
        // ハードウェアエンコーダが見つからない場合はソフトウェアエンコーダを試す
        if (ppActivate) {
            for (UINT32 i = 0; i < count; ++i) ppActivate[i]->Release();
            CoTaskMemFree(ppActivate);
        }

        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr,
            &output_type_info,
            &ppActivate,
            &count);

        if (SUCCEEDED(hr) && count > 0) {
            activate = ppActivate[0];
            for (UINT32 i = 0; i < count; ++i) {
                if (i != 0) ppActivate[i]->Release();
            }
            CoTaskMemFree(ppActivate);
            std::printf("[MfEncoder] Using software H.264 encoder\n");
        } else {
            if (ppActivate) {
                for (UINT32 i = 0; i < count; ++i) ppActivate[i]->Release();
                CoTaskMemFree(ppActivate);
            }
            std::printf("[MfEncoder] No H.264 encoder found\n");
            return false;
        }
    }

    // MFT をアクティベート
    hr = activate->ActivateObject(
        IID_PPV_ARGS(&encoder_transform_));
    if (FAILED(hr)) {
        std::printf("[MfEncoder] ActivateObject failed: 0x%08lX\n", hr);
        return false;
    }

    // ハードウェアアクセラレーション用に D3D11 Device Manager を設定
    ComPtr<IMFAttributes> attributes;
    hr = encoder_transform_->GetAttributes(&attributes);
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC, TRUE);
        if (SUCCEEDED(hr)) {
            hr = attributes->SetUnknown(
                MF_SA_D3D11_DEVICE,
                static_cast<IUnknown*>(mf_device_manager_.Get()));
        }
        if (FAILED(hr)) {
            std::printf("[MfEncoder] Warning: could not set D3D11 device manager: 0x%08lX\n", hr);
        }
    }

    // イベントジェネレータを取得（非同期MFTの場合）
    encoder_transform_.As(&event_generator_);

    return true;
}

bool MfEncoder::ConfigureInputType() {
    HRESULT hr = MFCreateMediaType(&input_type_);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] MFCreateMediaType (input) failed: 0x%08lX\n", hr);
        return false;
    }

    hr = input_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return false;
    hr = input_type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeSize(input_type_.Get(), MF_MT_FRAME_SIZE, width_, height_);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeRatio(input_type_.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
    if (FAILED(hr)) return false;
    hr = input_type_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return false;

    hr = encoder_transform_->SetInputType(0, input_type_.Get(), 0);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] SetInputType failed: 0x%08lX\n", hr);
        return false;
    }

    return true;
}

bool MfEncoder::ConfigureOutputType() {
    HRESULT hr = MFCreateMediaType(&output_type_);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] MFCreateMediaType (output) failed: 0x%08lX\n", hr);
        return false;
    }

    hr = output_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return false;
    hr = output_type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeSize(output_type_.Get(), MF_MT_FRAME_SIZE, width_, height_);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeRatio(output_type_.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
    if (FAILED(hr)) return false;
    hr = output_type_->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate_kbps * 1000);
    if (FAILED(hr)) return false;

    hr = encoder_transform_->SetOutputType(0, output_type_.Get(), 0);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] SetOutputType failed: 0x%08lX\n", hr);
        return false;
    }

    // 低遅延設定
    ComPtr<IMFAttributes> attributes;
    hr = encoder_transform_->GetAttributes(&attributes);
    if (SUCCEEDED(hr)) {
        attributes->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        attributes->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, config_.bitrate_kbps * 1000);
        attributes->SetUINT32(CODECAPI_AVEncVideoForceKeyFrame, 1);
        attributes->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        attributes->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 100);
        attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }

    // 処理開始
    hr = encoder_transform_->ProcessMessage(
        MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] BEGIN_STREAMING failed: 0x%08lX\n", hr);
        return false;
    }

    return true;
}

bool MfEncoder::AllocateInputSample() {
    // NV12フォーマットの入力サンプルを作成
    // Y平面: width * height
    // UV平面: width * (height/2)
    const uint32_t y_size = width_ * height_;
    const uint32_t uv_size = width_ * (height_ / 2);
    const uint32_t total_size = y_size + uv_size;

    HRESULT hr = MFCreateSample(&input_sample_);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] MFCreateSample failed: 0x%08lX\n", hr);
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreate2DMediaBuffer(width_, height_, MFVideoFormat_NV12.Data1, FALSE, &buffer);
    if (FAILED(hr)) {
        // フォールバック: 通常のバッファ
        hr = MFCreateMemoryBuffer(total_size, &buffer);
        if (FAILED(hr)) {
            std::printf("[MfEncoder] MFCreateMemoryBuffer failed: 0x%08lX\n", hr);
            return false;
        }
    }

    hr = input_sample_->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
        std::printf("[MfEncoder] AddBuffer failed: 0x%08lX\n", hr);
        return false;
    }

    return true;
}

int MfEncoder::SubmitFrame(ID3D11Texture2D* texture, bool force_idr) {
    if (!initialized_ || !encoder_transform_ || !texture) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(api_mutex_);

    // D3D11テクスチャからNV12への変換
    // staging textureを使ってCPU経由でデータを取得
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC src_desc{};
    texture->GetDesc(&src_desc);

    D3D11_TEXTURE2D_DESC staging_desc = src_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    HRESULT hr = device_->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] CreateTexture2D (staging) failed: 0x%08lX\n", hr);
        return -1;
    }

    context_->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::printf("[MfEncoder] Map failed: 0x%08lX\n", hr);
        return -1;
    }

    // BGRA → NV12 変換
    ComPtr<IMFMediaBuffer> buffer;
    hr = input_sample_->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) {
        context_->Unmap(staging.Get(), 0);
        return -1;
    }

    ComPtr<IMF2DBuffer2> buffer2d;
    hr = buffer.As(&buffer2d);
    if (SUCCEEDED(hr)) {
        BYTE* src_data = static_cast<BYTE*>(mapped.pData);
        const uint32_t src_pitch = mapped.RowPitch;

        BYTE* dest_y = nullptr;
        BYTE* dest_uv = nullptr;
        LONG dest_y_pitch = 0;
        DWORD dest_uv_pitch_dword = 0;
        hr = buffer2d->Lock2DSize(
            MF2DBuffer_LockFlags_Write,
            &dest_y, &dest_y_pitch,
            &dest_uv, &dest_uv_pitch_dword);
        const LONG dest_uv_pitch = static_cast<LONG>(dest_uv_pitch_dword);
        if (SUCCEEDED(hr)) {
            // Y平面
            for (uint32_t y = 0; y < height_; ++y) {
                const uint8_t* src = src_data + y * src_pitch;
                uint8_t* dst = dest_y + y * dest_y_pitch;
                for (uint32_t x = 0; x < width_; ++x) {
                    const uint8_t b = src[x * 4 + 0];
                    const uint8_t g = src[x * 4 + 1];
                    const uint8_t r = src[x * 4 + 2];
                    // BT.601 Y
                    dst[x] = static_cast<uint8_t>(
                        (66 * r + 129 * g + 25 * b + 128) >> 8);
                }
            }
            // UV平面 (NV12: interleaved)
            for (uint32_t y = 0; y < height_ / 2; ++y) {
                const uint8_t* src0 = src_data + (y * 2) * src_pitch;
                const uint8_t* src1 = src_data + (y * 2 + 1) * src_pitch;
                uint8_t* dst = dest_uv + y * dest_uv_pitch;
                for (uint32_t x = 0; x < width_ / 2; ++x) {
                    const uint8_t b0 = src0[x * 2 * 4 + 0];
                    const uint8_t g0 = src0[x * 2 * 4 + 1];
                    const uint8_t r0 = src0[x * 2 * 4 + 2];
                    const uint8_t b1 = src1[x * 2 * 4 + 0];
                    const uint8_t g1 = src1[x * 2 * 4 + 1];
                    const uint8_t r1 = src1[x * 2 * 4 + 2];
                    const int b = (b0 + b1) / 2;
                    const int g = (g0 + g1) / 2;
                    const int r = (r0 + r1) / 2;
                    // BT.601 U/V
                    dst[x * 2 + 0] = static_cast<uint8_t>(
                        std::clamp((-38 * r - 74 * g + 112 * b + 128) >> 8, 0, 255) + 128);
                    dst[x * 2 + 1] = static_cast<uint8_t>(
                        std::clamp((112 * r - 94 * g - 18 * b + 128) >> 8, 0, 255) + 128);
                }
            }
            buffer2d->Unlock2D();
        }
    } else {
        // フォールバック: 1Dバッファ
        ComPtr<IMF2DBuffer> buffer2d_v1;
        hr = buffer.As(&buffer2d_v1);
        if (SUCCEEDED(hr)) {
            BYTE* dest = nullptr;
            LONG dest_pitch = 0;
            hr = buffer2d_v1->Lock2D(&dest, &dest_pitch);
            if (SUCCEEDED(hr)) {
                const uint8_t* src_data = static_cast<uint8_t*>(mapped.pData);
                const uint32_t src_pitch = mapped.RowPitch;
                for (uint32_t y = 0; y < height_; ++y) {
                    const uint8_t* src = src_data + y * src_pitch;
                    uint8_t* dst = dest + y * dest_pitch;
                    for (uint32_t x = 0; x < width_; ++x) {
                        const uint8_t b = src[x * 4 + 0];
                        const uint8_t g = src[x * 4 + 1];
                        const uint8_t r = src[x * 4 + 2];
                        dst[x] = static_cast<uint8_t>(
                            (66 * r + 129 * g + 25 * b + 128) >> 8);
                    }
                }
                for (uint32_t y = 0; y < height_ / 2; ++y) {
                    const uint8_t* src0 = src_data + (y * 2) * src_pitch;
                    const uint8_t* src1 = src_data + (y * 2 + 1) * src_pitch;
                    uint8_t* dst = dest + (height_ + y) * dest_pitch;
                    for (uint32_t x = 0; x < width_ / 2; ++x) {
                        const uint8_t b0 = src0[x * 2 * 4 + 0];
                        const uint8_t g0 = src0[x * 2 * 4 + 1];
                        const uint8_t r0 = src0[x * 2 * 4 + 2];
                        const uint8_t b1 = src1[x * 2 * 4 + 0];
                        const uint8_t g1 = src1[x * 2 * 4 + 1];
                        const uint8_t r1 = src1[x * 2 * 4 + 2];
                        const int b = (b0 + b1) / 2;
                        const int g = (g0 + g1) / 2;
                        const int r = (r0 + r1) / 2;
                        dst[x * 2 + 0] = static_cast<uint8_t>(
                            std::clamp((-38 * r - 74 * g + 112 * b + 128) >> 8, 0, 255) + 128);
                        dst[x * 2 + 1] = static_cast<uint8_t>(
                            std::clamp((112 * r - 94 * g - 18 * b + 128) >> 8, 0, 255) + 128);
                    }
                }
                buffer2d_v1->Unlock2D();
            }
        }
    }

    context_->Unmap(staging.Get(), 0);

    // タイムスタンプ設定
    const int64_t timestamp_100ns = static_cast<int64_t>(
        NowMicroseconds() * 10);
    input_sample_->SetSampleTime(timestamp_100ns);
    input_sample_->SetSampleDuration(static_cast<int64_t>(
        10000000 / config_.fps));

    // キーフレーム強制
    if (force_idr) {
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(encoder_transform_->GetAttributes(&attrs))) {
            attrs->SetUINT32(CODECAPI_AVEncVideoForceKeyFrame, 1);
        }
    }

    // エンコード
    hr = encoder_transform_->ProcessInput(0, input_sample_.Get(), 0);
    if (FAILED(hr)) {
        // ME_E_NOTACCEPTING — 前のフレームがまだ取り出されていない
        return -1;
    }

    int frame_id = next_frame_id_++;
    {
        std::lock_guard<std::mutex> jlock(jobs_mutex_);
        pending_jobs_.push_back({frame_id, texture});
    }

    return frame_id;
}

bool MfEncoder::GetCompletedFrame(
    std::vector<uint8_t>& out_bitstream,
    ID3D11Texture2D** out_texture,
    int* out_frame_id,
    uint64_t timeout_us) {
    if (!initialized_ || !encoder_transform_) return false;

    std::lock_guard<std::mutex> lock(api_mutex_);

    // 出力を取得
    MFT_OUTPUT_DATA_BUFFER output_buffer{};
    output_buffer.pSample = nullptr;
    DWORD status = 0;

    HRESULT hr = encoder_transform_->ProcessOutput(
        0, 1, &output_buffer, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        // まだ出力準備ができていない
        return false;
    }

    if (FAILED(hr)) {
        std::printf("[MfEncoder] ProcessOutput failed: 0x%08lX\n", hr);
        return false;
    }

    // 出力サンプルからビットストリームを取得
    ComPtr<IMFSample> output_sample(output_buffer.pSample);
    if (output_buffer.pEvents) {
        output_buffer.pEvents->Release();
    }

    if (!output_sample) {
        return false;
    }

    DWORD total_length = 0;
    hr = output_sample->GetTotalLength(&total_length);
    if (FAILED(hr) || total_length == 0) {
        return false;
    }

    out_bitstream.resize(total_length);

    ComPtr<IMFMediaBuffer> media_buffer;
    hr = output_sample->ConvertToContiguousBuffer(&media_buffer);
    if (FAILED(hr)) {
        // 複数バッファの場合
        DWORD buffer_count = 0;
        output_sample->GetBufferCount(&buffer_count);
        size_t offset = 0;
        for (DWORD i = 0; i < buffer_count; ++i) {
            ComPtr<IMFMediaBuffer> buf;
            hr = output_sample->GetBufferByIndex(i, &buf);
            if (FAILED(hr)) continue;
            DWORD max_len = 0, cur_len = 0;
            buf->GetMaxLength(&max_len);
            buf->GetCurrentLength(&cur_len);
            if (offset + cur_len > out_bitstream.size()) {
                out_bitstream.resize(offset + cur_len);
            }
            BYTE* data = nullptr;
            buf->Lock(&data, &max_len, &cur_len);
            memcpy(out_bitstream.data() + offset, data, cur_len);
            buf->Unlock();
            offset += cur_len;
        }
        out_bitstream.resize(offset);
    } else {
        BYTE* data = nullptr;
        DWORD max_len = 0, cur_len = 0;
        hr = media_buffer->Lock(&data, &max_len, &cur_len);
        if (SUCCEEDED(hr)) {
            out_bitstream.assign(data, data + cur_len);
            media_buffer->Unlock();
        }
    }

    // ペンジョブから対応するテクスチャを取得
    PendingJob job{};
    {
        std::lock_guard<std::mutex> jlock(jobs_mutex_);
        if (pending_jobs_.empty()) {
            return false;
        }
        job = pending_jobs_.front();
        pending_jobs_.pop_front();
    }

    *out_texture = job.texture;
    *out_frame_id = job.frame_id;

    return true;
}

bool MfEncoder::Flush(std::vector<uint8_t>& out_bitstream) {
    if (!initialized_ || !encoder_transform_) return false;

    out_bitstream.clear();

    // 残りの出力を取り出す
    HRESULT hr = encoder_transform_->ProcessMessage(
        MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<uint8_t> temp;
        ID3D11Texture2D* tex = nullptr;
        int id = 0;
        if (!GetCompletedFrame(temp, &tex, &id, 0)) {
            break;
        }
    }

    return true;
}

bool MfEncoder::ReconfigureBitrate(uint32_t bitrate_kbps) {
    if (!initialized_ || !encoder_transform_ || bitrate_kbps == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(api_mutex_);

    ComPtr<IMFAttributes> attrs;
    HRESULT hr = encoder_transform_->GetAttributes(&attrs);
    if (SUCCEEDED(hr)) {
        attrs->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate_kbps * 1000);
        config_.bitrate_kbps = bitrate_kbps;
        std::printf("[MfEncoder] bitrate reconfigured: %u kbps\n", bitrate_kbps);
        return true;
    }
    return false;
}

void MfEncoder::Shutdown() {
    initialized_ = false;

    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        if (encoder_transform_) {
            encoder_transform_->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            encoder_transform_->ProcessMessage(
                MFT_MESSAGE_COMMAND_DRAIN, 0);
            encoder_transform_.Reset();
        }
    }

    input_sample_.Reset();
    output_type_.Reset();
    input_type_.Reset();
    event_generator_.Reset();
    encoder_transform_.Reset();
    mf_device_manager_.Reset();
    context_.Reset();
    device_.Reset();

    {
        std::lock_guard<std::mutex> jlock(jobs_mutex_);
        pending_jobs_.clear();
    }

    width_ = 0;
    height_ = 0;

    MFShutdown();
}

} // namespace yamanoha
