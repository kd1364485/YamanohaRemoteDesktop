// ============================================================================
// nvenc_encoder.cpp
// NVIDIA NVENC SDK を使用したハードウェア H.264 エンコーダクラスの実装
// ============================================================================
#include "nvenc_encoder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace yamanoha {

namespace {

constexpr bool kUseAsyncEncoding = false;

uint64_t NowMicroseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

GUID SelectPresetGuid(uint32_t preset) {
    static const GUID presets[] = {
        NV_ENC_PRESET_P1_GUID,
        NV_ENC_PRESET_P2_GUID,
        NV_ENC_PRESET_P3_GUID,
        NV_ENC_PRESET_P4_GUID,
        NV_ENC_PRESET_P5_GUID,
        NV_ENC_PRESET_P6_GUID,
        NV_ENC_PRESET_P7_GUID,
    };
    const uint32_t index = (std::clamp)(preset, 1u, 7u) - 1;
    return presets[index];
}

NV_ENC_TUNING_INFO SelectTuningInfo(bool zero_latency) {
    return zero_latency
        ? NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY
        : NV_ENC_TUNING_INFO_LOW_LATENCY;
}

void ApplyRateControl(
    NV_ENC_CONFIG& encode_config,
    const EncoderConfig& config) {
    const uint32_t fps = (std::max)(config.fps, 1u);
    encode_config.rcParams.version = NV_ENC_RC_PARAMS_VER;
    encode_config.rcParams.rateControlMode = config.use_cbr
        ? NV_ENC_PARAMS_RC_CBR
        : NV_ENC_PARAMS_RC_VBR;
    encode_config.rcParams.averageBitRate = config.bitrate_kbps * 1000;
    encode_config.rcParams.maxBitRate =
        (std::max)(config.max_bitrate_kbps, config.bitrate_kbps) * 1000;
    encode_config.rcParams.vbvBufferSize =
        encode_config.rcParams.averageBitRate / fps;
    encode_config.rcParams.vbvInitialDelay =
        encode_config.rcParams.vbvBufferSize;
    encode_config.rcParams.lowDelayKeyFrameScale =
        config.zero_latency ? 1 : 0;
}

} // namespace

#define LOG_NVENC_ERR(msg, status) \
    std::printf("[NvencEncoder] " msg " 失敗: エラーコード=%d\n", static_cast<int>(status))

typedef NVENCSTATUS(NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST*);

// ---------------------------------------------------------------------------
// コンストラクタ
// ---------------------------------------------------------------------------
NvencEncoder::NvencEncoder() {
    std::memset(&nv_api_, 0, sizeof(nv_api_));
}

// ---------------------------------------------------------------------------
// デストラクタ
// ---------------------------------------------------------------------------
NvencEncoder::~NvencEncoder() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// NVENC APIテーブルを DLL から動的ロードする
// ---------------------------------------------------------------------------
bool NvencEncoder::LoadNvencApi() {
    if (nvenc_dll_) return true; // 既にロード済み

    // NVIDIAドライバに同梱されているライブラリをロード
    nvenc_dll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!nvenc_dll_) {
        std::printf("[NvencEncoder] nvEncodeAPI64.dll のロードに失敗しました。"
                    "NVIDIA GPUドライバが正しくインストールされているか確認してください。\n");
        return false;
    }

    // エントリポイント関数のアドレスを取得
    auto nvEncCreateInstance = reinterpret_cast<PNVENCODEAPICREATEINSTANCE>(
        GetProcAddress(nvenc_dll_, "NvEncodeAPICreateInstance"));
    if (!nvEncCreateInstance) {
        std::printf("[NvencEncoder] NvEncodeAPICreateInstance のアドレス取得に失敗しました。\n");
        return false;
    }

    // API 関数ポインタのリストを取得
    nv_api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS status = nvEncCreateInstance(&nv_api_);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("NvEncodeAPICreateInstance 呼び出し", status);
        return false;
    }

    return true;
}

