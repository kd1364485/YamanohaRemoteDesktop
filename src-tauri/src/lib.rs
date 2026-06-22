use std::fs;

mod models;
mod config;
mod project;
mod gemini;
mod indexer;
mod rag;
mod magic_comment;
mod watcher;

// Learn more about Tauri commands at https://tauri.app/develop/calling-rust/
#[tauri::command]
fn greet(name: &str) -> String {
    format!("Hello, {}! You've been greeted from Rust!", name)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            use tauri::Manager;
            // ベクトルストアの保存先をアプリデータディレクトリに設定する。
            // src-tauri直下に置くとdev監視でアプリが再起動するため外部に保存する。
            if let Ok(data_dir) = app.path().app_data_dir() {
                let _ = fs::create_dir_all(&data_dir);
                let path = data_dir.join("vector_store.json");
                eprintln!("ベクトルストア保存先: {}", path.display());
                *indexer::VECTOR_STORE_PATH.lock().unwrap() = path;
            }
            // 永続化済みのベクトルストアをメモリに読み込む
            let loaded = indexer::load_vector_store();
            eprintln!("永続化済みインデックスを読み込みました: {} チャンク", loaded.len());
            *indexer::VECTOR_STORE.lock().unwrap() = loaded;
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            greet,
            config::get_extensions,
            config::save_extensions,
            gemini::ask_gemini,
            project::get_project_state,
            watcher::start_watching,
            watcher::stop_watching,
            indexer::build_vector_index,
            indexer::reindex_changed_chunks,
            indexer::has_vector_index,
            rag::search_similar_chunks,
            magic_comment::process_magic_comment
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
