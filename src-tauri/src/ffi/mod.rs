mod bindings;//C++DLLの関数宣言
mod callbacks;//C++　→Rustへのコールバック
mod wrappers;//FFI呼び出しのRustラッパー

use std::os::raw::c_int;
//#[repr(C)] — C言語と同じメモリレイアウトを保証。C++側の構造体と一致させる必要がある
#[repr(C)]
#[derive(Debug, Clone)]
pub struct YamanohaDisplayInfo {
    pub name: [u16; 128],//ディスプレイ名
    pub adapter_name: [u16; 128],//GPUアダプター名
    pub adapter_index: c_int,//アダプターインデックス
    pub output_index: c_int,//出力インデックス
    pub x: i32,//X座標
    pub y: i32,//Y座標
    pub width: u32,//幅
    pub height: u32,//高さ
    pub refresh_rate: f32,//リフレッシュレート
    pub is_primary: c_int,//プライマリディスプレイかどうか
}

//デフォルト値を設定
impl Default for YamanohaDisplayInfo {
    fn default() -> Self {
        Self {
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
        }
    }
}

// callbacksモジュールからWS_SENDERを公開
pub use callbacks::WS_SENDER;
// wrappersモジュールからすべてのものを公開
pub use wrappers::*;

// リモート入力ハンドラ
pub fn handle_remote_input(message: &str) {
    crate::input_control::handle_remote_input(message);
}
