// Prevents additional console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

// NVIDIA Optimus 環境で統合グラフィックス（Intel iGPU）を優先する。
// これにより Desktop Duplication API が内蔵ディスプレイで動作するようになる。
// NVENC エンコードは、プロセスが iGPU 上でも引き続き NVIDIA dGPU を直接利用できる。
#[cfg(target_os = "windows")]
#[no_mangle]
#[used]
pub static NvOptimusEnablement: u32 = 0;

fn main() {
    yamanoha_remote_desktop_lib::run()
}
