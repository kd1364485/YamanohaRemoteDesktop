use crate::ffi;
use futures_util::SinkExt;
use tokio_tungstenite::tungstenite::protocol::Message;
use tokio_tungstenite::WebSocketStream;

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
}

pub fn display_id(display: &ffi::YamanohaDisplayInfo) -> String {
    format!("{}:{}", display.adapter_index, display.output_index)
}

fn wide_string(value: &[u16]) -> String {
    let length = value.iter().position(|&ch| ch == 0).unwrap_or(value.len());
    String::from_utf16_lossy(&value[..length])
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
        }

        "client-connected" => {
            std::println!("[Rust WS] Client connected, starting stream...");
            let current_id = format!("{}:{}", ctx.adapter_idx, ctx.output_idx);
            let _ = write
                .send(Message::Text(display_list_message(&ctx.displays, &current_id)))
                .await;
            std::println!(
                "[Rust] Stream start: Adapter={}, Output={}, {}x{}, FPS={}",
                ctx.adapter_idx, ctx.output_idx, ctx.w, ctx.h, ctx.fps
            );
            if let Err(e) = ffi::start_stream(
                ctx.adapter_idx,
                ctx.output_idx,
                ctx.w,
                ctx.h,
                ctx.fps,
                4000,
            ) {
                std::println!("[Rust WS] Stream start error: {}", e);
            }
        }

        "select-display" => {
            handle_select_display(j, ctx, write).await;
        }

        "webrtc-answer" => {
            let sdp = j["sdp"].as_str().unwrap_or("");
            std::println!("[Rust WS] Answer SDP received");
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
            std::println!("[Rust WS] Remote ICE: {}", candidate_str);
            let _ = ffi::process_signaling("candidate", candidate_str, mid_str);
        }

        "input" => {
            let msg_str = j.to_string();
            ffi::handle_remote_input(&msg_str);
        }

        _ => {}
    }

    None
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
        return;
    };

    if selected.adapter_index == ctx.adapter_idx
        && selected.output_index == ctx.output_idx
    {
        return;
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
        4000,
    ) {
        std::println!("[Rust WS] Display switch failed: {}", e);
    }
}
