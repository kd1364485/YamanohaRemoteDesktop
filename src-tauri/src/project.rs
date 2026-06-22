use std::fs;
use std::path::Path;
use std::collections::HashMap;
use std::sync::Mutex;
use ignore::Walk;
use once_cell::sync::Lazy;
use rayon::prelude::*;

use crate::config::is_text_file;
use crate::models::{FileContent, FileInfo, ProjectState};

// 最新状態のキャッシュ
pub(crate) static PROJECT_STATE_CACHE: Lazy<Mutex<Option<ProjectState>>> = Lazy::new(|| {
    Mutex::new(None)
});

#[tauri::command]
pub(crate) fn get_project_state() -> Result<ProjectState, String> {
    PROJECT_STATE_CACHE
        .lock()
        .unwrap()
        .clone()
        .ok_or_else(|| "プロジェクト状態がまだ生成されていません".to_string())
}

pub(crate) fn read_file_safely(path: &Path) -> Result<String, String> {
    use std::io::Read;

    let mut file = fs::File::open(path)
        .map_err(|e| format!("ファイルオープンエラー: {}", e))?;

    let mut buffer = Vec::new();
    file.read_to_end(&mut buffer)
        .map_err(|e| format!("ファイル読み込みエラー: {}", e))?;

    // UTF-8検証
    std::str::from_utf8(&buffer)
        .map_err(|_| "ファイルはUTF-8エンコーディングではありません（バイナリファイルの可能性があります）".to_string())?;

    // UTF-8文字列に変換
    String::from_utf8(buffer)
        .map_err(|e| format!("UTF-8変換エラー: {}", e))
}

fn should_exclude_directory(dir_name: &str) -> bool {
    const EXCLUDED_DIRS: &[&str] = &[
        "node_modules", ".git", ".svn", ".hg", "dist", "build", "target",
        "out", "bin", "obj", "vendor", ".vscode", ".idea", "coverage",
        "__pycache__", ".pytest_cache", ".next", ".nuxt", ".cache",
    ];

    EXCLUDED_DIRS.contains(&dir_name)
}

// ディレクトリを1回だけ走査してファイル情報を収集
fn collect_files(source_path: &Path) -> Result<Vec<FileInfo>, String> {
    let mut files: Vec<FileInfo> = Vec::new();
    let walker = Walk::new(source_path);

    for result in walker {
        let entry = result.map_err(|e| format!("ディレクトリ走査エラー: {}", e))?;
        let entry_path = entry.path().to_path_buf();

        // ディレクトリの除外チェック
        if entry_path.is_dir() {
            if let Some(dir_name) = entry_path.file_name().and_then(|n| n.to_str()) {
                if should_exclude_directory(dir_name) {
                    continue;
                }
            }
            continue;
        }

        // テキストファイルのみを対象にする
        if !is_text_file(&entry_path) {
            continue;
        }

        let relative_path = entry_path
            .strip_prefix(source_path)
            .map_err(|e| format!("相対パス計算エラー: {}", e))?;

        let path_str = relative_path
            .to_str()
            .ok_or("パス文字列変換エラー")?;

        // Windowsのパス区切り文字をスラッシュに変換
        let normalized = path_str.replace('\\', "/");

        let extension = entry_path
            .extension()
            .and_then(|ext| ext.to_str())
            .unwrap_or("")
            .to_lowercase();

        files.push(FileInfo {
            relative_path: normalized,
            full_path: entry_path,
            extension,
        });
    }

    files.sort_by(|a, b| a.relative_path.cmp(&b.relative_path));

    Ok(files)
}

