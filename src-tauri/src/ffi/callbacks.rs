use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use tauri::{AppHandle, Emitter};
use serde::Serialize;
use tokio::sync::mpsc::UnboundedSender;
use std::sync::Mutex;
use once_cell::sync::Lazy;

//Tauriアプリハンドル
pub static TAURI_APP: Lazy<Mutex<Option<AppHandle>>> = Lazy::new(|| Mutex::new(None));
//WebSocket送信チャネル
pub static WS_SENDER: Lazy<Mutex<Option<UnboundedSender<String>>>> = Lazy::new(|| Mutex::new(None));

#[derive(Clone, Serialize)]
struct SignalingPayload {
    pub content: String,
    pub r#type: String,
}

#[derive(Clone, Serialize)]
struct StatePayload {
    pub connected: bool,
}

//C++エンジンがSDP Offerを生成した時に呼ばれる
pub unsafe extern "C" fn on_local_description(sdp: *const c_char, sdp_type: *const c_char) {
    if sdp.is_null() || sdp_type.is_null() {
        return;
    }
    //なぜ .into_owned() が必要か: C++から渡されたポインタは関数が終わると無効になる可能性がある。
    // Rust側で文字列を長期間保持するには所有権を持つ String に変換する必要がある。
    let sdp_str = CStr::from_ptr(sdp).to_string_lossy().into_owned();
    let type_str = CStr::from_ptr(sdp_type).to_string_lossy().into_owned();
    //どちらのSDPが生成されたかをコンソールに出力
    std::println!("[Rust FFI] Local Description (SDP): {}", type_str);

    if let Some(app) = TAURI_APP.lock().unwrap().as_ref() {
        let _ = app.emit(
            "webrtc-local-sdp",
            SignalingPayload {
                content: sdp_str.clone(),
                r#type: type_str.clone(),
            },
        );
    }

    if let Some(sender) = WS_SENDER.lock().unwrap().as_ref() {
        let mapped_type = if type_str == "offer" {
            "webrtc-offer"
        } else {
            &type_str
        };
        let ws_msg = serde_json::json!({
            "type": mapped_type,
            "sdp": sdp_str
        })
        .to_string();
        let _ = sender.send(ws_msg);
    }
}

pub unsafe extern "C" fn on_local_candidate(candidate: *const c_char, mid: *const c_char) {
    if candidate.is_null() {
        return;
    }
    let cand_str = CStr::from_ptr(candidate).to_string_lossy().into_owned();
    let mid_str = if mid.is_null() {
        String::new()
    } else {
        CStr::from_ptr(mid).to_string_lossy().into_owned()
    };

    //Tauriイベント送信
    if let Some(app) = TAURI_APP.lock().unwrap().as_ref() {
        let _ = app.emit(
            "webrtc-local-candidate",
            SignalingPayload {
                content: cand_str.clone(),
                r#type: mid_str.clone(),
            },
        );
    }

    if let Some(sender) = WS_SENDER.lock().unwrap().as_ref() {
        let ws_msg = serde_json::json!({
            "type": "webrtc-ice",
            "candidate": cand_str,
            "mid": mid_str
        })
        .to_string();
        let _ = sender.send(ws_msg);
    }
}

pub unsafe extern "C" fn on_remote_message(message: *const c_char) {
    if message.is_null() {
        return;
    }
    let msg_str = CStr::from_ptr(message).to_string_lossy().into_owned();
    super::handle_remote_input(&msg_str);
}

pub unsafe extern "C" fn on_state_change(connected: c_int) {
    let is_connected = connected != 0;
    std::println!("[Rust FFI] Connection state: {}", is_connected);

    if let Some(app) = TAURI_APP.lock().unwrap().as_ref() {
        let _ = app.emit(
            "webrtc-state-change",
            StatePayload { connected: is_connected },
        );
    }
}
