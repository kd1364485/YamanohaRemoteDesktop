use rayon::prelude::*;

use crate::indexer::{embed_texts, VECTOR_STORE};
use crate::models::CodeChunk;

// 2つのベクトルのコサイン類似度を計算する（-1.0〜1.0、大きいほど似ている）
fn cosine_similarity(a: &[f32], b: &[f32]) -> f32 {
    let dot: f32 = a.iter().zip(b.iter()).map(|(x, y)| x * y).sum();
    let norm_a: f32 = a.iter().map(|x| x * x).sum::<f32>().sqrt();
    let norm_b: f32 = b.iter().map(|x| x * x).sum::<f32>().sqrt();

    if norm_a == 0.0 || norm_b == 0.0 {
        return 0.0;
    }

    dot / (norm_a * norm_b)
}

// 質問に関連するチャンクをコサイン類似度で検索してTop-Kを返す共通関数
pub(crate) async fn retrieve_relevant_chunks(
    api_key: &str,
    query: &str,
    top_k: usize,
) -> Result<Vec<CodeChunk>, String> {
    // クエリをベクトル化
    let query_embeddings = embed_texts(api_key, &[query.to_string()]).await?;
    let query_vec = query_embeddings
        .into_iter()
        .next()
        .ok_or("クエリのベクトル化に失敗しました")?;

    // ベクトルストアのスナップショットを取得（ロックは即解放）
    let store = VECTOR_STORE.lock().unwrap().clone();
    if store.is_empty() {
        return Err("ベクトルストアが空です。先にインデックスを構築してください".to_string());
    }

    // 各チャンクとの類似度を並列で計算
    let mut scored: Vec<(f32, CodeChunk)> = store
        .par_iter()
        .map(|ec| (cosine_similarity(&query_vec, &ec.embedding), ec.chunk.clone()))
        .collect();

    // 類似度の高い順にソート
    scored.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap_or(std::cmp::Ordering::Equal));

    // Top-Kだけ取り出す
    Ok(scored.into_iter().take(top_k).map(|(_, c)| c).collect())
}

#[tauri::command]
pub(crate) async fn search_similar_chunks(
    api_key: String,
    query: String,
    top_k: usize,
) -> Result<Vec<CodeChunk>, String> {
    let api_key = api_key.trim();
    if api_key.is_empty() {
        return Err("APIキーが入力されていません".to_string());
    }

    let results = retrieve_relevant_chunks(api_key, &query, top_k).await?;
    eprintln!("検索結果: {} チャンクを返却", results.len());
    Ok(results)
}