bool NvencEncoder::BuildSessionConfiguration(
    const EncoderConfig& config,
    NV_ENC_CONFIG& encode_config,
    NV_ENC_INITIALIZE_PARAMS& initialize_params) {
    const GUID encode_guid = NV_ENC_CODEC_H264_GUID;
    const GUID preset_guid = SelectPresetGuid(config.preset);
    const NV_ENC_TUNING_INFO tuning_info =
        SelectTuningInfo(config.zero_latency);

    NV_ENC_PRESET_CONFIG preset_config{};
    preset_config.version = NV_ENC_PRESET_CONFIG_VER;
    preset_config.presetCfg.version = NV_ENC_CONFIG_VER;
    const NVENCSTATUS status = nv_api_.nvEncGetEncodePresetConfigEx(
        encoder_handle_,
        encode_guid,
        preset_guid,
        tuning_info,
        &preset_config);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncGetEncodePresetConfigEx", status);
        return false;
    }

    encode_config = preset_config.presetCfg;
    encode_config.gopLength = config.gop_size;
    encode_config.frameIntervalP = 1;
    ApplyRateControl(encode_config, config);

    auto& h264 = encode_config.encodeCodecConfig.h264Config;
    h264.idrPeriod = config.gop_size;
    h264.repeatSPSPPS = 1;
    h264.sliceMode = 0;
    h264.sliceModeData = 0;

    initialize_params = {};
    initialize_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize_params.encodeGUID = encode_guid;
    initialize_params.presetGUID = preset_guid;
    initialize_params.tuningInfo = tuning_info;
    initialize_params.encodeWidth = config.width;
    initialize_params.encodeHeight = config.height;
    initialize_params.darWidth = config.width;
    initialize_params.darHeight = config.height;
    initialize_params.frameRateNum = (std::max)(config.fps, 1u);
    initialize_params.frameRateDen = 1;
    initialize_params.enablePTD = 1;
    initialize_params.enableEncodeAsync = kUseAsyncEncoding ? 1 : 0;
    initialize_params.encodeConfig = &encode_config;
    return true;
}

// ---------------------------------------------------------------------------
// エンコーダの初期化
// ---------------------------------------------------------------------------
bool NvencEncoder::Initialize(ID3D11Device* device, const EncoderConfig& config) {
    if (!device) {
        std::printf("[NvencEncoder] D3D11デバイスが null です\n");
        return false;
    }

    Shutdown();

    device_ = device;
    config_ = config;
    width_ = config.width;
    height_ = config.height;

    // API のロード
    if (!LoadNvencApi()) {
        return false;
    }

    // 1. エンコードセッションのオープン
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params{};
    open_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open_params.device = device_.Get();
    open_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open_params.apiVersion = NVENCAPI_VERSION;

    std::printf("[NvencEncoder] nvEncOpenEncodeSessionEx パラメータデバッグ:\n");
    std::printf("  sizeof(open_params)   : %zu\n", sizeof(open_params));
    std::printf("  open_params.version   : 0x%08X\n", open_params.version);
    std::printf("  open_params.device    : %p\n", open_params.device);
    std::printf("  open_params.deviceType: %d\n", open_params.deviceType);
    std::printf("  open_params.apiVersion: 0x%08X (Expected: 0x%08X)\n", open_params.apiVersion, NVENCAPI_VERSION);
    std::printf("  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER: 0x%08X\n", NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER);

    NVENCSTATUS status = nv_api_.nvEncOpenEncodeSessionEx(&open_params, &encoder_handle_);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncOpenEncodeSessionEx", status);
        return false;
    }

    // セッション設定を一箇所で組み立てる。再設定時も同じ値を基準にする。
    NV_ENC_CONFIG encode_config{};
    NV_ENC_INITIALIZE_PARAMS init_params{};
    if (!BuildSessionConfiguration(config, encode_config, init_params)) {
        return false;
    }

    status = nv_api_.nvEncInitializeEncoder(encoder_handle_, &init_params);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncInitializeEncoder", status);
        return false;
    }

    active_encode_config_ = encode_config;
    active_initialize_params_ = init_params;
    active_initialize_params_.encodeConfig = &active_encode_config_;

    // 6. 非同期エンコード用リソースプールの作成
    if (!CreateAsyncResources()) {
        return false;
    }

    initialized_ = true;
    std::printf("[NvencEncoder] 初期化成功: %u x %u (fps: %u, CBR: %d, Bitrate: %u kbps)\n",
                width_, height_, config.fps, config.use_cbr, config.bitrate_kbps);
    return true;
}

