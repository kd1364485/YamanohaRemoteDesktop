use std::fs;
use std::path::PathBuf;
use std::collections::HashMap;
use std::sync::Mutex;
use once_cell::sync::Lazy;
use rayon::prelude::*;
use reqwest::Client;

use crate::models::{CodeChunk, EmbeddedChunk, FileContent};
use crate::project::PROJECT_STATE_CACHE;

// ベクトルストア（メモリ）。setup時にファイルから読み込む
pub(crate) static VECTOR_STORE: Lazy<Mutex<Vec<EmbeddedChunk>>> = Lazy::new(|| {
    Mutex::new(Vec::new())
});

// ベクトルストアの永続化先。setup時にアプリデータディレクトリへ解決される。
// dev監視対象（src-tauri直下）に置くとアプリが再起動してしまうため外部に保存する。
pub(crate) static VECTOR_STORE_PATH: Lazy<Mutex<PathBuf>> = Lazy::new(|| {
    Mutex::new(PathBuf::from("vector_store.json"))
});

// チャンク分割の設定（1チャンクの行数とオーバーラップ行数）
// TPM（毎分トークン数）制限対策のため、チャンクを細かく分割
const CHUNK_LINES: usize = 40;
const CHUNK_OVERLAP: usize = 10;

// Embedding APIのレート制限対策
// 1回のバッチで送るチャンク数（batchEmbedContentsは最大100件まで対応）
const EMBED_BATCH_SIZE: usize = 50;
// 同時に並列実行するバッチ数（順番に1個ずつ送ると遅いため並列化する）
const EMBED_CONCURRENCY: usize = 10;

// 429（レート制限）時の自動リトライ設定
const EMBED_MAX_RETRIES: u32 = 5;
const EMBED_BACKOFF_BASE_MS: u64 = 2000;

// ファイルからベクトルストアを読み込む（なければ空）
pub(crate) fn load_vector_store() -> Vec<EmbeddedChunk> {
    let path = VECTOR_STORE_PATH.lock().unwrap().clone();
    if let Ok(content) = fs::read_to_string(&path) {
        if let Ok(store) = serde_json::from_str::<Vec<EmbeddedChunk>>(&content) {
            return store;
        }
    }
    Vec::new()
}

// ベクトルストアをファイルに保存する
fn save_vector_store(store: &[EmbeddedChunk]) -> Result<(), String> {
    let path = VECTOR_STORE_PATH.lock().unwrap().clone();
    let json = serde_json::to_string(store)
        .map_err(|e| format!("ベクトルストアのシリアライズエラー: {}", e))?;
    fs::write(&path, json)
        .map_err(|e| format!("ベクトルストアの書き込みエラー: {}", e))?;
    Ok(())
}

// 1ファイルを行ベースでチャンクに分割する
fn chunk_file(path: &str, content: &str) -> Vec<CodeChunk> {
    // lines: ファイルの全行を配列で取得
    let lines: Vec<&str> = content.lines().collect();
    // chunks: 分割されたコードチャンクの配列
    let mut chunks = Vec::new();

    if lines.is_empty() {
        return chunks;
    }

    // 次のチャンクの開始位置を進める量（オーバーラップ分だけ重ねる）
    let step = CHUNK_LINES - CHUNK_OVERLAP;
    let mut start = 0;

    while start < lines.len() {
        let end = (start + CHUNK_LINES).min(lines.len());
        let chunk_content = lines[start..end].join("\n");

        chunks.push(CodeChunk {
            path: path.to_string(),
            start_line: start + 1, // 1始まりにする
            end_line: end,
            content: chunk_content,
        });

        // 最後まで到達したら終了
        if end == lines.len() {
            break;
        }

        start += step;
    }

    chunks
}

// 全ファイルを並列でチャンク分割する
fn chunk_all_files(files: &[FileContent]) -> Vec<CodeChunk> {
    files
        .par_iter()
        .flat_map(|f| chunk_file(&f.path, &f.content))
        .collect()
}

// 429エラーのレスポンスJSONから retryDelay（"38s" など）をミリ秒で取り出す
fn parse_retry_delay_ms(error_text: &str) -> Option<u64> {
    let json: serde_json::Value = serde_json::from_str(error_text).ok()?;
    let details = json["error"]["details"].as_array()?;

    for detail in details {
        if let Some(delay) = detail["retryDelay"].as_str() {
            // "38s" のような形式 → 秒数を取り出す
            let secs: u64 = delay.trim_end_matches('s').parse().ok()?;
            return Some(secs * 1000);
        }
    }
    None
}

