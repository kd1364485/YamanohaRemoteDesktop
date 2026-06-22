use std::fs;
use std::path::PathBuf;

use crate::gemini::generate_with_gemini;
use crate::models::MagicCommentRequest;
use crate::project::read_file_safely;
use crate::rag::retrieve_relevant_chunks;

// マジックコメントのマーカー。この後ろに続く指示文をAIへの依頼として扱う
pub(crate) const MAGIC_MARKER: &str = "Synapse:";

// ファイル内容からマジックコメント行を探し、(行インデックス, 指示文) を返す
// 例: "    // Synapse: ここにプレイヤーの移動処理を書いて" を検知する
pub(crate) fn extract_magic_comment(content: &str) -> Option<(usize, String)> {
    for (i, line) in content.lines().enumerate() {
        if let Some(pos) = line.find(MAGIC_MARKER) {
            // マーカーの直前がコメント記号で終わっているか確認する
            let before = line[..pos].trim_end();
            let is_comment = before.ends_with("//")
                || before.ends_with('#')
                || before.ends_with("--")
                || before.ends_with("/*")
                || before.ends_with(';');
            if !is_comment {
                continue;
            }

            // マーカー以降を指示文として取り出す（末尾のコメント終端記号は除去）
            let instruction = line[pos + MAGIC_MARKER.len()..]
                .trim()
                .trim_end_matches("*/")
                .trim_end_matches("-->")
                .trim()
                .to_string();

            if !instruction.is_empty() {
                return Some((i, instruction));
            }
        }
    }
    None
}

// Geminiの出力からマークダウンのコードフェンス(```)を除去する
fn strip_code_fences(text: &str) -> String {
    let trimmed = text.trim();
    if !trimmed.starts_with("```") {
        return trimmed.to_string();
    }

    let mut lines: Vec<&str> = trimmed.lines().collect();
    // 先頭の ```lang 行を除去
    if !lines.is_empty() {
        lines.remove(0);
    }
    // 末尾の ``` 行を除去
    if let Some(last) = lines.last() {
        if last.trim().starts_with("```") {
            lines.pop();
        }
    }
    lines.join("\n")
}

// マジックコメントを検知したファイルを処理する。
// RAGで関連コードを集め、Geminiにコード生成を依頼し、コメント行を生成コードで置き換える。
#[tauri::command]
pub(crate) async fn process_magic_comment(request: MagicCommentRequest) -> Result<String, String> {
    let api_key = request.api_key.trim();
    if api_key.is_empty() {
        return Err("APIキーが入力されていません".to_string());
    }

    let path = PathBuf::from(&request.file_path);
    let content = read_file_safely(&path)?;

    // マジックコメントを抽出
    let (line_idx, instruction) = extract_magic_comment(&content)
        .ok_or("マジックコメントが見つかりませんでした")?;

    eprintln!("マジックコメント処理開始: {} -> 「{}」", request.file_path, instruction);

    // RAGで関連コードを取得（失敗してもコンテキストなしで続行）
    let context = match retrieve_relevant_chunks(api_key, &instruction, 8).await {
        Ok(chunks) => chunks
            .iter()
            .map(|c| format!("=== {} ({}-{}行) ===\n{}", c.path, c.start_line, c.end_line, c.content))
            .collect::<Vec<_>>()
            .join("\n\n"),
        Err(e) => {
            eprintln!("RAG検索に失敗（コンテキストなしで続行）: {}", e);
            String::new()
        }
    };

    // インデント（マジックコメント行の先頭の空白）を取得して生成コードに適用する
    let target_line = content.lines().nth(line_idx).unwrap_or("");
    let indent: String = target_line
        .chars()
        .take_while(|c| c.is_whitespace())
        .collect();

    let prompt = format!(
        "あなたは熟練のプログラマーです。以下の指示に従ってコードを生成してください。\n\
        制約:\n\
        - 生成するコードのみを出力してください。説明文やマークダウンのコードブロック記号(```)は一切含めないでください。\n\
        - 編集対象ファイルの言語・コーディングスタイルに合わせてください。\n\
        - コメント行を置き換える形で挿入されるため、そのまま貼り付けて動くコードにしてください。\n\n\
        # 関連コード（参考）\n{}\n\n\
        # 編集対象ファイル: {}\n```\n{}\n```\n\n\
        # 指示\n{}\n",
        context, request.file_path, content, instruction
    );

    // Geminiでコード生成
    let generated = generate_with_gemini(api_key, &request.model, &prompt).await?;
    let cleaned = strip_code_fences(&generated);

    // 生成コードの各行にインデントを付与する（空行はそのまま）
    let replacement = cleaned
        .lines()
        .map(|l| {
            if l.trim().is_empty() {
                String::new()
            } else {
                format!("{}{}", indent, l)
            }
        })
        .collect::<Vec<_>>()
        .join("\n");

    // マジックコメント行から Synapse: を削除し、生成コードをその後に挿入
    let mut new_lines: Vec<String> = content.lines().map(|s| s.to_string()).collect();
    if line_idx < new_lines.len() {
        // Synapse: を削除
        let original_line = &new_lines[line_idx];
        let updated_line = original_line.replacen(MAGIC_MARKER, "", 1);
        new_lines[line_idx] = updated_line;

        // 生成コードを行の後に挿入
        let replacement_lines: Vec<String> = replacement.lines().map(|s| s.to_string()).collect();
        for (i, line) in replacement_lines.iter().enumerate() {
            new_lines.insert(line_idx + 1 + i, line.clone());
        }
    }
    let new_content = new_lines.join("\n");

    fs::write(&path, new_content)
        .map_err(|e| format!("ファイル書き込みエラー: {}", e))?;

    eprintln!("マジックコメント処理完了: {}", request.file_path);
    Ok(format!("{} にコードを生成しました", request.file_path))
}