// ---------------------------------------------------------------------------
// 非同期エンコード用リソースプールの作成
// ---------------------------------------------------------------------------
bool NvencEncoder::CreateAsyncResources() {
    // パイプライン化のため、複数の出力バッファと完了イベントを確保
    const size_t pool_size = 4;

    for (size_t i = 0; i < pool_size; ++i) {
        NV_ENC_CREATE_BITSTREAM_BUFFER bitstream_params{};
        bitstream_params.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

        NVENCSTATUS status = nv_api_.nvEncCreateBitstreamBuffer(encoder_handle_, &bitstream_params);
        if (status != NV_ENC_SUCCESS) {
            LOG_NVENC_ERR("nvEncCreateBitstreamBuffer", status);
            ReleaseAsyncResources();
            return false;
        }

        if (!kUseAsyncEncoding) {
            free_output_buffers_.push_back(bitstream_params.bitstreamBuffer);
            continue;
        }

        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            std::printf("[NvencEncoder] 完了イベントの作成に失敗しました\n");
            nv_api_.nvEncDestroyBitstreamBuffer(encoder_handle_, bitstream_params.bitstreamBuffer);
            ReleaseAsyncResources();
            return false;
        }

        NV_ENC_EVENT_PARAMS event_params{};
        event_params.version = NV_ENC_EVENT_PARAMS_VER;
        event_params.completionEvent = event;
        status = nv_api_.nvEncRegisterAsyncEvent(
            encoder_handle_, &event_params);
        if (status != NV_ENC_SUCCESS) {
            LOG_NVENC_ERR("nvEncRegisterAsyncEvent", status);
            CloseHandle(event);
            nv_api_.nvEncDestroyBitstreamBuffer(
                encoder_handle_, bitstream_params.bitstreamBuffer);
            ReleaseAsyncResources();
            return false;
        }

        free_output_buffers_.push_back(bitstream_params.bitstreamBuffer);
        free_events_.push_back(event);
    }

    std::printf("[NvencEncoder] 非同期リソースプール作成: %zu セット\n", pool_size);
    return true;
}

// ---------------------------------------------------------------------------
// 非同期エンコード用リソースプールの解放
// ---------------------------------------------------------------------------
void NvencEncoder::ReleaseAsyncResources() {
    std::deque<AsyncJob> pending_jobs;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        pending_jobs.swap(pending_jobs_);
    }

    for (auto& job : pending_jobs) {
        if (job.mapped_resource && encoder_handle_) {
            nv_api_.nvEncUnmapInputResource(
                encoder_handle_, job.mapped_resource);
        }
        if (job.completion_event && encoder_handle_) {
            NV_ENC_EVENT_PARAMS params{};
            params.version = NV_ENC_EVENT_PARAMS_VER;
            params.completionEvent = job.completion_event;
            nv_api_.nvEncUnregisterAsyncEvent(encoder_handle_, &params);
        }
        if (job.completion_event) {
            CloseHandle(job.completion_event);
        }
        if (job.output_buffer && encoder_handle_) {
            nv_api_.nvEncDestroyBitstreamBuffer(
                encoder_handle_, job.output_buffer);
        }
    }

    for (auto buffer : free_output_buffers_) {
        if (buffer && encoder_handle_) {
            nv_api_.nvEncDestroyBitstreamBuffer(encoder_handle_, buffer);
        }
    }
    free_output_buffers_.clear();

    for (auto event : free_events_) {
        if (event && encoder_handle_) {
            NV_ENC_EVENT_PARAMS params{};
            params.version = NV_ENC_EVENT_PARAMS_VER;
            params.completionEvent = event;
            nv_api_.nvEncUnregisterAsyncEvent(encoder_handle_, &params);
        }
        if (event) {
            CloseHandle(event);
        }
    }
    free_events_.clear();
}

