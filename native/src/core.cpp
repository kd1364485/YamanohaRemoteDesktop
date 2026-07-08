// ============================================================================
// core.cpp
// YamanohaRemoteDesktop ネイティブ DLL の C API 実装
// ============================================================================

#include "core.h"
#include "gpu/d3d11_device.h"
#include "capture/capture_engine.h"
#include "capture/desktop_duplicator.h"
#include "capture/display_manager.h"
#include "encoder/nvenc_encoder.h"
#include "pipeline/frame_pipeline.h"
#include "pipeline/display_session_manager.h"
#include "webrtc/webrtc_streamer.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>
#include <atomic>
#include <algorithm>

#include <dbghelp.h>

using namespace yamanoha;

// ===========================================================================
// 内部グローバル状態
// ===========================================================================
namespace {

struct EngineState {
    bool initialized = false;
    bool com_initialized = false;
    std::mutex mutex;

    // ディスプレイ情報キャッシュ
    std::vector<Display> displays;
};

EngineState g_engine;

// WebRTCシグナリング・データチャネル用グローバルコールバック関数ポインタ
YamanohaLocalDescriptionCallback g_sdp_cb = nullptr;
YamanohaLocalCandidateCallback g_candidate_cb = nullptr;
YamanohaMessageCallback g_message_cb = nullptr;
YamanohaStateChangeCallback g_state_cb = nullptr;

// 非同期配信セッションマネージャ
// DisplaySessionManager は WebrtcStreamer のライフサイクルを
// ディスプレイ切替時にも維持し、FramePipeline のみ再構築する。
std::unique_ptr<DisplaySessionManager> g_session_manager;

// 配信中フラグ
std::atomic<bool> g_is_streaming{false};

// ---------------------------------------------------------------------------
// 内部ヘルパー関数
// ---------------------------------------------------------------------------

const Display* FindDisplay(
    const std::vector<Display>& displays,
    int adapter_index,
    int output_index) {
    for (const auto& display : displays) {
        if (display.adapter_index == adapter_index &&
            display.output_index == output_index) {
            return &display;
        }
    }
    return nullptr;
}

void SetupStreamerCallbacks(WebrtcStreamer* streamer) {
    streamer->SetLocalDescriptionCallback(
        [](const std::string& sdp, const std::string& type) {
            if (g_sdp_cb) g_sdp_cb(sdp.c_str(), type.c_str());
        });
    streamer->SetLocalCandidateCallback(
        [](const std::string& candidate, const std::string& mid) {
            if (g_candidate_cb) g_candidate_cb(candidate.c_str(), mid.c_str());
        });
    streamer->SetMessageCallback(
        [](const std::string& msg) {
            if (g_message_cb) g_message_cb(msg.c_str());
        });
    streamer->SetStateChangeCallback(
        [](bool connected) {
            if (g_state_cb) g_state_cb(connected ? 1 : 0);
        });
}

} // anonymous namespace

// ===========================================================================
// C API 実装
// ===========================================================================

extern "C" {

// ---------------------------------------------------------------------------
// 初期化
// ---------------------------------------------------------------------------
YAMANOHA_API int yamanoha_init() {
    std::lock_guard<std::mutex> lock(g_engine.mutex);

    if (g_engine.initialized) {
        return 0;
    }

    // コンソール出力を UTF-8 に設定して日本語ログの文字化けを防止
    SetConsoleOutputCP(CP_UTF8);

    // 未処理の Win32 例外をキャッチしてログ出力 + ミニダンプ保存
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ExceptionInfo) -> LONG {
        std::printf("[Core] 未処理の例外: 0x%08lX\n", ExceptionInfo->ExceptionRecord->ExceptionCode);
        std::fflush(stdout);

        HANDLE hFile = CreateFileA(
            "yamanoha_crash.dmp",
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        
        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mdei{};
            mdei.ThreadId = GetCurrentThreadId();
            mdei.ExceptionPointers = ExceptionInfo;
            mdei.ClientPointers = FALSE;

            BOOL ok = MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                hFile,
                MiniDumpNormal,
                &mdei,
                nullptr,
                nullptr
            );
            CloseHandle(hFile);
            if (ok) {
                std::printf("[Core] クラッシュダンプを保存しました: yamanoha_crash.dmp\n");
            } else {
                std::printf("[Core] クラッシュダンプの保存に失敗: 0x%08lX\n", GetLastError());
            }
            std::fflush(stdout);
        }

        return EXCEPTION_EXECUTE_HANDLER;
    });

    // COM 初期化（マルチスレッドモード）
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE) {
        g_engine.com_initialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        // 既に別スレッドモード（STAなど）で初期化済みの場合でも継続可能
        g_engine.com_initialized = false;
    } else {
        std::printf("[Core] COM初期化失敗: 0x%08lX\n", hr);
        return -1;
    }

    g_engine.initialized = true;
    std::printf("[Core] エンジン初期化成功\n");
    std::fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// ストリーミング停止の内部実装
