use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use once_cell::sync::Lazy;

static HOST_PROCESS: Lazy<Mutex<Option<Child>>> = Lazy::new(|| Mutex::new(None));

mod screen_capture;
mod input_control;
mod network;
mod session;

#[tauri::command]
fn start_host(server_url: String, code: String) -> Result<(), String> {
    let exe_dir = std::env::current_exe()
        .map_err(|e| e.to_string())?
        .parent()
        .ok_or("Cannot get parent dir")?
        .to_path_buf();

    let host_script = exe_dir
        .join("..")
        .join("..")
        .join("..")
        .join("..")
        .join("host")
        .join("host.js");

    let host_script = if host_script.exists() {
        host_script
    } else {
        let fallback = std::env::current_dir()
            .map_err(|e| e.to_string())?
            .join("..")
            .join("host")
            .join("host.js");
        if !fallback.exists() {
            return Err(format!("Host script not found"));
        }
        fallback
    };

    let child = Command::new("node")
        .arg(&host_script)
        .env("SIGNALING_URL", &server_url)
        .env("SIGNALING_CODE", &code)
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .spawn()
        .map_err(|e| format!("Failed to start host process: {}", e))?;

    *HOST_PROCESS.lock().unwrap() = Some(child);
    Ok(())
}

#[tauri::command]
fn stop_host() -> Result<(), String> {
    let mut proc = HOST_PROCESS.lock().unwrap();
    if let Some(mut child) = proc.take() {
        child.kill().map_err(|e| format!("Failed to kill host: {}", e))?;
    }
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
