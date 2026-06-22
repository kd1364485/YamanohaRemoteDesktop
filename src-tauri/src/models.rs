use serde::{Deserialize, Serialize};
use std::path::PathBuf;

// プロジェクトの最新状態を保持する構造体
#[derive(Serialize, Clone)]
pub(crate) struct ProjectState {
    pub overview: String,
    pub file_tree: String,
    pub files_content: Vec<FileContent>,
}

#[derive(Serialize, Clone)]
pub(crate) struct FileContent {
    pub path: String,
    pub content: String,
}

// コードチャンク（行ベースで分割された塊）
#[derive(Serialize, Deserialize, Clone)]
pub(crate) struct CodeChunk {
    pub path: String,
    pub start_line: usize,
    pub end_line: usize,
    pub content: String,
}

// ベクトル化されたチャンク（チャンク本体＋埋め込みベクトル）
#[derive(Serialize, Deserialize, Clone)]
pub(crate) struct EmbeddedChunk {
    pub chunk: CodeChunk,
    pub embedding: Vec<f32>,
}

// ファイル情報を保持する構造体
#[derive(Clone)]
pub(crate) struct FileInfo {
    pub relative_path: String,
    pub full_path: PathBuf,
    pub extension: String,
}

#[derive(Deserialize, Serialize)]
pub(crate) struct ExtensionsConfig {
    pub text_extensions: Vec<String>,
}

#[derive(Deserialize)]
pub(crate) struct GeminiRequest {
    pub api_key: String,
    pub message: String,
    #[serde(default = "default_model")]
    pub model: String,
}

#[derive(Serialize)]
pub(crate) struct GeminiResponse {
    pub reply: String,
}

#[derive(Deserialize)]
pub(crate) struct MagicCommentRequest {
    pub api_key: String,
    pub file_path: String,
    #[serde(default = "default_model")]
    pub model: String,
}

pub(crate) fn default_model() -> String {
    "gemini-2.5-flash".to_string()
}
