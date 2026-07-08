// ============================================================================
// input_control.rs
// Windows API (windows-sys) を用いた OS レベルの入力シミュレーション (インジェクション)
// P2Pデータチャネルから受信した操作コマンドを超低遅延で注入します。
// ============================================================================

use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    SendInput, INPUT, INPUT_MOUSE, INPUT_KEYBOARD,
    MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
    MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP,
    MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP,
    KEYEVENTF_KEYUP, KEYEVENTF_UNICODE, MOUSEEVENTF_ABSOLUTE, MOUSEEVENTF_MOVE,
    MOUSEEVENTF_WHEEL, MOUSEEVENTF_VIRTUALDESK
};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    GetSystemMetrics, SM_CXSCREEN, SM_CYSCREEN,
    SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN
};
use once_cell::sync::Lazy;
use std::sync::Mutex;

#[derive(Clone, Copy)]
struct DisplayBounds {
    x: i32,
    y: i32,
    width: i32,
    height: i32,
}

static ACTIVE_DISPLAY: Lazy<Mutex<Option<DisplayBounds>>> =
    Lazy::new(|| Mutex::new(None));

pub fn set_active_display(x: i32, y: i32, width: u32, height: u32) {
    *ACTIVE_DISPLAY.lock().unwrap() = Some(DisplayBounds {
        x,
        y,
        width: width as i32,
        height: height as i32,
    });
}
//main.ts からの正規化座標（0〜1）を実際のピクセル座標に変換
//bounds.x / bounds.y を足すことで、マルチディスプレイ環境で正しいディスプレイにカーソルを配置
fn active_display_position(nx: f64, ny: f64) -> (i32, i32) {
    match *ACTIVE_DISPLAY.lock().unwrap() {
        Some(bounds) => (
            bounds.x + (nx.clamp(0.0, 1.0) * (bounds.width - 1).max(0) as f64).round() as i32,
            bounds.y + (ny.clamp(0.0, 1.0) * (bounds.height - 1).max(0) as f64).round() as i32,
        ),
        None => (
            (nx.clamp(0.0, 1.0) * (get_screen_width() - 1).max(0) as f64).round() as i32,
            (ny.clamp(0.0, 1.0) * (get_screen_height() - 1).max(0) as f64).round() as i32,
        ),
    }
}

// GetCursorPos / SetCursorPos は windows-sys 0.52 の WindowsAndMessaging には無いため、raw FFI で直接呼び出す
#[repr(C)]
#[derive(Clone, Copy)]
struct Point {
    x: i32,
    y: i32,
}

#[link(name = "user32")]
extern "system" {
    fn GetCursorPos(lpPoint: *mut Point) -> i32;
    fn SetCursorPos(x: i32, y: i32) -> i32;
}

/// @brief プライマリディスプレイの幅を取得する
pub fn get_screen_width() -> i32 {
    unsafe { GetSystemMetrics(SM_CXSCREEN) }
}

/// @brief プライマリディスプレイの高さを取得する
pub fn get_screen_height() -> i32 {
    unsafe { GetSystemMetrics(SM_CYSCREEN) }
}

/// @brief マウスカーソルを物理座標 (x, y) へ移動する
pub fn inject_mouse_move(x: i32, y: i32) -> Result<(), String> {
    unsafe {
        // デスクトップのプライマリ解像度を取得
        let virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        let virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        let screen_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        let screen_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        if screen_width == 0 || screen_height == 0 {
            return Err("Failed to get screen metrics".to_string());
        }

        // Windowsの絶対座標系 (0〜65535) へノーマライズ
        // Mickey 座標への変換式: (pos * 65536) / screen_size
        let normalized_x =
            (((x - virtual_x) as i64 * 65535) / (screen_width - 1).max(1) as i64) as i32;
        let normalized_y =
            (((y - virtual_y) as i64 * 65535) / (screen_height - 1).max(1) as i64) as i32;

        let mut input: INPUT = std::mem::zeroed();
        input.r#type = INPUT_MOUSE;
        
        // MOUSEINPUT 構造体の設定
        let mouse_input = &mut input.Anonymous.mi;
        mouse_input.dx = normalized_x;
        mouse_input.dy = normalized_y;
        mouse_input.mouseData = 0;
        mouse_input.dwFlags =
            MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        mouse_input.time = 0;
        mouse_input.dwExtraInfo = 0;

        let sent = SendInput(1, &input, std::mem::size_of::<INPUT>() as i32);
        if sent != 1 {
            return Err("SendInput mouse move failed".to_string());
        }
    }
    Ok(())
}

