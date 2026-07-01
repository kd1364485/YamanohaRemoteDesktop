use std::os::raw::{c_char, c_int};
use std::ffi::{CString, CStr};
use std::sync::Mutex;
use once_cell::sync::Lazy;
use tauri::{AppHandle, Emitter};
use serde::Serialize;
use tokio::sync::mpsc::UnboundedSender;

// Tauri の AppHandle をグローバルに保持するための静的変数
pub static TAURI_APP: Lazy<Mutex<Option<AppHandle>>> = Lazy::new(|| Mutex::new(None));
// WebSocket 送信チャンネルのグローバル静的変数
pub static WS_SENDER: Lazy<Mutex<Option<UnboundedSender<String>>>> = Lazy::new(|| Mutex::new(None));

// FFI から通知されるデータのシリアライズ用構造体
#[derive(Clone, Serialize)]
struct SignalingPayload {
    pub content: String,
    pub r#type: String, // sdp_type もしくは mid
}

#[derive(Clone, Serialize)]
struct MessagePayload {
    pub message: String,
}

#[derive(Clone, Serialize)]
struct StatePayload {
    pub connected: bool,
}

// ---------------------------------------------------------------------------
// FFI 経由で C++ 側から呼ばれる extern "C" コールバック関数群
// ---------------------------------------------------------------------------

// 1. ローカルSDP生成完了時の通知コールバック
unsafe extern "C" fn on_local_description(sdp: *const c_char, sdp_type: *const c_char) {
    if sdp.is_null() || sdp_type.is_null() { return; }
    let sdp_str = CStr::from_ptr(sdp).to_string_lossy().into_owned();
    let type_str = CStr::from_ptr(sdp_type).to_string_lossy().into_owned();

    std::println!("[Rust FFI] Local Description (SDP) を受信: {}", type_str);
    
    // Tauriのフロントエンドへイベント通知
    if let Some(app) = TAURI_APP.lock().unwrap().as_ref() {
        let payload = SignalingPayload {
            content: sdp_str.clone(),
            r#type: type_str.clone(),
        };
        let _ = app.emit("webrtc-local-sdp", payload);
    }

    // WebSocket送信タスクへ流し込み
    if let Some(sender) = WS_SENDER.lock().unwrap().as_ref() {
        // シグナリングサーバーの仕様 (webrtc-offer) に合わせる
        let mapped_type = if type_str == "offer" { "webrtc-offer" } else { &type_str };
        let ws_msg = serde_json::json!({
            "type": mapped_type,
            "sdp": sdp_str
        }).to_string();
        let _ = sender.send(ws_msg);
    }
}

// 2. ローカルICE候補生成完了時の通知コールバック (Trickle ICE用)
unsafe extern "C" fn on_local_candidate(candidate: *const c_char, mid: *const c_char) {
    if candidate.is_null() { return; }
    let cand_str = CStr::from_ptr(candidate).to_string_lossy().into_owned();
    let mid_str = if mid.is_null() {
        "".to_string()
    } else {
        CStr::from_ptr(mid).to_string_lossy().into_owned()
    };

    // Tauriのフロントエンドへイベント通知
    if let Some(app) = TAURI_APP.lock().unwrap().as_ref() {
        let payload = SignalingPayload {
            content: cand_str.clone(),
            r#type: mid_str.clone(),
        };
        let _ = app.emit("webrtc-local-candidate", payload);
    }

    // WebSocket送信タスクへ流し込み
    if let Some(sender) = WS_SENDER.lock().unwrap().as_ref() {
        // シグナリングサーバーの仕様 (webrtc-ice) に合わせる
        let ws_msg = serde_json::json!({
            "type": "webrtc-ice",
            "candidate": cand_str,
            "mid": mid_str
        }).to_string();
        let _ = sender.send(ws_msg);
    }
}

