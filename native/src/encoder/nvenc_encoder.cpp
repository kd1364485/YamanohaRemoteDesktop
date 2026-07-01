// ============================================================================
// nvenc_encoder.cpp
// NVIDIA NVENC SDK を使用したハードウェア H.264 エンコーダクラスの実装
// ============================================================================
#include "nvenc_encoder.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace yamanoha {

// エラーロギングマクロ
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

    // 2. プリセットとチューニングの GUID を設定
    GUID encode_guid = NV_ENC_CODEC_H264_GUID;
    
    // プリセットの選択 (P1〜P7)
    GUID preset_guid = NV_ENC_PRESET_P1_GUID; // デフォルト (最高速)
    if (config.preset >= 1 && config.preset <= 7) {
        static const GUID presets[] = {
            NV_ENC_PRESET_P1_GUID, NV_ENC_PRESET_P2_GUID, NV_ENC_PRESET_P3_GUID,
            NV_ENC_PRESET_P4_GUID, NV_ENC_PRESET_P5_GUID, NV_ENC_PRESET_P6_GUID,
            NV_ENC_PRESET_P7_GUID
        };
        preset_guid = presets[config.preset - 1];
    }

    // 超低遅延チューニングの指定
    NV_ENC_TUNING_INFO tuning_info = config.zero_latency ? NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY : NV_ENC_TUNING_INFO_LOW_LATENCY;

    // 3. プリセット設定の取得
    NV_ENC_PRESET_CONFIG preset_config{};
    preset_config.version = NV_ENC_PRESET_CONFIG_VER;
    preset_config.presetCfg.version = NV_ENC_CONFIG_VER;

    status = nv_api_.nvEncGetEncodePresetConfigEx(
        encoder_handle_, encode_guid, preset_guid, tuning_info, &preset_config);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncGetEncodePresetConfigEx", status);
        return false;
    }

    // 4. カスタムパラメータの設定
    NV_ENC_CONFIG encode_config = preset_config.presetCfg;
    
    // GOP サイズ（キーフレーム間隔）
    encode_config.gopLength = config.gop_size;
    encode_config.frameIntervalP = 1; // Bフレームを使用しない (I/Pフレームのみ)
    
    // レート制御
    encode_config.rcParams.version = NV_ENC_RC_PARAMS_VER;
    if (config.use_cbr) {
        encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    } else {
        encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    }
    encode_config.rcParams.averageBitRate = config.bitrate_kbps * 1000;
    encode_config.rcParams.maxBitRate = config.max_bitrate_kbps * 1000;
    encode_config.rcParams.vbvBufferSize = encode_config.rcParams.averageBitRate / (config.fps > 0 ? config.fps : 30); // 1フレーム分のバッファ
    encode_config.rcParams.vbvInitialDelay = encode_config.rcParams.vbvBufferSize;

    if (config.zero_latency) {
        encode_config.rcParams.lowDelayKeyFrameScale = 1;
    }

    // コーデック固有設定 (H.264)
    encode_config.encodeCodecConfig.h264Config.idrPeriod = config.gop_size;
    encode_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1; // 毎キーフレームにSPS/PPSを付与
    encode_config.encodeCodecConfig.h264Config.sliceMode = 0;
    encode_config.encodeCodecConfig.h264Config.sliceModeData = 0;

    // 5. エンコーダの初期化パラメータ設定
    NV_ENC_INITIALIZE_PARAMS init_params{};
    init_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_params.encodeGUID = encode_guid;
    init_params.presetGUID = preset_guid;
    init_params.tuningInfo = tuning_info;
    init_params.encodeWidth = config.width;
    init_params.encodeHeight = config.height;
    init_params.darWidth = config.width;
    init_params.darHeight = config.height;
    init_params.frameRateNum = config.fps;
    init_params.frameRateDen = 1;
    init_params.enablePTD = 1; // ピクチャタイプの決定をSDKに任せる
    init_params.reportSliceOffsets = 0;
    init_params.enableSubFrameWrite = 0;
    init_params.enableEncodeAsync = 1; // 非同期エンコードを有効化
    init_params.encodeConfig = &encode_config;

    status = nv_api_.nvEncInitializeEncoder(encoder_handle_, &init_params);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncInitializeEncoder", status);
        return false;
    }

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

        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            std::printf("[NvencEncoder] 完了イベントの作成に失敗しました\n");
            nv_api_.nvEncDestroyBitstreamBuffer(encoder_handle_, bitstream_params.bitstreamBuffer);
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
    // 未完了ジョブがあればクリア
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        pending_jobs_.clear();
    }

    for (auto buffer : free_output_buffers_) {
        if (buffer && encoder_handle_) {
            nv_api_.nvEncDestroyBitstreamBuffer(encoder_handle_, buffer);
        }
    }
    free_output_buffers_.clear();

    for (auto event : free_events_) {
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

    if (free_output_buffers_.empty() || free_events_.empty()) {
        std::printf("[NvencEncoder] 非同期リソースプールが枯渇しました\n");
        std::fflush(stdout);
        return -1;
    }

    NV_ENC_OUTPUT_PTR output_buffer = free_output_buffers_.back();
    free_output_buffers_.pop_back();
    HANDLE completion_event = free_events_.back();
    free_events_.pop_back();

    // イベントを未シグナル状態にリセット
    ResetEvent(completion_event);

    // 1. テクスチャの登録 (またはキャッシュ取得)
    void* registered_res = RegisterResource(texture);
    if (!registered_res) {
        std::printf("[NvencEncoder] RegisterResource 失敗\n");
        std::fflush(stdout);
        free_output_buffers_.push_back(output_buffer);
        free_events_.push_back(completion_event);
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
        free_events_.push_back(completion_event);
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
        free_events_.push_back(completion_event);
        return -1;
    }

    int frame_id = next_frame_id_++;
    AsyncJob job{};
    job.frame_id = frame_id;
    job.texture = texture;
    job.mapped_resource = map_params.mappedResource;
    job.output_buffer = output_buffer;
    job.completion_event = completion_event;
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
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        if (pending_jobs_.empty()) {
            return false;
        }
        job = pending_jobs_.front();
    }

    // ロックを解除してイベント待機（SubmitFrameをブロックしない）
    DWORD wait_ms = 0;
    if (timeout_us == UINT64_MAX) {
        wait_ms = INFINITE;
    } else if (timeout_us > 0) {
        wait_ms = static_cast<DWORD>(std::min<uint64_t>(timeout_us / 1000, INFINITE - 1));
    }

    DWORD result = WaitForSingleObject(job.completion_event, wait_ms);
    if (result != WAIT_OBJECT_0) {
        // タイムアウト時はデバッグログを出力
        if (result == WAIT_TIMEOUT) {
            static int timeout_log_count = 0;
            if (++timeout_log_count % 100 == 0) {
                std::printf("[NvencEncoder] GetCompletedFrame timeout (frame_id=%d, pending_jobs=%zu)\n",
                            job.frame_id, pending_jobs_.size());
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

    // ロックは完了を待たずに行う（イベント済みなので即座に成功するはず）
    NV_ENC_LOCK_BITSTREAM lock_params{};
    lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock_params.outputBitstream = job.output_buffer;
    lock_params.doNotWait = 1;

    NVENCSTATUS status = nv_api_.nvEncLockBitstream(encoder_handle_, &lock_params);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncLockBitstream", status);
        nv_api_.nvEncUnmapInputResource(encoder_handle_, job.mapped_resource);
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        free_output_buffers_.push_back(job.output_buffer);
        free_events_.push_back(job.completion_event);
        return false;
    }

    if (lock_params.bitstreamSizeInBytes > 0) {
        auto* data = static_cast<uint8_t*>(lock_params.bitstreamBufferPtr);
        out_bitstream.assign(data, data + lock_params.bitstreamSizeInBytes);
    } else {
        out_bitstream.clear();
    }

    nv_api_.nvEncUnlockBitstream(encoder_handle_, job.output_buffer);
    nv_api_.nvEncUnmapInputResource(encoder_handle_, job.mapped_resource);

    *out_texture = job.texture;
    *out_frame_id = job.frame_id;

    // リソースをプールに戻す
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    free_output_buffers_.push_back(job.output_buffer);
    free_events_.push_back(job.completion_event);

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

// ---------------------------------------------------------------------------
// ストリームのフラッシュ
// ---------------------------------------------------------------------------
bool NvencEncoder::Flush(std::vector<uint8_t>& out_bitstream) {
    if (!initialized_ || !encoder_handle_) return false;

    out_bitstream.clear();

    // 未完了ジョブがあれば完了まで待つ
    {
        std::vector<uint8_t> temp;
        ID3D11Texture2D* tex = nullptr;
        int id = 0;
        while (GetCompletedFrame(temp, &tex, &id, UINT64_MAX)) {
            // フラッシュ前に未処理のビットストリームを破棄
        }
    }

    // 非同期リソースを一時取得
    NV_ENC_OUTPUT_PTR output_buffer = nullptr;
    HANDLE event = nullptr;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        if (free_output_buffers_.empty() || free_events_.empty()) {
            return false;
        }
        output_buffer = free_output_buffers_.back();
        free_output_buffers_.pop_back();
        event = free_events_.back();
        free_events_.pop_back();
        ResetEvent(event);
    }

    // フラッシュ用の空入力で nvEncEncodePicture を呼び出す
    NV_ENC_PIC_PARAMS pic_params{};
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.inputBuffer = nullptr;
    pic_params.outputBitstream = output_buffer;
    pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS; // End of Stream
    pic_params.completionEvent = event;

    NVENCSTATUS status = nv_api_.nvEncEncodePicture(encoder_handle_, &pic_params);
    if (status != NV_ENC_SUCCESS) {
        LOG_NVENC_ERR("nvEncEncodePicture (EOS)", status);
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        free_output_buffers_.push_back(output_buffer);
        free_events_.push_back(event);
        return false;
    }

    WaitForSingleObject(event, INFINITE);

    // ロックしてフラッシュされたデータを取り出す
    NV_ENC_LOCK_BITSTREAM lock_params{};
    lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock_params.outputBitstream = output_buffer;
    lock_params.doNotWait = 1;

    status = nv_api_.nvEncLockBitstream(encoder_handle_, &lock_params);
    if (status == NV_ENC_SUCCESS) {
        if (lock_params.bitstreamSizeInBytes > 0) {
            auto* data = static_cast<uint8_t*>(lock_params.bitstreamBufferPtr);
            out_bitstream.assign(data, data + lock_params.bitstreamSizeInBytes);
        }
        nv_api_.nvEncUnlockBitstream(encoder_handle_, output_buffer);
    }

    std::lock_guard<std::mutex> lock(jobs_mutex_);
    free_output_buffers_.push_back(output_buffer);
    free_events_.push_back(event);

    return true;
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