/// @brief マウスクリックおよびドラッグイベントを注入する
/// @param button: 0=左, 1=中, 2=右
/// @param is_down: true=押し込み, false=離す
pub fn inject_mouse_click(button: u8, is_down: bool) -> Result<(), String> {
    unsafe {
        let mut input: INPUT = std::mem::zeroed();
        input.r#type = INPUT_MOUSE;

        let mouse_input = &mut input.Anonymous.mi;
        mouse_input.dx = 0;
        mouse_input.dy = 0;
        mouse_input.mouseData = 0;
        mouse_input.time = 0;
        mouse_input.dwExtraInfo = 0;

        // ボタンの押し下げ・解放フラグの決定
        mouse_input.dwFlags = match button {
            0 => { // 左ボタン
                if is_down { MOUSEEVENTF_LEFTDOWN } else { MOUSEEVENTF_LEFTUP }
            }
            2 => { // 右ボタン
                if is_down { MOUSEEVENTF_RIGHTDOWN } else { MOUSEEVENTF_RIGHTUP }
            }
            1 => { // 中ボタン
                if is_down { MOUSEEVENTF_MIDDLEDOWN } else { MOUSEEVENTF_MIDDLEUP }
            }
            _ => return Err(format!("Unsupported mouse button: {}", button)),
        };

        let sent = SendInput(1, &input, std::mem::size_of::<INPUT>() as i32);
        if sent != 1 {
            return Err("SendInput mouse click failed".to_string());
        }
    }
    Ok(())
}

/// @brief キーボード入力を注入する
/// @param vk_code: Windows仮想キーコード (key_name_to_vk で変換済み)
/// @param is_down: true=押し込み, false=離す
pub fn inject_key_event(vk_code: u32, is_down: bool) -> Result<(), String> {
    unsafe {
        let mut input: INPUT = std::mem::zeroed();
        input.r#type = INPUT_KEYBOARD;

        let key_input = &mut input.Anonymous.ki;
        key_input.wVk = vk_code as u16;
        key_input.wScan = 0;
        key_input.time = 0;
        key_input.dwExtraInfo = 0;
        
        // キー解放フラグの設定
        key_input.dwFlags = if is_down { 0 } else { KEYEVENTF_KEYUP };

        let sent = SendInput(1, &input, std::mem::size_of::<INPUT>() as i32);
        if sent != 1 {
            return Err("SendInput keyboard event failed".to_string());
        }
    }
    Ok(())
}

/// @brief マウスカーソルを相対移動 (dx, dy) させる
pub fn inject_mouse_move_rel(dx: i32, dy: i32) -> Result<(), String> {
    unsafe {
        let mut pt: Point = std::mem::zeroed();
        if GetCursorPos(&mut pt) == 0 {
            return Err("Failed to get cursor position".to_string());
        }
        let new_x = pt.x + dx;
        let new_y = pt.y + dy;
        if SetCursorPos(new_x, new_y) == 0 {
            return Err("Failed to set cursor position".to_string());
        }
    }
    Ok(())
}