// mutex ロック済みの呼び出し元から呼び出すこと
// ---------------------------------------------------------------------------
void StopStreamInternal() {
    // 配信停止フラグ設定
    g_is_streaming = false;

    // セッションマネージャの停止（パイプライン + ストリーマー）
    if (g_session_manager) {
        g_session_manager->Stop();
        g_session_manager.reset();
    }

    std::printf("[Core] ストリーミング配信を停止しました\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// ストリーミング停止
// ---------------------------------------------------------------------------
YAMANOHA_API int yamanoha_stop_stream() {
    std::lock_guard<std::mutex> lock(g_engine.mutex);
    StopStreamInternal();
    return 0;
}

// ---------------------------------------------------------------------------
// 終了
// ---------------------------------------------------------------------------
YAMANOHA_API void yamanoha_shutdown() {
    std::lock_guard<std::mutex> lock(g_engine.mutex);

    if (!g_engine.initialized) {
        return;
    }

    // ストリーミングパイプラインの停止 (mutex 既に取得済みのため内部関数を呼ぶ)
    StopStreamInternal();

    // ディスプレイキャッシュクリア
    g_engine.displays.clear();

    // COM 終了
    if (g_engine.com_initialized) {
        CoUninitialize();//COM終了
        g_engine.com_initialized = false;
    }

    g_engine.initialized = false;
    std::printf("[Core] エンジンシャットダウン完了\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// ディスプレイ列挙
// ---------------------------------------------------------------------------
YAMANOHA_API int yamanoha_list_displays(YamanohaDisplayInfo* out, int max_count) {
    std::lock_guard<std::mutex> lock(g_engine.mutex);

    if (!g_engine.initialized) {
        std::printf("[Core] 未初期化状態でディスプレイ列挙が呼ばれました\n");
        return -1;
    }

    if (!out || max_count <= 0) {
        return -2;
    }

    // 最新のディスプレイ情報を列挙してキャッシュ
    g_engine.displays = DisplayManager::EnumerateDisplays();

    int count = static_cast<int>((std::min)(static_cast<size_t>(max_count), g_engine.displays.size()));

    for (int i = 0; i < count; ++i) {
        const auto& src = g_engine.displays[i];
        auto& dst = out[i];

        // 各フィールドをC互換構造体に安全コピー
        wcsncpy_s(dst.name, _countof(dst.name), src.name.c_str(), _TRUNCATE);
        wcsncpy_s(dst.adapter_name, _countof(dst.adapter_name), src.adapter_name.c_str(), _TRUNCATE);

        dst.adapter_index = src.adapter_index;
        dst.output_index = src.output_index;
        dst.x = src.bounds.left;
        dst.y = src.bounds.top;
        dst.width = src.width;
        dst.height = src.height;
        dst.refresh_rate = src.refresh_rate;
        dst.is_primary = src.is_primary ? 1 : 0;
    }

    return count;
}

// ---------------------------------------------------------------------------
// WebRTCコールバック登録
// ---------------------------------------------------------------------------
YAMANOHA_API void yamanoha_set_callbacks(
    YamanohaLocalDescriptionCallback sdp_cb,
    YamanohaLocalCandidateCallback candidate_cb,
    YamanohaMessageCallback message_cb,
    YamanohaStateChangeCallback state_cb
) {
    std::lock_guard<std::mutex> lock(g_engine.mutex);
    g_sdp_cb = sdp_cb;
    g_candidate_cb = candidate_cb;
    g_message_cb = message_cb;
    g_state_cb = state_cb;
    
    std::printf("[Core] WebRTC FFI コールバック関数ポインタが設定されました\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// ストリーミング開始
// ---------------------------------------------------------------------------
YAMANOHA_API int yamanoha_start_stream(
    int adapter_index,
    int output_index,
    int width,
    int height,
    int fps,
    int bitrate_kbps
) {
    std::lock_guard<std::mutex> lock(g_engine.mutex);

    if (!g_engine.initialized) {
        std::printf("[Core] 未初期化状態で配信開始が呼ばれました\n");
        return -1;
    }

    // 既に起動中の場合は一度停止する (mutex 既に取得済みのため内部関数を呼ぶ)
    if (g_is_streaming) {
        StopStreamInternal();
    }

    // 1. ディスプレイ列挙
    DisplayManager display_manager;
    if (!display_manager.Refresh()) {
        std::printf("[Core] ディスプレイ列挙に失敗しました\n");
        return -2;
    }
    display_manager.PrintDisplayInfo();
    const auto& displays = display_manager.GetDisplays();

    // 2. 指定ディスプレイを検索、失敗時は代替を試行
    const Display* target = FindDisplay(displays, adapter_index, output_index);

    g_session_manager = std::make_unique<DisplaySessionManager>();

    if (target) {
        std::printf("[Core] セッション開始を試行: %ls (Adapter=%d, Output=%d)\n",
                    target->display_id.c_str(),
                    target->adapter_index,
                    target->output_index);
        if (!g_session_manager->Start(*target, fps, bitrate_kbps)) {
            target = nullptr;
        }
    }

    if (!target) {
        for (const auto& display : displays) {
            if (display.adapter_index == adapter_index &&
                display.output_index == output_index) {
                continue;
            }
            std::printf("[Core] 代替ディスプレイで試行: %ls (Adapter=%d, Output=%d)\n",
                        display.display_id.c_str(),
                        display.adapter_index,
                        display.output_index);
            if (g_session_manager->Start(display, fps, bitrate_kbps)) {
                target = &display;
                break;
            }
        }
    }

    if (!target) {
        std::printf("[Core] セッション開始に失敗しました\n");
        g_session_manager.reset();
        return -4;
    }

    // 3. WebRTC ストリーマーの初期化
    auto* streamer = g_session_manager->GetStreamer();
    if (!streamer) {
        std::printf("[Core] ストリーマーが取得できません\n");
        g_session_manager.reset();
        return -5;
    }

    SetupStreamerCallbacks(streamer);

    if (!streamer->Initialize()) {
        std::printf("[Core] WebrtcStreamer 初期化失敗\n");
        g_session_manager.reset();
        return -6;
    }

    // 4. パイプラインスレッド開始
    g_session_manager->GetPipeline()->StartThreads();
    g_is_streaming = true;

    std::printf("[Core] 配信開始 (%u x %u, %d FPS)\n",
                g_session_manager->GetWidth(),
                g_session_manager->GetHeight(),
                fps);
    std::fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// シグナリング処理
// ---------------------------------------------------------------------------
YAMANOHA_API int yamanoha_process_signaling(
    const char* msg_type,
    const char* sdp_or_candidate,
    const char* mid
) {
    std::lock_guard<std::mutex> lock(g_engine.mutex);
    if (!g_session_manager || !msg_type || !sdp_or_candidate) {
        return 0;
    }

    auto* streamer = g_session_manager->GetStreamer();
    if (!streamer) {
        return 0;
    }

    std::string type_str(msg_type);
    std::string content_str(sdp_or_candidate);
    std::string mid_str = mid ? std::string(mid) : "";

    bool success = streamer->ProcessSignalingMessage(type_str, content_str, mid_str);
    return success ? 1 : 0;
}

// ---------------------------------------------------------------------------
// データチャネルメッセージ送信
// ---------------------------------------------------------------------------
YAMANOHA_API int yamanoha_send_message(const char* message) {
    std::lock_guard<std::mutex> lock(g_engine.mutex);
    if (!g_session_manager || !message) {
        return 0;
    }

    auto* streamer = g_session_manager->GetStreamer();
    if (!streamer) {
        return 0;
    }

    std::string msg_str(message);
    bool success = streamer->SendMessageOverDataChannel(msg_str);
    return success ? 1 : 0;
}

// ---------------------------------------------------------------------------
// ディスプレイ切替（WebRTC接続維持）
// ---------------------------------------------------------------------------
YAMANOHA_API int yamanoha_switch_display(
    int adapter_index,
    int output_index,
    int width,
    int height,
    int fps,
    int bitrate_kbps
) {
    std::lock_guard<std::mutex> lock(g_engine.mutex);

    if (!g_session_manager) {
        std::printf("[Core] セッションが存在しません\n");
        return -1;
    }

    DisplayManager display_manager;
    if (!display_manager.Refresh()) {
        std::printf("[Core] ディスプレイ列挙に失敗しました\n");
        return -2;
    }

    const Display* target = FindDisplay(
        display_manager.GetDisplays(), adapter_index, output_index);
    if (!target) {
        std::printf("[Core] ディスプレイが見つかりません (Adapter=%d, Output=%d)\n",
                    adapter_index, output_index);
        return -3;
    }

    std::printf("[Core] ディスプレイ切替: %ls (Adapter=%d, Output=%d)\n",
                target->display_id.c_str(),
                target->adapter_index,
                target->output_index);

    if (!g_session_manager->SwitchDisplay(*target, fps, bitrate_kbps)) {
        std::printf("[Core] ディスプレイ切替に失敗しました\n");
        return -4;
    }

    std::printf("[Core] ディスプレイ切替成功 (%u x %u)\n",
                g_session_manager->GetWidth(),
                g_session_manager->GetHeight());
    return 0;
}

} // extern "C"