// 1バッチ分のテキストをEmbeddingする（429/5xxは指数バックオフで自動リトライ）
async fn embed_batch(
    client: &Client,
    url: &str,
    batch: &[String],
) -> Result<Vec<Vec<f32>>, String> {
    let requests: Vec<serde_json::Value> = batch
        .iter()
        .map(|text| {
            serde_json::json!({
                "model": "models/gemini-embedding-001",
                "content": { "parts": [{ "text": text }] }
            })
        })
        .collect();

    let body = serde_json::json!({ "requests": requests });

    let mut attempt: u32 = 0;
    let json: serde_json::Value = loop {
        let response = client
            .post(url)
            .header("Content-Type", "application/json")
            .json(&body)
            .send()
            .await
            .map_err(|e| format!("Embeddingネットワークエラー: {}", e))?;

        let status = response.status();

        if status.is_success() {
            break response
                .json()
                .await
                .map_err(|e| format!("Embeddingレスポンス解析エラー: {}", e))?;
        }

        let error_text = response
            .text()
            .await
            .unwrap_or_else(|_| "エラーメッセージなし".to_string());

        let code = status.as_u16();
        // リトライ対象: 429（レート制限）と5xx（サーバー側の一時的エラー: 500/502/503/504など）
        let retryable = code == 429 || (500..=599).contains(&code);
        if !retryable {
            return Err(format!("Embedding APIエラー (HTTP {}): {}", status, error_text));
        }

        if attempt >= EMBED_MAX_RETRIES {
            return Err(format!(
                "Embedding APIエラー (HTTP {}) が {}回のリトライ後も解消しませんでした。時間をおいて再試行してください。詳細: {}",
                status, EMBED_MAX_RETRIES, error_text
            ));
        }

        // サーバー指定の待機時間があれば優先、なければ指数バックオフ（2,4,8,16...秒）
        let backoff_ms = parse_retry_delay_ms(&error_text)
            .unwrap_or_else(|| EMBED_BACKOFF_BASE_MS * 2u64.pow(attempt));

        eprintln!(
            "HTTP {} 検知: {}ms 待機してリトライします（{}/{}）",
            status,
            backoff_ms,
            attempt + 1,
            EMBED_MAX_RETRIES
        );

        tokio::time::sleep(std::time::Duration::from_millis(backoff_ms)).await;
        attempt += 1;
    };

    let mut embeddings: Vec<Vec<f32>> = Vec::new();
    if let Some(arr) = json["embeddings"].as_array() {
        for emb in arr {
            if let Some(values) = emb["values"].as_array() {
                let vector: Vec<f32> = values
                    .iter()
                    .filter_map(|v| v.as_f64().map(|f| f as f32))
                    .collect();
                embeddings.push(vector);
            }
        }
    }
    Ok(embeddings)
}

// 複数のテキストをGemini Embedding APIでベクトル化する（バッチを並列処理）
pub(crate) async fn embed_texts(api_key: &str, texts: &[String]) -> Result<Vec<Vec<f32>>, String> {
    use futures::stream::StreamExt;

    eprintln!("Embedding開始: {}個のテキスト", texts.len());
    let client = Client::new();
    let url = format!(
        "https://generativelanguage.googleapis.com/v1beta/models/gemini-embedding-001:batchEmbedContents?key={}",
        api_key
    );

    // 借用をasyncタスクに跨がせるとライフタイムの制約に引っかかるため、所有権付きで分割する
    let batches: Vec<Vec<String>> = texts
        .chunks(EMBED_BATCH_SIZE)
        .map(|b| b.to_vec())
        .collect();
    let total_batches = batches.len();

    // 各バッチを非同期タスク化し、最大EMBED_CONCURRENCY個まで並列実行する。
    // buffered()は投入順に結果を返すため、チャンクとベクトルの対応が保たれる。
    let results: Vec<Result<Vec<Vec<f32>>, String>> =
        futures::stream::iter(batches.into_iter().enumerate()
        .map(|(i, batch)| {
            let client = client.clone();
            let url = url.clone();
            async move {
                eprintln!("バッチ {}/{} を送信 ({}個)", i + 1, total_batches, batch.len());
                embed_batch(&client, &url, &batch).await
            }
        }))
        .buffered(EMBED_CONCURRENCY)
        .collect()
        .await;

    // 投入順（=チャンク順）に結合する
    let mut all_embeddings: Vec<Vec<f32>> = Vec::new();
    for r in results {
        all_embeddings.extend(r?);//成功なら中身を取り出し失敗ならエラーを返す
    }

    Ok(all_embeddings)
}