// 3. データチャネル経由で制御コマンドメッセージを受信した時のコールバック
unsafe extern "C" fn on_remote_message(message: *const c_char) {
    if message.is_null() { return; }
    let msg_str = CStr::from_ptr(message).to_string_lossy().into_owned();

    std::println!("[Rust FFI] DataChannel 受信メッセージ: {}", msg_str);

    // 受信した JSON コマンドを解析し、フロントエンドを介さずに直接 OS に入力をシミュレートする (超低遅延)
    if let Ok(j) = serde_json::from_str::<serde_json::Value>(&msg_str) {
        let msg_type = j["type"].as_str().unwrap_or("");
        match msg_type {
            // クライアント側からの統一入力形式: {type: "input", action: "...", ...}
            "input" => {
                let action = j["action"].as_str().unwrap_or("");
                match action {
                    "mouse-move" => {
                        // クライアントは 0..1 の正規化座標を送る
                        let nx = j["nx"].as_f64().unwrap_or(0.0);
                        let ny = j["ny"].as_f64().unwrap_or(0.0);
                        let screen_width = crate::input_control::get_screen_width();
                        let screen_height = crate::input_control::get_screen_height();
                        let x = (nx * screen_width as f64) as i32;
                        let y = (ny * screen_height as f64) as i32;
                        let _ = crate::input_control::inject_mouse_move(x, y);
                    }
                    "mouse-move-rel" => {
                        let dx = j["dx"].as_i64().unwrap_or(0) as i32;
                        let dy = j["dy"].as_i64().unwrap_or(0) as i32;
                        let _ = crate::input_control::inject_mouse_move_rel(dx, dy);
                    }
                    "mouse-down" | "mouse-up" => {
                        let is_down = action == "mouse-down";
                        let button = j["button"].as_str().unwrap_or("left");
                        let _ = crate::input_control::inject_mouse_click(
                            crate::input_control::parse_button_name(button), is_down);
                    }
                    "mouse-scroll" => {
                        let delta_y = j["deltaY"].as_i64().unwrap_or(0) as i32;
                        let _ = crate::input_control::inject_mouse_scroll(delta_y);
                    }
                    "key" => {
                        let key = j["key"].as_str().unwrap_or("");
                        let is_down = j["isDown"].as_bool().unwrap_or(false);
                        if let Some(vk) = crate::input_control::key_name_to_vk(key) {
                            let _ = crate::input_control::inject_key_event(vk, is_down);
                        }
                    }
                    "type-char" => {
                        let ch = j["char"].as_str().unwrap_or("");
                        if let Some(c) = ch.chars().next() {
                            let _ = crate::input_control::inject_unicode_char(c);
                        }
                    }
                    _ => {}
                }
            }
            // 旧形式（互換性維持）
            "mousemove" => {
                let x = j["x"].as_f64().unwrap_or(0.0) as i32;
                let y = j["y"].as_f64().unwrap_or(0.0) as i32;
                let _ = crate::input_control::inject_mouse_move(x, y);
            }
            "mousedown" | "mouseup" => {
                // button: 0=左, 1=中, 2=右 (nut-jsやDOM標準に準拠)
                let button = j["button"].as_u64().unwrap_or(0) as u8;
                let is_down = msg_type == "mousedown";
                let _ = crate::input_control::inject_mouse_click(button, is_down);
            }
            "keydown" | "keyup" => {
                let key_code = j["keyCode"].as_u64().unwrap_or(0) as u32;
                let is_down = msg_type == "keydown";
                let _ = crate::input_control::inject_key_event(key_code, is_down);
            }
            _ => {}
        }
    }

    // Tauriのフロントエンドへイベント通知
    if let Some(app) = TAURI_APP.lock().unwrap().as_ref() {
        let payload = MessagePayload {
            message: msg_str,
        };
        let _ = app.emit("webrtc-remote-message", payload);
    }
}

// 4. 接続状態変更時のコールバック
unsafe extern "C" fn on_state_change(connected: c_int) {
    let is_connected = connected != 0;
    std::println!("[Rust FFI] 接続状態変更: {}", is_connected);

    // Tauriのフロントエンドへイベント通知
    if let Some(app) = TAURI_APP.lock().unwrap().as_ref() {
        let payload = StatePayload {
            connected: is_connected,
        };
        let _ = app.emit("webrtc-state-change", payload);
    }
}

// ---------------------------------------------------------------------------
// C++ DLL からのエクスポート関数バインディング
// ---------------------------------------------------------------------------
#[link(name = "yamanoha_core")]
extern "C" {
    fn yamanoha_init() -> c_int;
    fn yamanoha_shutdown();
    fn yamanoha_list_displays(out: *mut YamanohaDisplayInfo, max_count: c_int) -> c_int;
    
    fn yamanoha_set_callbacks(
        sdp_cb: Option<unsafe extern "C" fn(*const c_char, *const c_char)>,
        candidate_cb: Option<unsafe extern "C" fn(*const c_char, *const c_char)>,
        message_cb: Option<unsafe extern "C" fn(*const c_char)>,
        state_cb: Option<unsafe extern "C" fn(c_int)>
    );

    fn yamanoha_start_stream(
        adapter_index: c_int,
        output_index: c_int,
        width: c_int,
        height: c_int,
        fps: c_int,
        bitrate_kbps: c_int
    ) -> c_int;

    fn yamanoha_stop_stream() -> c_int;

    fn yamanoha_process_signaling(
        msg_type: *const c_char,
        sdp_or_candidate: *const c_char,
        mid: *const c_char
    ) -> c_int;

    fn yamanoha_send_message(message: *const c_char) -> c_int;
}