fn generate_file_tree(files: &[FileInfo]) -> Result<String, String> {
    println!("収集したファイル数: {}", files.len());

    // ツリー文字列を生成
    let mut output = String::from("■ Project Structure\n\n");

    // シンプルなツリー出力
    let mut last_dir: Option<String> = None;

    for file in files {
        let parts: Vec<&str> = file.relative_path.split('/').collect();

        if parts.len() > 1 {
            let dir = parts[..parts.len()-1].join("/");
            let filename = parts.last().unwrap_or(&"");

            // ディレクトリが変わったら出力
            if last_dir.as_ref() != Some(&dir) {
                output.push_str(&format!("{}/\n", dir));
                last_dir = Some(dir.clone());
            }

            output.push_str(&format!("  {}\n", filename));
        } else {
            // ルートのファイル
            output.push_str(&format!("{}\n", file.relative_path));
        }
    }

    Ok(output)
}

pub(crate) fn update_project_state(source_path: &Path) -> Result<(), String> {
    // ファイルを収集
    let files = collect_files(source_path)?;

    // ファイルツリーを生成
    let file_tree = generate_file_tree(&files)?;

    // プロジェクト概要を生成
    let overview = generate_project_overview(source_path, &files)?;

    // ファイル内容を読み込み
    let files_content: Vec<FileContent> = files
        .par_iter()
        .filter_map(|file|
        {
            read_file_safely(&file.full_path)
                .ok()
                .map(|content| FileContent {
                    path: file.relative_path.clone(),
                    content,
                })
        })
        .collect();

    let state = ProjectState {
        overview,
        file_tree,
        files_content,
    };

    *PROJECT_STATE_CACHE.lock().unwrap() = Some(state);

    Ok(())
}

fn generate_project_overview(source_path: &Path, files: &[FileInfo]) -> Result<String, String> {
    let mut tech_stack: Vec<String> = Vec::new();
    let mut file_counts: HashMap<String, usize> = HashMap::new();

    for file in files {
        *file_counts.entry(file.extension.clone()).or_insert(0) += 1;
    }

    // テックスタックを推測
    if file_counts.contains_key(&"js".to_string()) || file_counts.contains_key(&"jsx".to_string()) {
        tech_stack.push("JavaScript".to_string());
    }
    if file_counts.contains_key(&"ts".to_string()) || file_counts.contains_key(&"tsx".to_string()) {
        tech_stack.push("TypeScript".to_string());
    }
    if file_counts.contains_key(&"py".to_string()) {
        tech_stack.push("Python".to_string());
    }
    if file_counts.contains_key(&"rs".to_string()) {
        tech_stack.push("Rust".to_string());
    }
    if file_counts.contains_key(&"go".to_string()) {
        tech_stack.push("Go".to_string());
    }
    if file_counts.contains_key(&"java".to_string()) {
        tech_stack.push("Java".to_string());
    }
    if file_counts.contains_key(&"cpp".to_string()) || file_counts.contains_key(&"c".to_string()) {
        tech_stack.push("C/C++".to_string());
    }
    if file_counts.contains_key(&"html".to_string()) || file_counts.contains_key(&"css".to_string()) {
        tech_stack.push("HTML/CSS".to_string());
    }
    if file_counts.contains_key(&"json".to_string()) {
        tech_stack.push("JSON configuration".to_string());
    }
    if file_counts.contains_key(&"md".to_string()) {
        tech_stack.push("Markdown documentation".to_string());
    }

    let folder_name = source_path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("Project");

    let mut overview = String::from("■ Project Overview\n\n");
    overview.push_str(&format!("**Project Name**: {}\n\n", folder_name));
    overview.push_str("**Tech Stack**:\n");

    if tech_stack.is_empty() {
        overview.push_str("- Various text files\n");
    } else {
        for tech in &tech_stack {
            overview.push_str(&format!("- {}\n", tech));
        }
    }

    overview.push_str("\n**File Statistics**:\n");
    for (ext, count) in &file_counts {
        overview.push_str(&format!("- .{}: {} files\n", ext, count));
    }

    overview.push_str("\n**Purpose**: This archive contains source code files for AI analysis.\n");
    overview.push_str("The files have been filtered to include only text-based source code files.\n");
    overview.push_str("Binary files and common build artifacts have been excluded.\n");

    Ok(overview)
}