// インデックス構築・更新の共通処理（差分ベース）。
// 既存ベクトルストアと内容が一致するチャンクは埋め込みを再利用し、
// 新規・変更されたチャンクだけをEmbedding APIに送る。
// 戻り値: (合計チャンク数, 新たにベクトル化したチャンク数)
async fn rebuild_index_incremental(api_key: &str) -> Result<(usize, usize), String> {
    // キャッシュからファイル内容を取得（ロックはawait前に解放）
    let files_content = {
        let cache = PROJECT_STATE_CACHE.lock().unwrap();
        match cache.as_ref() {
            Some(state) => state.files_content.clone(),
            None => {
                return Err("プロジェクト状態がまだ生成されていません。先に監視を開始してください".to_string())
            }
        }
    };

    // チャンク分割（並列）
    let new_chunks = chunk_all_files(&files_content);
    eprintln!("生成したチャンク数: {}", new_chunks.len());

    // チャンクが空ならストアもクリアする
    if new_chunks.is_empty() {
        save_vector_store(&[])?;
        *VECTOR_STORE.lock().unwrap() = Vec::new();
        return Ok((0, 0));
    }

    // 既存ストアを「内容 → 埋め込みベクトル」のマップにする（再利用用）。
    // 内容が同じチャンクは行番号がずれていても同じ埋め込みになるため再利用できる。
    let existing: HashMap<String, Vec<f32>> = {
        let store = VECTOR_STORE.lock().unwrap();
        store
            .iter()
            .map(|ec| (ec.chunk.content.clone(), ec.embedding.clone()))
            .collect()
    };

    // 既存に無い（=新規・変更された）チャンクだけをベクトル化対象にする
    let mut to_embed_idx: Vec<usize> = Vec::new();
    let mut to_embed_texts: Vec<String> = Vec::new();
    for (i, c) in new_chunks.iter().enumerate() {
        if !existing.contains_key(&c.content) {
            to_embed_idx.push(i);
            to_embed_texts.push(c.content.clone());
        }
    }

    let newly_embedded = to_embed_texts.len();
    eprintln!(
        "再利用: {} チャンク / 新規ベクトル化: {} チャンク",
        new_chunks.len() - newly_embedded,
        newly_embedded
    );

    // 新規チャンクだけEmbedding APIへ
    let new_embeddings = if to_embed_texts.is_empty() {
        Vec::new()
    } else {
        embed_texts(api_key, &to_embed_texts).await?
    };

    if new_embeddings.len() != to_embed_texts.len() {
        return Err(format!(
            "チャンク数({})とベクトル数({})が一致しません",
            to_embed_texts.len(),
            new_embeddings.len()
        ));
    }

    // インデックス → 新規埋め込み のマップ
    let mut embed_map: HashMap<usize, Vec<f32>> =
        to_embed_idx.into_iter().zip(new_embeddings.into_iter()).collect();

    // 全チャンクを「新規埋め込み or 既存再利用」で組み立てる
    let embedded: Vec<EmbeddedChunk> = new_chunks
        .into_iter()
        .enumerate()
        .map(|(i, chunk)| {
            let embedding = embed_map
                .remove(&i)
                .or_else(|| existing.get(&chunk.content).cloned())
                .unwrap_or_default();
            EmbeddedChunk { chunk, embedding }
        })
        .collect();

    let total = embedded.len();

    // 永続化＋メモリ保存
    save_vector_store(&embedded)?;
    *VECTOR_STORE.lock().unwrap() = embedded;

    Ok((total, newly_embedded))
}

// プロジェクトをチャンク化＆ベクトル化してベクトルストアを構築する（手動の全体構築）
#[tauri::command]
pub(crate) async fn build_vector_index(api_key: String) -> Result<usize, String> {
    eprintln!("インデックス構築開始");
    let api_key = api_key.trim();
    if api_key.is_empty() {
        return Err("APIキーが入力されていません".to_string());
    }

    let (total, newly) = rebuild_index_incremental(api_key).await?;
    eprintln!("ベクトルストアを構築しました: {} チャンク（新規 {}）", total, newly);
    Ok(total)
}

// ベクトルストア（インデックス）が既に構築済みかどうかを返す。
// アプリ再起動後、永続化済みインデックスの有無を判定するために使う。
#[tauri::command]
pub(crate) fn has_vector_index() -> bool {
    !VECTOR_STORE.lock().unwrap().is_empty()
}

// ファイル変更時に呼ばれる増分更新。変更・新規チャンクだけ再ベクトル化する。
// 戻り値: 新たにベクトル化したチャンク数（0なら変更なし）
#[tauri::command]
pub(crate) async fn reindex_changed_chunks(api_key: String) -> Result<usize, String> {
    let api_key = api_key.trim();
    if api_key.is_empty() {
        return Err("APIキーが入力されていません".to_string());
    }

    // インデックス未構築なら何もしない（手動構築が先）
    if VECTOR_STORE.lock().unwrap().is_empty() {
        return Ok(0);
    }

    let (_total, newly) = rebuild_index_incremental(api_key).await?;
    Ok(newly)
}
