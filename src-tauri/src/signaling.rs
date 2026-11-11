use crate::ffi;//C++エンジンのFFI呼び出し
use futures_util::SinkExt;//WebSocket送信
use tokio_tungstenite::tungstenite::protocol::Message;//WebSocketメッセージ
use tokio_tungstenite::WebSocketStream;//WebSocketストリーム
use tauri_plugin_dialog::{DialogExt, MessageDialogButtons, MessageDialogKind};//接続の許可ダイアログ

/// ストリーム配信時のデフォルトビットレート（kbps）
const DEFAULT_BITRATE_KBPS: i32 = 4000;

/// シグナリングサーバーとの WebSocket の送信側
type WsWrite = futures_util::stream::SplitSink<
    WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>,
    Message,
>;

#[derive(PartialEq)]
pub enum Action {
    Stop,
}

pub struct SessionContext {
    pub displays: Vec<ffi::YamanohaDisplayInfo>,
    pub adapter_idx: i32,
    pub output_idx: i32,
    pub w: i32,
    pub h: i32,
    pub fps: i32,
    pub device_name: String,
    pub instance_id: String,

    // --- 接続の許可 ---
    // クライアントが接続しても、この PC の持ち主が「許可」を押すまでは、
    // 画面の配信も、マウス・キーボードの入力の受け付けも、始めない(approved が true になるまで)。
    pub app: tauri::AppHandle,                                  // ダイアログを出すために必要
    pub approval_tx: tokio::sync::mpsc::UnboundedSender<bool>,  // ダイアログの答え(許可 = true)を、メインのループに戻す
    pub client_present: bool,     // 今、クライアントが接続しているか
    pub approval_pending: bool,   // ダイアログを出して、答えを待っているか
    pub approved: bool,           // 今のクライアントを、許可したか
    pub latest_who: String,       // 今、接続しているクライアントの「アカウント名 / IP」
    pub pending_who: String,      // ダイアログに表示した、クライアントの「アカウント名 / IP」
}

/// 接続の許可を求めるダイアログを出す(答えは approval_tx で、メインのループに戻る)
fn ask_permission(ctx: &mut SessionContext, user: &str, ip: &str) {
    ctx.approval_pending = true;
    ctx.pending_who = ctx.latest_who.clone();
    std::println!("[Rust WS] Client wants to connect (user={}, ip={}); asking for permission", user, ip);

    let tx = ctx.approval_tx.clone();
    ctx.app
        .dialog()
        .message(format!(
            "リモートからの接続要求です。\n\nアカウント: {}\n接続元 IP: {}\n\n\
             許可すると、この PC の画面が表示され、マウスとキーボードを操作されます。\n\
             心当たりがなければ「拒否する」を選んでください。",
            user, ip
        ))
        .title("Yamanoha Remote Desktop - 接続の許可")
        .kind(MessageDialogKind::Warning)
        .buttons(MessageDialogButtons::OkCancelCustom(
            "許可する".to_string(),
            "拒否する".to_string(),
        ))
        .show(move |allowed| {
            let _ = tx.send(allowed);//答えを、メインのループに戻す
        });
}

/// ホストの画面に、今の状態を知らせる(main.ts の "host-status" で受け取る)
/// state: "waiting"(接続待ち) / "asking"(許可ダイアログ表示中) / "streaming"(配信中) / "error"
/// 録画や画面共有に映ることがあるので、IP は渡さず、アカウント名だけを渡す。
pub fn emit_status(app: &tauri::AppHandle, state: &str, user: &str) {
    use tauri::Emitter;
    let _ = app.emit("host-status", serde_json::json!({ "state": state, "user": user }));
}

pub fn display_id(display: &ffi::YamanohaDisplayInfo) -> String {
    format!("{}:{}", display.adapter_index, display.output_index)
}

fn wide_string(value: &[u16]) -> String {
    let length = value.iter().position(|&ch| ch == 0).unwrap_or(value.len());
    String::from_utf16_lossy(&value[..length])//UTF-16文字列をUTF-8文字列に変換
}