// ---------------------------------------------------------------------------
// D3D11 テクスチャの登録 (キャッシュ対応)
// ---------------------------------------------------------------------------
void* NvencEncoder::RegisterResource(ID3D11Texture2D* texture) {
    // 既に登録済みのテクスチャかキャッシュを探索
    for (const auto& item : registered_textures_) {
        if (item.raw_texture == texture) {
            return item.nvenc_registered_resource;
        }
    }

    // 未登録の場合は新しく登録する
    NV_ENC_REGISTER_RESOURCE reg_params{};
    reg_params.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg_params.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg_params.resourceToRegister = texture;
    reg_params.width = width_;
    reg_params.height = height_;
    reg_params.pitch = 0;
    reg_params.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB; // DXGI_FORMAT_B8G8R8A8_UNORM は ARGB 扱い

    NVENCSTATUS status = nv_api_.nvEncRegisterResource(encoder_handle_, &reg_params);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncRegisterResource", status);
        return nullptr;
    }

    // キャッシュに追加
    RegisteredTexture item{};
    item.raw_texture = texture;
    item.nvenc_registered_resource = reg_params.registeredResource;
    registered_textures_.push_back(item);

    return reg_params.registeredResource;
}

// ---------------------------------------------------------------------------
// テクスチャ登録の解除
// ---------------------------------------------------------------------------
void NvencEncoder::UnregisterResource(void* registered_resource) {
    if (!encoder_handle_ || !registered_resource) return;
    nv_api_.nvEncUnregisterResource(encoder_handle_, registered_resource);
}

// ---------------------------------------------------------------------------
// フレームをエンコーダに投入する（非同期）
// ---------------------------------------------------------------------------
int NvencEncoder::SubmitFrame(ID3D11Texture2D* texture, bool force_idr) {
    if (!initialized_ || !encoder_handle_) {
        std::printf("[NvencEncoder] 未初期化状態で SubmitFrame が呼ばれました\n");
        std::fflush(stdout);
        return -1;
    }

    std::lock_guard<std::mutex> lock(jobs_mutex_);
    std::lock_guard<std::mutex> api_lock(api_mutex_);

    if (free_output_buffers_.empty() ||
        (kUseAsyncEncoding && free_events_.empty())) {
        std::printf("[NvencEncoder] 非同期リソースプールが枯渇しました\n");
        std::fflush(stdout);
        return -1;
    }

    NV_ENC_OUTPUT_PTR output_buffer = free_output_buffers_.back();
    free_output_buffers_.pop_back();
    HANDLE completion_event = nullptr;
    if (kUseAsyncEncoding) {
        completion_event = free_events_.back();
        free_events_.pop_back();
    }

    // イベントを未シグナル状態にリセット
    if (completion_event) {
        ResetEvent(completion_event);
    }

    // 1. テクスチャの登録 (またはキャッシュ取得)
    void* registered_res = RegisterResource(texture);
    if (!registered_res) {
        std::printf("[NvencEncoder] RegisterResource 失敗\n");
        std::fflush(stdout);
        free_output_buffers_.push_back(output_buffer);
        if (completion_event) {
            free_events_.push_back(completion_event);
        }
        return -1;
    }

    // 2. リソースのマップ
    NV_ENC_MAP_INPUT_RESOURCE map_params{};
    map_params.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map_params.registeredResource = registered_res;

    NVENCSTATUS status = nv_api_.nvEncMapInputResource(encoder_handle_, &map_params);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncMapInputResource", status);
        std::fflush(stdout);
        free_output_buffers_.push_back(output_buffer);
        if (completion_event) {
            free_events_.push_back(completion_event);
        }
        return -1;
    }

    // 3. ピクチャパラメータの構成
    NV_ENC_PIC_PARAMS pic_params{};
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.inputWidth = width_;
    pic_params.inputHeight = height_;
    pic_params.inputPitch = width_;
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic_params.inputBuffer = map_params.mappedResource;
    pic_params.bufferFmt = map_params.mappedBufferFmt;
    pic_params.outputBitstream = output_buffer;
    pic_params.completionEvent = completion_event;

    if (force_idr) {
        pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
        pic_params.pictureType = NV_ENC_PIC_TYPE_IDR;
    } else {
        pic_params.pictureType = NV_ENC_PIC_TYPE_UNKNOWN;
    }

    // 4. エンコードの実行（非同期）
    status = nv_api_.nvEncEncodePicture(encoder_handle_, &pic_params);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncEncodePicture", status);
        std::fflush(stdout);
        nv_api_.nvEncUnmapInputResource(encoder_handle_, map_params.mappedResource);
        free_output_buffers_.push_back(output_buffer);
        if (completion_event) {
            free_events_.push_back(completion_event);
        }
        return -1;
    }

    int frame_id = next_frame_id_++;
    AsyncJob job{};
    job.frame_id = frame_id;
    job.texture = texture;
    job.mapped_resource = map_params.mappedResource;
    job.output_buffer = output_buffer;
    job.completion_event = completion_event;
    job.submitted_time_us = NowMicroseconds();
    pending_jobs_.push_back(job);

    return frame_id;
}