/// @brief マウススクロールを注入する
pub fn inject_mouse_scroll(delta_y: i32) -> Result<(), String> {
    unsafe {
        let mut input: INPUT = std::mem::zeroed();
        input.r#type = INPUT_MOUSE;
        let mouse_input = &mut input.Anonymous.mi;
        mouse_input.dx = 0;
        mouse_input.dy = 0;
        mouse_input.mouseData = delta_y as u32;
        mouse_input.dwFlags = MOUSEEVENTF_WHEEL;
        mouse_input.time = 0;
        mouse_input.dwExtraInfo = 0;
        let sent = SendInput(1, &input, std::mem::size_of::<INPUT>() as i32);
        if sent != 1 {
            return Err("SendInput mouse scroll failed".to_string());
        }
    }
    Ok(())
}

/// @brief ボタン名 ("left"/"middle"/"right") をホスト側の数値 (0/1/2) に変換
pub fn parse_button_name(button: &str) -> u8 {
    match button {
        "left" => 0,
        "middle" => 1,
        "right" => 2,
        _ => 0,
    }
}

/// @brief キー名 (JS の e.key) から Windows の仮想キーコードを取得する
pub fn key_name_to_vk(key: &str) -> Option<u32> {
    match key {
        "Backspace" => Some(0x08),
        "Tab" => Some(0x09),
        "Enter" => Some(0x0D),
        "Shift" => Some(0x10),
        "Control" => Some(0x11),
        "Alt" => Some(0x12),
        "Pause" => Some(0x13),
        "CapsLock" => Some(0x14),
        "Escape" => Some(0x1B),
        " " | "Space" => Some(0x20),
        "PageUp" => Some(0x21),
        "PageDown" => Some(0x22),
        "End" => Some(0x23),
        "Home" => Some(0x24),
        "ArrowLeft" => Some(0x25),
        "ArrowUp" => Some(0x26),
        "ArrowRight" => Some(0x27),
        "ArrowDown" => Some(0x28),
        "PrintScreen" => Some(0x2C),
        "Insert" => Some(0x2D),
        "Delete" => Some(0x2E),
        "0" => Some(0x30),
        "1" => Some(0x31),
        "2" => Some(0x32),
        "3" => Some(0x33),
        "4" => Some(0x34),
        "5" => Some(0x35),
        "6" => Some(0x36),
        "7" => Some(0x37),
        "8" => Some(0x38),
        "9" => Some(0x39),
        "a" => Some(0x41),
        "b" => Some(0x42),
        "c" => Some(0x43),
        "d" => Some(0x44),
        "e" => Some(0x45),
        "f" => Some(0x46),
        "g" => Some(0x47),
        "h" => Some(0x48),
        "i" => Some(0x49),
        "j" => Some(0x4A),
        "k" => Some(0x4B),
        "l" => Some(0x4C),
        "m" => Some(0x4D),
        "n" => Some(0x4E),
        "o" => Some(0x4F),
        "p" => Some(0x50),
        "q" => Some(0x51),
        "r" => Some(0x52),
        "s" => Some(0x53),
        "t" => Some(0x54),
        "u" => Some(0x55),
        "v" => Some(0x56),
        "w" => Some(0x57),
        "x" => Some(0x58),
        "y" => Some(0x59),
        "z" => Some(0x5A),
        "F1" => Some(0x70),
        "F2" => Some(0x71),
        "F3" => Some(0x72),
        "F4" => Some(0x73),
        "F5" => Some(0x74),
        "F6" => Some(0x75),
        "F7" => Some(0x76),
        "F8" => Some(0x77),
        "F9" => Some(0x78),
        "F10" => Some(0x79),
        "F11" => Some(0x7A),
        "F12" => Some(0x7B),
        "Meta" => Some(0x5B),
        "ContextMenu" => Some(0x5D),
        ";" => Some(0xBA),
        "=" => Some(0xBB),
        "," => Some(0xBC),
        "-" => Some(0xBD),
        "." => Some(0xBE),
        "/" => Some(0xBF),
        "`" => Some(0xC0),
        "[" => Some(0xDB),
        "\\" => Some(0xDC),
        "]" => Some(0xDD),
        "'" => Some(0xDE),
        _ => None,
    }
}