fn display_list_message(
    displays: &[ffi::YamanohaDisplayInfo],
    current_id: &str,
) -> String {
    let items: Vec<_> = displays
        .iter()
        .map(|display| {
            serde_json::json!({
                "id": display_id(display),
                "label": wide_string(&display.name),
                "width": display.width,
                "height": display.height,
                "primary": display.is_primary == 1,
            })
        })
        .collect();

    serde_json::json!({
        "type": "display-list",
        "displays": items,
        "currentId": current_id,
    })
    .to_string()
}

pub fn select_input_display(display: &ffi::YamanohaDisplayInfo) {
    crate::input_control::set_active_display(
        display.x,
        display.y,
        display.width,
        display.height,
    );
}

pub async fn handle_message(
    j: &serde_json::Value,
    ctx: &mut SessionContext,
    write: &mut futures_util::stream::SplitSink<
        WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>,
        Message,
    >,
) -> Option<Action> {
    let msg_type = j["type"].as_str().unwrap_or("");

    match msg_type {
        "auth-ok" => {
            std::println!("[Rust WS] Auth OK, registering host...");
            let host_msg = serde_json::json!({
                "type": "host",
                "deviceName": ctx.device_name,
                "instanceId": ctx.instance_id,
            })
            .to_string();
            let _ = write.send(Message::Text(host_msg)).await;
        }

        "host-ready" => {
            let host_id = j["hostId"].as_str().unwrap_or("");
            std::println!("[Rust WS] Host ready, ID: {}", host_id);
            emit_status(&ctx.app, "waiting", "");
        }

        "client-connected" => {
            // 新しいクライアントが来たら、前の許可は無効にして、改めて許可を求める
            ctx.client_present = true;
            ctx.approved = false;

            let user = j["username"].as_str().unwrap_or("(不明)").to_string();
            let ip = j["clientIp"].as_str().unwrap_or("(不明)").to_string();
            ctx.latest_who = format!("{} / {}", user, ip);

            // すでにダイアログを出しているときは、その答えを待つ。
            // (答えが来たとき、表示した相手と、今の相手が違えば、改めて聞き直す。on_approval を参照)
            if !ctx.approval_pending {
                emit_status(&ctx.app, "asking", &user);
                ask_permission(ctx, &user, &ip);
            }
        }

        // クライアントが切断した。次のクライアントには、改めて許可を求める
        "disconnected" => {
            std::println!("[Rust WS] Client disconnected");
            ctx.client_present = false;
            ctx.approved = false;
            emit_status(&ctx.app, "waiting", "");
        }

        "select-display" => {
            if !ctx.approved {
                return None;//許可されるまで、クライアントの操作は受け付けない
            }
            handle_select_display(j, ctx, write).await;
        }

        //クライアントから返ってきたAnswer SDPをC++エンジン（libdatachannel）に渡す
        "webrtc-answer" => {
            if !ctx.approved {
                return None;
            }
            let sdp = j["sdp"].as_str().unwrap_or("");
            std::println!("[Rust WS] Answer SDP received");
            let _ = ffi::process_signaling("answer", sdp, None);
        }

        //クライアントからのICE候補をC++エンジンに渡す
        "webrtc-ice" => {
            if !ctx.approved {
                return None;
            }
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
            std::println!("[Rust WS] Remote ICE: {}", candidate_str);
            let _ = ffi::process_signaling("candidate", candidate_str, mid_str);
        }

        //クライアントからの入力JSONをC++エンジンに渡す
        "input" => {
            if !ctx.approved {
                return None;//許可されるまで、マウス・キーボードの操作は、受け付けない
            }
            let msg_str = j.to_string();
            ffi::handle_remote_input(&msg_str);
        }

        _ => {}
    }

    None
}

