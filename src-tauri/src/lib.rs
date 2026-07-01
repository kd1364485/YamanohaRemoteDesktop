use std::sync::Mutex;
use once_cell::sync::Lazy;
use tokio_tungstenite::{connect_async, tungstenite::protocol::Message};
use futures_util::{StreamExt, SinkExt};

mod ffi;
mod screen_capture;
mod input_control;
mod network;
mod session;

// WebSocket接続キャンセル用のoneshot送信チャネル
static WS_CANCEL_TX: Lazy<Mutex<Option<tokio::sync::oneshot::Sender<()>>>> = Lazy::new(|| Mutex::new(None));

#[tauri::command]
async fn start_host(
    app: tauri::AppHandle,
    server_url: String,
    token: String,
    device_name: String
) -> Result<(), String> {
    // 既存のホストセッションがあれば一度停止
    let _ = stop_host().await;

    std::println!("[Rust] ネイティブ C++ エンジン初期化開始...");
    // 1. C++ DLLのロードとコールバック設定
    ffi::init_engine(app)?;

    // 2. ディスプレイ一覧を取得して、後段のクライアント接続時に使用する配信パラメータを決定する
    let displays = ffi::list_displays().unwrap_or_default();
    let mut adapter_idx = 0;
    let mut output_idx = 0;
    let mut w = 1920;
    let mut h = 1080;
    let mut fps = 30;

    // プライマリディスプレイの情報を優先適用
    if let Some(primary) = displays.iter().find(|d| d.is_primary == 1) {
        adapter_idx = primary.adapter_index;
        output_idx = primary.output_index;
        w = primary.width as i32;
        h = primary.height as i32;
        if primary.refresh_rate > 10.0 {
            fps = primary.refresh_rate as i32;
        }
    } else if let Some(first) = displays.first() {
        adapter_idx = first.adapter_index;
        output_idx = first.output_index;
        w = first.width as i32;
        h = first.height as i32;
    }

    // CPUコピーfallbackが発生するハイブリッドGPU環境では、60fpsを維持すると
    // PCIe帯域とCPUコピーがボトルネックになりラグが大きくなるため、30fpsに制限する
    fps = 60;

    // 3. WebSocket シグナリングクライアントタスクのセットアップ
    let (ws_tx, mut ws_rx) = tokio::sync::mpsc::unbounded_channel::<String>();
    *ffi::WS_SENDER.lock().unwrap() = Some(ws_tx);

    let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel::<()>();
    *WS_CANCEL_TX.lock().unwrap() = Some(cancel_tx);

    let server_url_clone = server_url.clone();
    let token_clone = token.clone();
    let device_name_clone = device_name.clone();
    
    // インスタンスIDの生成 (UNIXタイムスタンプミリ秒を使用)
    let instance_id = format!(
        "inst-native-{}",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis()
    );

    // 非同期シグナリングループタスクの起動 (Tokio スレッド)
    tokio::spawn(async move {
        std::println!("[Rust WS] シグナリングサーバーに接続中: {}", server_url_clone);
        
        let (ws_stream, _) = match connect_async(&server_url_clone).await {
            Ok(val) => val,
            Err(e) => {
                std::println!("[Rust WS] シグナリングサーバーへの接続に失敗しました: {}", e);
                return;
            }
        };
        std::println!("[Rust WS] シグナリングサーバーに接続完了");

        let (mut write, mut read) = ws_stream.split();
        
        // 最初のメッセージとしてシグナリング認証要求 (type: "auth") を送信
        let auth_msg = serde_json::json!({
            "type": "auth",
            "token": token_clone
        }).to_string();

        if let Err(e) = write.send(Message::Text(auth_msg)).await {
            std::println!("[Rust WS] 認証メッセージの送信に失敗しました: {}", e);
            return;
        }

        let mut cancel_rx = cancel_rx;

        loop {
            tokio::select! {
                // 停止（キャンセル）通知の受信
                _ = &mut cancel_rx => {
                    std::println!("[Rust WS] 配信停止指示を受信しました");
                    break;
                }
                
                // C++ FFI コールバックから発生した送信メッセージ（SDP/ICE）の処理
                Some(msg) = ws_rx.recv() => {
                    if let Err(e) = write.send(Message::Text(msg)).await {
                        std::println!("[Rust WS] シグナリングデータの送信に失敗: {}", e);
                        break;
                    }
                }

                // シグナリングサーバーからの受信メッセージの処理
                msg_res = read.next() => {
                    let msg = match msg_res {
                        Some(Ok(Message::Text(t))) => t,
                        Some(Ok(_)) => continue,
                        Some(Err(e)) => {
                            std::println!("[Rust WS] 受信エラー: {}", e);
                            break;
                        }
                        None => {
                            std::println!("[Rust WS] サーバーが切断しました");
                            break;
                        }
                    };

                    // メッセージをパースしてプロトコルに従った処理を行う
                    if let Ok(j) = serde_json::from_str::<serde_json::Value>(&msg) {
                        let msg_type = j["type"].as_str().unwrap_or("");
                        
                        match msg_type {
                            // 1. 認証が通ったらホストとして登録
                            "auth-ok" => {
                                std::println!("[Rust WS] 認証成功。ホスト登録メッセージを送信します...");
                                let host_msg = serde_json::json!({
                                    "type": "host",
                                    "deviceName": device_name_clone,
                                    "instanceId": instance_id
                                }).to_string();
                                let _ = write.send(Message::Text(host_msg)).await;
                            }

                            // 2. ホスト準備完了の通知
                            "host-ready" => {
                                let host_id = j["hostId"].as_str().unwrap_or("");
                                std::println!("[Rust WS] 接続待ち状態になりました。Host ID: {}", host_id);
                            }

                            // 3. クライアントが接続要求したタイミングで初めて配信とWebRTCをキック (遅延初期化)
                            "client-connected" => {
                                std::println!("[Rust WS] クライアント接続を検知。C++ストリームを開始します...");
                                std::println!(
                                    "[Rust] ストリーム開始: Adapter={}, Output={}, 解像度={}x{}, FPS={}",
                                    adapter_idx, output_idx, w, h, fps
                                );
                                if let Err(e) = ffi::start_stream(adapter_idx, output_idx, w, h, fps, 4000) {
                                    std::println!("[Rust WS] C++ストリーム開始エラー: {}", e);
                                }
                            }

                            // 4. クライアントからの WebRTC シグナリング処理
                            "webrtc-answer" => {
                                let sdp = j["sdp"].as_str().unwrap_or("");
                                std::println!("[Rust WS] Answer SDP を受信しました。C++へ転送します...");
                                let _ = ffi::process_signaling("answer", sdp, None);
                            }
                            "webrtc-ice" => {
                                let candidate_val = &j["candidate"];
                                let candidate_str = if candidate_val.is_object() {
                                    candidate_val["candidate"].as_str().unwrap_or("")
                                } else {
                                    candidate_val.as_str().unwrap_or("")
                                };

                                let mid_str = match j["mid"].as_str() {
                                    Some(m) => Some(m),
                                    None => candidate_val["sdpMid"].as_str(),
                                };

                                std::println!("[Rust WS] Remote ICE Candidate を受信しました。C++へ転送します: {}", candidate_str);
                                let _ = ffi::process_signaling("candidate", candidate_str, mid_str);
                            }
                            "input" => {
                                ffi::handle_remote_input(&msg);
                            }
                            _ => {}
                        }
                    }
                }
            }
        }
        std::println!("[Rust WS] WebSocket クローズ処理実行中...");
        let _ = write.close().await;
    });

    Ok(())
}

#[tauri::command]
async fn stop_host() -> Result<(), String> {
    std::println!("[Rust] ストリーミング配信の停止を要求中...");

    // 1. WebSocket シグナリングタスクのキャンセル
    if let Some(cancel_tx) = WS_CANCEL_TX.lock().unwrap().take() {
        let _ = cancel_tx.send(());
    }
    *ffi::WS_SENDER.lock().unwrap() = None;

    // 2. C++ 配信ストリームの停止
    let _ = ffi::stop_stream();

    // 3. FFI 接続のクリーンアップとシャットダウン
    ffi::shutdown_engine();

    std::println!("[Rust] 配信停止完了");
    Ok(())
}

#[tauri::command]
fn connect_to_host(code: String) -> Result<String, String> {
    session::connect_session(&code)
}

#[tauri::command]
fn disconnect_session() -> Result<(), String> {
    session::disconnect()
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            start_host,
            stop_host,
            connect_to_host,
            disconnect_session,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
