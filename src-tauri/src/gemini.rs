use reqwest::Client;

use crate::models::{GeminiRequest, GeminiResponse};

// Gemini generateContent APIを呼び出して回答テキストを返す共通関数
pub(crate) async fn generate_with_gemini(api_key: &str, model: &str, prompt: &str) -> Result<String, String> {
    // 許可されたモデルだけを使う（不正な値はデフォルトにフォールバック）
    let allowed_models = ["gemini-2.5-flash", "gemini-2.5-flash-lite", "gemini-3.1-flash-lite", "gemini-3-flash"];
    let model = if allowed_models.contains(&model) {
        model
    } else {
        "gemini-2.5-flash"
    };

    let client = Client::new();
    let url = format!(
        "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent?key={}",
        model, api_key
    );

    let body = serde_json::json!({
        "contents": [{
            "parts": [{
                "text": prompt
            }]
        }]
    });

    let response = client
        .post(&url)//どのAIに送るか
        .header("Content-Type", "application/json")
        .json(&body)//ソースコードや質問内容などが入っている
        .send()
        .await//終わるまで待機
        .map_err(|e| format!("ネットワークエラー: {}", e))?;//エラーがあれば即座に返す

    if !response.status().is_success() {
        let status = response.status();
        let error_text = response.text().await.unwrap_or_else(|_| "エラーメッセージなし".to_string());

        if let Ok(json) = serde_json::from_str::<serde_json::Value>(&error_text) {
            if let Some(msg) = json["error"]["message"].as_str() {
                if msg.contains("API key not valid") {
                    return Err("APIキーが無効です。Google AI Studio (https://aistudio.google.com/app/apikey) で有効なAPIキーを生成してください。".to_string());
                }
                return Err(format!("APIエラー: {}", msg));
            }
        }

        return Err(format!("APIエラー (HTTP {}): リクエストに失敗しました", status));
    }

    let response_json: serde_json::Value = response
        .json()
        .await
        .map_err(|e| format!("レスポンスの解析に失敗しました: {}", e))?;

    let reply = response_json["candidates"][0]["content"]["parts"][0]["text"]
        .as_str()
        .unwrap_or("回答を取得できませんでした")
        .to_string();

    Ok(reply)
}

#[tauri::command]
pub(crate) async fn ask_gemini(request: GeminiRequest) -> Result<GeminiResponse, String> {
    let api_key = request.api_key.trim();

    if api_key.is_empty() {
        return Err("APIキーが入力されていません".to_string());
    }

    let reply = generate_with_gemini(api_key, &request.model, &request.message).await?;

    Ok(GeminiResponse { reply })
}