/// 許可ダイアログの答えを受け取ったときの処理
/// 許可: 画面の配信を始める / 拒否: サーバーに、クライアントを切るよう伝える
pub async fn on_approval(allowed: bool, ctx: &mut SessionContext, write: &mut WsWrite) {
    ctx.approval_pending = false;

    // ダイアログが開いている間に、クライアントが切断していた。何もしない(配信も始めない)
    if !ctx.client_present {
        std::println!("[Rust WS] The client already left; ignoring the answer");
        ctx.approved = false;
        emit_status(&ctx.app, "waiting", "");
        return;
    }

    // ダイアログに表示した相手と、今接続している相手が、違う(表示のあとで、別のクライアントに替わった)。
    // この答えは、今の相手への許可ではないので、使わずに、今の相手で、聞き直す。
    if ctx.pending_who != ctx.latest_who {
        std::println!("[Rust WS] The client changed while the dialog was open; asking again");
        ctx.approved = false;
        let who = ctx.latest_who.clone();
        let (user, ip) = who.split_once(" / ").unwrap_or((who.as_str(), ""));
        ask_permission(ctx, user, ip);
        return;
    }

    if !allowed {
        std::println!("[Rust WS] Rejected by the user");
        ctx.approved = false;
        ctx.client_present = false;//サーバーが、このクライアントを切る
        emit_status(&ctx.app, "waiting", "");
        let _ = write
            .send(Message::Text(
                serde_json::json!({ "type": "reject-client" }).to_string(),
            ))
            .await;
        return;
    }

    std::println!("[Rust WS] Allowed by the user, starting stream...");
    ctx.approved = true;
    let current_id = format!("{}:{}", ctx.adapter_idx, ctx.output_idx);
    let _ = write
        .send(Message::Text(display_list_message(&ctx.displays, &current_id)))
        .await;//ディスプレイ一覧をクライアントに送信
    std::println!(
        "[Rust] Stream start: Adapter={}, Output={}, {}x{}, FPS={}",
        ctx.adapter_idx, ctx.output_idx, ctx.w, ctx.h, ctx.fps
    );
    // 許可されたら、C++エンジンを起動して画面キャプチャを開始
    if let Err(e) = ffi::start_stream(
        ctx.adapter_idx,
        ctx.output_idx,
        ctx.w,
        ctx.h,
        ctx.fps,
        DEFAULT_BITRATE_KBPS,
    ) {
        std::println!("[Rust WS] Stream start error: {}", e);
        emit_status(&ctx.app, "error", "");
        return;
    }
    let user = ctx.latest_who.split(" / ").next().unwrap_or("").to_string();
    emit_status(&ctx.app, "streaming", &user);
}

async fn handle_select_display(
    j: &serde_json::Value,
    ctx: &mut SessionContext,
    write: &mut futures_util::stream::SplitSink<
        WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>,
        Message,
    >,
) {
    let requested_id = j["displayId"].as_str().unwrap_or("");
    let Some(selected) = ctx
        .displays
        .iter()
        .find(|d| display_id(d) == requested_id)
    else {
        std::println!("[Rust WS] Unknown display: {}", requested_id);
        return;//未知のディスプレイIDは無視
    };

    if selected.adapter_index == ctx.adapter_idx
        && selected.output_index == ctx.output_idx
    {
        return;//同じディスプレイを選択した場合は何もしない
    }

    std::println!(
        "[Rust WS] Display switch: {} ({}x{})",
        requested_id,
        selected.width,
        selected.height
    );

    ctx.adapter_idx = selected.adapter_index;
    ctx.output_idx = selected.output_index;
    ctx.w = selected.width as i32;
    ctx.h = selected.height as i32;
    select_input_display(selected);

    let current_id = display_id(selected);
    let _ = write
        .send(Message::Text(display_list_message(&ctx.displays, &current_id)))
        .await;

    if let Err(e) = ffi::switch_display(
        ctx.adapter_idx,
        ctx.output_idx,
        ctx.w,
        ctx.h,
        ctx.fps,
        DEFAULT_BITRATE_KBPS,
    ) {
        std::println!("[Rust WS] Display switch failed: {}", e);
    }
}
