mod screen_capture;
mod input_control;
mod network;
mod session;

use serde::Serialize;

#[derive(Serialize)]
struct SessionInfo {
    session_id: String,
    status: String,
}

#[tauri::command]
fn get_session_info() -> SessionInfo {
    SessionInfo {
        session_id: String::new(),
        status: "idle".to_string(),
    }
}

#[tauri::command]
fn start_host() -> Result<String, String> {
    session::start_host_session()
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
            get_session_info,
            start_host,
            connect_to_host,
            disconnect_session,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
