// ============================================================================
// core.h
// YamanohaRemoteDesktop ネイティブ DLL の公開 C API
// Rust (Tauri) の FFI から呼び出すためのインターフェース
// ============================================================================
#pragma once

#ifdef YAMANOHA_EXPORTS
#define YAMANOHA_API __declspec(dllexport)
#else
#define YAMANOHA_API __declspec(dllimport)
#endif

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// ディスプレイ情報（C 互換構造体）
// ---------------------------------------------------------------------------
typedef struct {
    wchar_t name[128];           // ディスプレイ名
    wchar_t adapter_name[128];   // GPU アダプタ名
    int adapter_index;           // アダプタインデックス
    int output_index;            // 出力インデックス
    int32_t x, y;                // 画面位置（デスクトップ座標）
    uint32_t width, height;      // 解像度
    float refresh_rate;          // リフレッシュレート（Hz）
    int is_primary;              // プライマリディスプレイなら 1
} YamanohaDisplayInfo;

// ---------------------------------------------------------------------------
// WebRTC シグナリング・データチャネル用コールバック関数定義
// ---------------------------------------------------------------------------

// ローカルSDP（Offer/Answer）が生成された時のコールバック
typedef void (*YamanohaLocalDescriptionCallback)(const char* sdp, const char* type);
// ローカルICE候補が生成された時のコールバック
typedef void (*YamanohaLocalCandidateCallback)(const char* candidate, const char* mid);
// クライアント（ブラウザ）から DataChannel 経由で制御コマンドメッセージを受信した時のコールバック
typedef void (*YamanohaMessageCallback)(const char* message);
// 接続状態が変化した時のコールバック (connected: 1 = 接続, 0 = 切断)
typedef void (*YamanohaStateChangeCallback)(int connected);

// ---------------------------------------------------------------------------
// 初期化・終了
// ---------------------------------------------------------------------------

/// @brief ネイティブエンジンを初期化する
/// @return 成功時 0、失敗時 負のエラーコード
YAMANOHA_API int yamanoha_init();

/// @brief ネイティブエンジンを終了しリソースを解放する
YAMANOHA_API void yamanoha_shutdown();

// ---------------------------------------------------------------------------
// ディスプレイ列挙
// ---------------------------------------------------------------------------

/// @brief 利用可能なディスプレイを列挙する
/// @param out 結果を格納する配列（呼び出し側が確保）
/// @param max_count 配列の最大要素数
/// @return 格納されたディスプレイ数。エラー時は負の値
YAMANOHA_API int yamanoha_list_displays(YamanohaDisplayInfo* out, int max_count);

// ---------------------------------------------------------------------------
// WebRTC配信制御 API
// ---------------------------------------------------------------------------

/// @brief コールバック関数を登録する
/// @param sdp_cb ローカルSDP受信用コールバック
/// @param candidate_cb ローカルICE受信用コールバック
/// @param message_cb 制御メッセージ受信用コールバック
/// @param state_cb 接続状態変化受信用コールバック
YAMANOHA_API void yamanoha_set_callbacks(
    YamanohaLocalDescriptionCallback sdp_cb,
    YamanohaLocalCandidateCallback candidate_cb,
    YamanohaMessageCallback message_cb,
    YamanohaStateChangeCallback state_cb
);

/// @brief 指定したディスプレイのキャプチャ・エンコード・WebRTC配信パイプラインを開始する
/// @param adapter_index 使用するGPUアダプタのインデックス (0が通常RTX)
/// @param output_index 使用するディスプレイ出力インデックス
/// @param width 配信目標解像度（幅）
/// @param height 配信目標解像度（高さ）
/// @param fps 配信フレームレート
/// @param bitrate_kbps 目標ビットレート (kbps)
/// @return 成功時 0、失敗時 負のエラーコード
YAMANOHA_API int yamanoha_start_stream(
    int adapter_index,
    int output_index,
    int width,
    int height,
    int fps,
    int bitrate_kbps
);

/// @brief キャプチャ・エンコード・WebRTC配信パイプラインを停止する
/// @return 成功時 0
YAMANOHA_API int yamanoha_stop_stream();

/// @brief リモート（ブラウザ）から受け取ったシグナリングメッセージ（SDP または Candidate）を処理する
/// @param msg_type メッセージの種別 ("offer", "answer", "candidate")
/// @param sdp_or_candidate SDPテキスト、またはICE Candidate文字列
/// @param mid Candidateのmid (SDPなどのCandidateに付与、SDP時は nullptr)
/// @return 成功時 1、失敗時 0
YAMANOHA_API int yamanoha_process_signaling(
    const char* msg_type,
    const char* sdp_or_candidate,
    const char* mid
);

/// @brief DataChannel を経由してクライアント（ブラウザ）へメッセージを送信する
/// @param message 送信する文字列メッセージ
/// @return 成功時 1、失敗時 0
YAMANOHA_API int yamanoha_send_message(const char* message);

#ifdef __cplusplus
}
#endif
