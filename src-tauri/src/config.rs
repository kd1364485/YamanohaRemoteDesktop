use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use once_cell::sync::Lazy;

use crate::models::ExtensionsConfig;

// 拡張子リストのキャッシュ
pub(crate) static EXTENSIONS_CACHE: Lazy<Mutex<Vec<String>>> = Lazy::new(|| {
    Mutex::new(load_extensions_from_file())
});

pub(crate) fn load_extensions_from_file() -> Vec<String> {
    let config_path = PathBuf::from("extensions.json");

    if let Ok(content) = fs::read_to_string(&config_path) {
        if let Ok(config) = serde_json::from_str::<ExtensionsConfig>(&content) {
            return config.text_extensions;
        }
    }

    // 設定ファイルの読み込みに失敗した場合のデフォルト値
    vec![
        "js", "ts", "jsx", "tsx", "py", "rs", "go", "java", "c", "cpp", "h", "hpp",
        "cs", "php", "rb", "swift", "kt", "scala", "clj", "hs", "ml", "fs",
        "md", "txt", "json", "yaml", "yml", "toml", "xml", "html", "css", "scss",
        "sql", "sh", "bash", "zsh", "ps1", "bat", "cmd", "dockerfile", "makefile",
        "r", "m", "lua", "pl", "pm", "t", "vue", "svelte", "astro"
    ].iter().map(|s| s.to_string()).collect()
}

pub(crate) fn get_extensions_cached() -> Vec<String> {
    EXTENSIONS_CACHE.lock().unwrap().clone()
}

#[tauri::command]
pub(crate) fn get_extensions() -> Vec<String> {
    get_extensions_cached()
}

#[tauri::command]
pub(crate) fn save_extensions(extensions: Vec<String>) -> Result<(), String> {
    let config = ExtensionsConfig {
        text_extensions: extensions.clone(),
    };

    let json_content = serde_json::to_string_pretty(&config)
        .map_err(|e| format!("JSONシリアライズエラー: {}", e))?;

    fs::write("extensions.json", json_content)
        .map_err(|e| format!("ファイル書き込みエラー: {}", e))?;

    // キャッシュを更新
    *EXTENSIONS_CACHE.lock().unwrap() = extensions;

    Ok(())
}

pub(crate) fn is_text_file(path: &Path) -> bool {
    let extensions = get_extensions_cached();

    path.extension()
        .and_then(|ext| ext.to_str())
        .map(|ext| extensions.iter().any(|e| e.eq_ignore_ascii_case(ext)))
        .unwrap_or(false)
}