/// @brief リモートからの入力メッセージをパースして対応する入力イベントを注入する
pub fn handle_remote_input(message: &str) {
    let json = match serde_json::from_str::<serde_json::Value>(message) {
        Ok(value) => value,
        Err(error) => {
            std::println!("[Rust Input] JSON parse failed: {}", error);
            return;//
        }
    };
    if json["type"].as_str() != Some("input") {
        return;
    }

    let action = json["action"].as_str().unwrap_or("");
    let result = match action {
        "mouse-move" => {
            let nx = json["nx"].as_f64().unwrap_or(0.0);
            let ny = json["ny"].as_f64().unwrap_or(0.0);
            let (x, y) = active_display_position(nx, ny);
            inject_mouse_move(x, y)
        }
        "mouse-move-rel" => inject_mouse_move_rel(
            json["dx"].as_i64().unwrap_or(0) as i32,
            json["dy"].as_i64().unwrap_or(0) as i32,
        ),
        "mouse-down" | "mouse-up" => {
            if action == "mouse-down" {
                if let (Some(nx), Some(ny)) =
                    (json["nx"].as_f64(), json["ny"].as_f64())
                {
                    let (x, y) = active_display_position(nx, ny);
                    inject_mouse_move(x, y).and_then(|_| inject_mouse_click(
                        parse_button_name(json["button"].as_str().unwrap_or("left")),
                        true,
                    ))
                } else {
                    inject_mouse_click(
                        parse_button_name(json["button"].as_str().unwrap_or("left")),
                        true,
                    )
                }
            } else {
                inject_mouse_click(
                    parse_button_name(json["button"].as_str().unwrap_or("left")),
                    false,
                )
            }
        },
        "mouse-click" => inject_click(
            json["button"].as_str().unwrap_or("left"), 1),
        "mouse-double-click" => inject_click(
            json["button"].as_str().unwrap_or("left"), 2),
        "mouse-scroll" => inject_mouse_scroll(
            json["deltaY"].as_f64().unwrap_or(0.0).round() as i32),
        "key" => {
            let key = json["key"].as_str().unwrap_or("");
            match key_name_to_vk(key) {
                Some(vk) => inject_key_event(
                    vk, json["isDown"].as_bool().unwrap_or(false)),
                None => Err(format!("Unsupported key: {}", key)),
            }
        }
        "type-char" => {
            match json["char"].as_str().and_then(|value| value.chars().next()) {
                Some(character) => inject_unicode_char(character),
                None => Err("Missing character".to_string()),
            }
        }
        _ => Err(format!("Unsupported action: {}", action)),
    };

    if let Err(error) = result {
        std::println!("[Rust Input] {} failed: {}", action, error);
        return;
    }

    // マウス移動は頻繁に発生するため、ログ出力を省く
    if action != "mouse-move" && action != "mouse-move-rel" {
        std::println!("[Rust Input] action applied: {}", action);
    }
}

fn inject_click(button_name: &str, count: usize) -> Result<(), String> {
    let button = parse_button_name(button_name);
    for _ in 0..count {
        inject_mouse_click(button, true)?;
        inject_mouse_click(button, false)?;
    }
    Ok(())
}

/// @brief Unicode 文字を直接入力する（印字可能文字用）
pub fn inject_unicode_char(ch: char) -> Result<(), String> {
    unsafe {
        let mut input: INPUT = std::mem::zeroed();
        input.r#type = INPUT_KEYBOARD;
        input.Anonymous.ki.wVk = 0;
        input.Anonymous.ki.wScan = ch as u16;
        input.Anonymous.ki.dwFlags = KEYEVENTF_UNICODE;
        input.Anonymous.ki.time = 0;
        input.Anonymous.ki.dwExtraInfo = 0;
        let sent = SendInput(1, &input, std::mem::size_of::<INPUT>() as i32);
        if sent != 1 {
            return Err("SendInput unicode down failed".to_string());
        }
        input.Anonymous.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        let sent = SendInput(1, &input, std::mem::size_of::<INPUT>() as i32);
        if sent != 1 {
            return Err("SendInput unicode up failed".to_string());
        }
    }
    Ok(())
}
