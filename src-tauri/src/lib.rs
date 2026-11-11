use std::sync::Mutex;
use once_cell::sync::Lazy;
use tokio_tungstenite::{connect_async, tungstenite::protocol::Message};
use futures_util::{StreamExt, SinkExt};

mod ffi;//C++　DLLとの橋渡し
mod input_control;//リモート入力処理
mod signaling;//シグナリングクライアント
//Websocketループを停止させるためのキャンセル信号
static WS_CANCEL_TX: Lazy<Mutex<Option<tokio::sync::oneshot::Sender<()>>>> =
    Lazy::new(|| Mutex::new(None));

    //アプリ起動
pub fn run() {
    let app = tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![start_host, stop_host])
        .build(tauri::generate_context!())
        .expect("error while building tauri application");

    app.run(|_app, event| {
        if let tauri::RunEvent::ExitRequested { .. } = event {
            std::println!("[Rust] Exit requested, cleaning up...");
            cleanup_resources();
        }
    });
}

//ホスト開始
#[tauri::command]
async fn start_host(
    app: tauri::AppHandle,
    server_url: String,
    token: String,
    device_name: String,
) -> Result<(), String> {
    let _ = stop_host().await;

    std::println!("[Rust] Initializing native C++ engine...");
    let app_for_dialog = app.clone();//接続の許可ダイアログを出すために、とっておく
    ffi::init_engine(app)?;//C++エンジン初期化

    let displays = ffi::list_displays().unwrap_or_default();
    let mut adapter_idx = 0;//
    let mut output_idx = 0;
    let mut w = 1920;
    let mut h = 1080;
    if let Some(primary) = displays.iter().find(|d| d.is_primary == 1) {
        adapter_idx = primary.adapter_index;
        output_idx = primary.output_index;
        w = primary.width as i32;
        h = primary.height as i32;
    } else if let Some(first) = displays.first() {
        adapter_idx = first.adapter_index;
        output_idx = first.output_index;
        w = first.width as i32;
        h = first.height as i32;
    }
    if let Some(selected) = displays
        .iter()
        .find(|d| d.adapter_index == adapter_idx && d.output_index == output_idx)
    {
        signaling::select_input_display(selected);//入力ディスプレイ選択
    }

    let fps = 60;

    let (ws_tx, mut ws_rx) = tokio::sync::mpsc::unbounded_channel::<String>();
    *ffi::WS_SENDER.lock().unwrap() = Some(ws_tx);

    let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel::<()>();
    *WS_CANCEL_TX.lock().unwrap() = Some(cancel_tx);

    let instance_id = format!(
        "inst-native-{}",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis()
    );

    // 接続の許可ダイアログの答え(許可 = true)を、WebSocket のループに戻すための通り道
    let (approval_tx, mut approval_rx) = tokio::sync::mpsc::unbounded_channel::<bool>();

    let mut ctx = signaling::SessionContext {
        displays,
        adapter_idx,
        output_idx,
        w,
        h,
        fps,
        device_name,
        instance_id,
        app: app_for_dialog,
        approval_tx,
        client_present: false,
        approval_pending: false,
        approved: false,
        latest_who: String::new(),
        pending_who: String::new(),
    };

    tokio::spawn(async move {
        std::println!("[Rust WS] Connecting to signaling server: {}", server_url);

        let (ws_stream, _) = match connect_async(&server_url).await {
            Ok(val) => val,
            Err(e) => {
                std::println!("[Rust WS] Connection failed: {}", e);
                signaling::emit_status(&ctx.app, "error", "");
                return;
            }
        };
        std::println!("[Rust WS] Connected");

        //WebSocketストリームを**送信側（write）と受信側（read）**に分割
        let (mut write, mut read) = ws_stream.split();

            let auth_msg = serde_json::json!({
            "type": "auth",
            "token": token
        })
        .to_string();
        if let Err(e) = write.send(Message::Text(auth_msg)).await {
            std::println!("[Rust WS] Auth send failed: {}", e);
            return;
        }

        let mut cancel_rx = cancel_rx;

        loop {
            tokio::select! {
                _ = &mut cancel_rx => {
                    std::println!("[Rust WS] Stop signal received");
                    break;
                }

                Some(msg) = ws_rx.recv() => {
                    if let Err(e) = write.send(Message::Text(msg)).await {
                        std::println!("[Rust WS] Send failed: {}", e);
                        break;
                    }
                }

                // 接続の許可ダイアログの答え
                Some(allowed) = approval_rx.recv() => {
                    signaling::on_approval(allowed, &mut ctx, &mut write).await;
                }

                msg_res = read.next() => {
                    let msg = match msg_res {
                        Some(Ok(Message::Text(t))) => t,
                        Some(Ok(_)) => continue,
                        Some(Err(e)) => {
                            std::println!("[Rust WS] Receive error: {}", e);
                            signaling::emit_status(&ctx.app, "error", "");
                            break;
                        }
                        None => {
                            std::println!("[Rust WS] Server disconnected");
                            signaling::emit_status(&ctx.app, "error", "");
                            break;
                        }
                    };

                    if let Ok(j) = serde_json::from_str::<serde_json::Value>(&msg) {
                        if let Some(action) = signaling::handle_message(
                            &j, &mut ctx, &mut write,
                        ).await {
                            if action == signaling::Action::Stop {
                                break;
                            }
                        }
                    }
                }
            }
        }

        std::println!("[Rust WS] Closing WebSocket...");
        let _ = write.close().await;
    });

    Ok(())
}

fn cleanup_resources() {
    if let Some(cancel_tx) = WS_CANCEL_TX.lock().unwrap().take() {
        let _ = cancel_tx.send(());//Websocketループに停止信号
    }
    *ffi::WS_SENDER.lock().unwrap() = None;//送信チャネル削除

    let _ = ffi::stop_stream();//C++エンジンのストリーム停止
    ffi::shutdown_engine();//C++エンジン終了
}

#[tauri::command]
async fn stop_host() -> Result<(), String> {
    std::println!("[Rust] Stopping stream...");
    cleanup_resources();
    std::println!("[Rust] Stopped");
    Ok(())
}