// ---------------------------------------------------------------------------
// 次に完了したエンコードフレームを取得する（非同期）
// ---------------------------------------------------------------------------
bool NvencEncoder::GetCompletedFrame(std::vector<uint8_t>& out_bitstream,
                                     ID3D11Texture2D** out_texture,
                                     int* out_frame_id,
                                     uint64_t timeout_us) {
    if (!initialized_ || !encoder_handle_) return false;

    // 先頭ジョブを取得
    AsyncJob job{};
    size_t pending_count = 0;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        if (pending_jobs_.empty()) {
            return false;
        }
        job = pending_jobs_.front();
        pending_count = pending_jobs_.size();
    }

    // ロックを解除してイベント待機（SubmitFrameをブロックしない）
    DWORD wait_ms = 0;
    if (timeout_us == UINT64_MAX) {
        wait_ms = INFINITE;
    } else if (timeout_us > 0) {
        wait_ms = static_cast<DWORD>(std::min<uint64_t>(timeout_us / 1000, INFINITE - 1));
    }

    DWORD result = job.completion_event
        ? WaitForSingleObject(job.completion_event, wait_ms)
        : WAIT_OBJECT_0;
    if (result != WAIT_OBJECT_0) {
        if (result == WAIT_TIMEOUT) {
            const uint64_t elapsed_us =
                NowMicroseconds() - job.submitted_time_us;
            static int stall_log_count = 0;
            if (elapsed_us > 100000 && ++stall_log_count % 30 == 0) {
                std::printf(
                    "[NvencEncoder] completion stall "
                    "(frame_id=%d, pending_jobs=%zu, elapsed_ms=%llu)\n",
                    job.frame_id,
                    pending_count,
                    static_cast<unsigned long long>(elapsed_us / 1000));
                std::fflush(stdout);
            }
        } else {
            std::printf("[NvencEncoder] WaitForSingleObject failed: %lu (frame_id=%d)\n", result, job.frame_id);
            std::fflush(stdout);
        }
        return false;
    }

    // 先頭ジョブをキューから削除
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        if (pending_jobs_.empty() || pending_jobs_.front().frame_id != job.frame_id) {
            // 単一コンシューマーでは発生しないはず
            return false;
        }
        pending_jobs_.pop_front();
    }

    bool bitstream_locked = false;
    {
        std::lock_guard<std::mutex> api_lock(api_mutex_);
        NV_ENC_LOCK_BITSTREAM lock_params{};
        lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock_params.outputBitstream = job.output_buffer;
        lock_params.doNotWait = job.completion_event ? 1 : 0;

        const NVENCSTATUS status =
            nv_api_.nvEncLockBitstream(encoder_handle_, &lock_params);
        if (status != NV_ENC_SUCCESS) {
            LOG_NVENC_ERR("nvEncLockBitstream", status);
            nv_api_.nvEncUnmapInputResource(
                encoder_handle_, job.mapped_resource);
        } else {
            bitstream_locked = true;
            if (lock_params.bitstreamSizeInBytes > 0) {
                auto* data =
                    static_cast<uint8_t*>(lock_params.bitstreamBufferPtr);
                out_bitstream.assign(
                    data, data + lock_params.bitstreamSizeInBytes);
            } else {
                out_bitstream.clear();
            }

            nv_api_.nvEncUnlockBitstream(
                encoder_handle_, job.output_buffer);
            nv_api_.nvEncUnmapInputResource(
                encoder_handle_, job.mapped_resource);
        }
    }

    if (!bitstream_locked) {
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            free_output_buffers_.push_back(job.output_buffer);
            if (job.completion_event) {
                free_events_.push_back(job.completion_event);
            }
        }
        // Report the failed frame as consumed so FramePipeline can release
        // its texture and submitted-frame entry instead of deadlocking once
        // the in-flight limit is reached.
        *out_texture = job.texture;
        *out_frame_id = job.frame_id;
        out_bitstream.clear();
        return true;
    }

    *out_texture = job.texture;
    *out_frame_id = job.frame_id;

    // リソースをプールに戻す
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    free_output_buffers_.push_back(job.output_buffer);
    if (job.completion_event) {
        free_events_.push_back(job.completion_event);
    }

    return true;
}

