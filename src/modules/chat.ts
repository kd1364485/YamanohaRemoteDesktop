import { invoke } from "@tauri-apps/api/core";

import { DEFAULT_PROMPT } from "./constants";
import { dom } from "./dom";
import { state } from "./state";
import { loadPrompt, saveApiKey } from "./storage";

function addMessage(message: string, isUser: boolean) {
  if (!dom.chatMessagesEl) return;

  const messageDiv = document.createElement("div");
  messageDiv.className = `flex ${isUser ? "justify-end" : "justify-start"}`;

  const bubble = document.createElement("div");
  bubble.className = `max-w-xs px-4 py-2 rounded-lg whitespace-pre-wrap break-words ${
    isUser
      ? "bg-gradient-to-r from-[#00F5D4] to-[#00D4AA] text-black font-medium"
      : "bg-gray-700 text-gray-100"
  }`;
  bubble.textContent = message;

  messageDiv.appendChild(bubble);
  dom.chatMessagesEl.appendChild(messageDiv);
  dom.chatMessagesEl.scrollTop = dom.chatMessagesEl.scrollHeight;
}

export async function sendChat() {
  if (!dom.apiKeyInput || !dom.chatInput) return;

  if (state.isChatting) {
    return;
  }

  const apiKey = dom.apiKeyInput.value.trim();
  const message = dom.chatInput.value.trim();

  if (!apiKey) {
    if (dom.chatStatusEl) {
      dom.chatStatusEl.textContent = "APIキーを入力してください";
    }
    return;
  }

  if (!message) {
    if (dom.chatStatusEl) {
      dom.chatStatusEl.textContent = "メッセージを入力してください";
    }
    return;
  }

  saveApiKey(apiKey);

  state.isChatting = true;
  if (dom.sendChatBtn) {
    dom.sendChatBtn.disabled = true;
    dom.sendChatBtn.textContent = "送信中...";
  }

  addMessage(message, true);
  dom.chatInput.value = "";

  if (dom.chatStatusEl) {
    dom.chatStatusEl.textContent = "AIが回答を生成中...";
  }

  try {
    // プロジェクト状態を取得
    let combinedMessage = message;
    try {
      const projectState = await invoke<{
        overview: string;
        file_tree: string;
        files_content: Array<{ path: string; content: string }>;
      }>("get_project_state");

      // RAG検索：質問に関連するチャンクだけを取得（Top-K）
      let filesContentText: string;
      try {
        const chunks = await invoke<
          Array<{
            path: string;
            start_line: number;
            end_line: number;
            content: string;
          }>
        >("search_similar_chunks", { apiKey, query: message, topK: 8 });

        // 関連チャンクだけを結合（全ソースではなく必要な部分だけ）
        filesContentText = chunks
          .map(
            (c) =>
              `=== ${c.path} (${c.start_line}-${c.end_line}行) ===\n${c.content}`
          )
          .join("\n\n");
      } catch (searchError) {
        // インデックス未構築などで検索失敗時は全ソースにフォールバック
        console.log("RAG検索に失敗、全ソースを使用:", searchError);
        filesContentText = projectState.files_content
          .map((file) => `=== ${file.path} ===\n${file.content}`)
          .join("\n\n");
      }

      // 保存されたプロンプトを取得
      const savedPrompt = loadPrompt() || DEFAULT_PROMPT;

      // プロンプトのプレースホルダーを置換
      combinedMessage = savedPrompt
        .replace("{overview}", projectState.overview)
        .replace("{file_tree}", projectState.file_tree)
        .replace("{files_content}", filesContentText)
        .replace("{message}", message);
    } catch (stateError) {
      // プロジェクト状態が取得できない場合は従来通り
      console.log("プロジェクト状態が取得できません:", stateError);
    }

    const model = dom.modelSelect?.value || "gemini-2.5-flash";

    const response = await invoke<{ reply: string }>("ask_gemini", {
      request: {
        api_key: apiKey,
        message: combinedMessage,
        model,
      },
    });

    addMessage(response.reply, false);

    if (dom.chatStatusEl) {
      dom.chatStatusEl.textContent = "";
    }
  } catch (error) {
    console.error("チャットエラー:", error);
    const errorMessage = String(error);

    // トークン制限エラーの場合はわかりやすいメッセージを表示
    let userErrorMessage = errorMessage;
    if (errorMessage.includes("quota") || errorMessage.includes("limit")) {
      userErrorMessage = "⚠️ APIの使用制限を超えました。プロジェクトのソースコードが大きすぎる可能性があります。しばらく待ってから再試行するか、プロジェクトサイズを小さくしてください。";
    }

    addMessage(`エラー: ${userErrorMessage}`, false);
    if (dom.chatStatusEl) {
      dom.chatStatusEl.textContent = `エラーが発生しました: ${userErrorMessage}`;
    }
  } finally {
    state.isChatting = false;
    if (dom.sendChatBtn) {
      dom.sendChatBtn.disabled = false;
      dom.sendChatBtn.textContent = "送信";
    }
  }
}