#[repr(C)]
#[derive(Debug, Clone)]
pub struct YamanohaDisplayInfo {
    pub name: [u16; 128],
    pub adapter_name: [u16; 128],
    pub adapter_index: c_int,
    pub output_index: c_int,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub refresh_rate: f32,
    pub is_primary: c_int,
}

// ---------------------------------------------------------------------------
// Rust 向け セーフティラッパー関数群
// ---------------------------------------------------------------------------

pub fn init_engine(app: AppHandle) -> Result<(), String> {
    // AppHandle をグローバルに設定
    *TAURI_APP.lock().unwrap() = Some(app);

    let res = unsafe { yamanoha_init() };
    if res != 0 {
        return Err(format!("Engine initialization failed with code: {}", res));
    }

    // コールバック関数を DLL 側にセット
    unsafe {
        yamanoha_set_callbacks(
            Some(on_local_description),
            Some(on_local_candidate),
            Some(on_remote_message),
            Some(on_state_change)
        );
    }

    Ok(())
}

pub fn shutdown_engine() {
    unsafe { yamanoha_shutdown() };
    *TAURI_APP.lock().unwrap() = None;
}

pub fn list_displays() -> Result<Vec<YamanohaDisplayInfo>, String> {
    let mut buffer = vec![
        YamanohaDisplayInfo {
            name: [0; 128],
            adapter_name: [0; 128],
            adapter_index: 0,
            output_index: 0,
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            refresh_rate: 0.0,
            is_primary: 0,
        };
        10
    ];

    let count = unsafe { yamanoha_list_displays(buffer.as_mut_ptr(), buffer.len() as c_int) };
    if count < 0 {
        return Err(format!("Failed to list displays: {}", count));
    }

    buffer.truncate(count as usize);
    Ok(buffer)
}

pub fn start_stream(
    adapter_index: i32,
    output_index: i32,
    width: i32,
    height: i32,
    fps: i32,
    bitrate_kbps: i32
) -> Result<(), String> {
    let res = unsafe {
        yamanoha_start_stream(
            adapter_index as c_int,
            output_index as c_int,
            width as c_int,
            height as c_int,
            fps as c_int,
            bitrate_kbps as c_int
        )
    };
    if res != 0 {
        return Err(format!("Failed to start stream: {}", res));
    }
    Ok(())
}

pub fn stop_stream() -> Result<(), String> {
    let res = unsafe { yamanoha_stop_stream() };
    if res != 0 {
        return Err(format!("Failed to stop stream: {}", res));
    }
    Ok(())
}

pub fn process_signaling(
    msg_type: &str,
    sdp_or_candidate: &str,
    mid: Option<&str>
) -> Result<(), String> {
    let c_msg_type = CString::new(msg_type).map_err(|e| e.to_string())?;
    let c_sdp_or_candidate = CString::new(sdp_or_candidate).map_err(|e| e.to_string())?;
    
    let c_mid = match mid {
        Some(m) => Some(CString::new(m).map_err(|e| e.to_string())?),
        None => None,
    };

    let c_mid_ptr = match &c_mid {
        Some(m) => m.as_ptr(),
        None => std::ptr::null(),
    };

    let res = unsafe {
        yamanoha_process_signaling(c_msg_type.as_ptr(), c_sdp_or_candidate.as_ptr(), c_mid_ptr)
    };

    if res == 0 {
        return Err("Failed to process signaling message in native engine".to_string());
    }
    Ok(())
}

pub fn send_message(message: &str) -> Result<(), String> {
    let c_message = CString::new(message).map_err(|e| e.to_string())?;
    let res = unsafe { yamanoha_send_message(c_message.as_ptr()) };
    if res == 0 {
        return Err("Failed to send message over data channel".to_string());
    }
    Ok(())
}