// ---------------------------------------------------------------------------
// フレームのエンコード（同期ラッパー、後方互換）
// ---------------------------------------------------------------------------
bool NvencEncoder::EncodeFrame(ID3D11Texture2D* texture, bool force_idr,
                                std::vector<uint8_t>& out_bitstream) {
    int frame_id = SubmitFrame(texture, force_idr);
    if (frame_id < 0) {
        return false;
    }

    ID3D11Texture2D* completed_texture = nullptr;
    int completed_frame_id = 0;
    return GetCompletedFrame(out_bitstream, &completed_texture, &completed_frame_id, UINT64_MAX);
}

bool NvencEncoder::ReconfigureBitrate(uint32_t bitrate_kbps) {
    if (!initialized_ || !encoder_handle_ || bitrate_kbps == 0) {
        return false;
    }

    EncoderConfig updated_encoder_config = config_;
    updated_encoder_config.bitrate_kbps = bitrate_kbps;
    updated_encoder_config.max_bitrate_kbps = bitrate_kbps;

    NV_ENC_CONFIG updated_encode_config = active_encode_config_;
    ApplyRateControl(updated_encode_config, updated_encoder_config);

    NV_ENC_INITIALIZE_PARAMS updated_initialize_params =
        active_initialize_params_;
    updated_initialize_params.encodeConfig = &updated_encode_config;

    NV_ENC_RECONFIGURE_PARAMS reconfigure_params{};
    reconfigure_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfigure_params.reInitEncodeParams = updated_initialize_params;
    reconfigure_params.forceIDR = 1;

    std::lock_guard<std::mutex> api_lock(api_mutex_);
    const NVENCSTATUS status = nv_api_.nvEncReconfigureEncoder(
        encoder_handle_, &reconfigure_params);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncReconfigureEncoder", status);
        return false;
    }

    config_ = updated_encoder_config;
    active_encode_config_ = updated_encode_config;
    active_initialize_params_ = updated_initialize_params;
    active_initialize_params_.encodeConfig = &active_encode_config_;

    std::printf(
        "[NvencEncoder] bitrate reconfigured: %u kbps\n",
        bitrate_kbps);
    std::fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// ストリームのフラッシュ
// ---------------------------------------------------------------------------
bool NvencEncoder::Flush(std::vector<uint8_t>& out_bitstream) {
    if (!initialized_ || !encoder_handle_) return false;

    out_bitstream.clear();

    // Bフレームなしの低遅延設定ではNVENC内部の遅延フレームはない。
    // 未完了ジョブだけを有限時間で回収し、停止処理を無限にブロックしない。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            if (pending_jobs_.empty()) {
                return true;
            }
        }

        std::vector<uint8_t> temp;
        ID3D11Texture2D* tex = nullptr;
        int id = 0;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            std::printf(
                "[NvencEncoder] flush deadline reached; pending jobs will be "
                "released during shutdown\n");
            std::fflush(stdout);
            return false;
        }

        const uint64_t remaining_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - now).count());
        if (!GetCompletedFrame(temp, &tex, &id, remaining_us)) {
            std::printf(
                "[NvencEncoder] flush timed out; pending jobs will be "
                "released during shutdown\n");
            std::fflush(stdout);
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// シャットダウンとリソース解放
// ---------------------------------------------------------------------------
void NvencEncoder::Shutdown() {
    initialized_ = false;

    if (encoder_handle_) {
        // キャッシュされたテクスチャ登録を解除
        for (auto& item : registered_textures_) {
            UnregisterResource(item.nvenc_registered_resource);
        }
        registered_textures_.clear();

        // 非同期リソースプールの破壊
        ReleaseAsyncResources();

        // セッションの破棄
        nv_api_.nvEncDestroyEncoder(encoder_handle_);
        encoder_handle_ = nullptr;
    }

    if (nvenc_dll_) {
        FreeLibrary(nvenc_dll_);
        nvenc_dll_ = nullptr;
    }

    device_.Reset();
    width_ = 0;
    height_ = 0;
    std::memset(&nv_api_, 0, sizeof(nv_api_));
}

} // namespace yamanoha
