import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

import { dom } from "./dom";
import { state } from "./state";
import { showToast } from "./toast";

// マジックコメント（// Synapse: ...）検知時の自動コード生成リスナーを登録する
export async function setupMagicCommentListener() {
  await listen<string>("magic-comment-detected", async (event) => {
    const filePath = event.payload;
    if (!dom.apiKeyInput) return;

    // 同一ファイルの多重処理を防ぐ
    if (state.processingMagicFiles.has(filePath)) return;

    const apiKey = dom.apiKeyInput.value.trim();
    if (!apiKey) {
      showToast("Synapseコメントを検知しましたが、APIキーが未設定です", "error");
      return;
    }

    state.processingMagicFiles.add(filePath);
    const model = dom.modelSelect?.value || "gemini-2.5-flash";

    try {
      showToast("Synapseコメントを検知、コードを生成中...", "info");

      const result = await invoke<string>("process_magic_comment", {
        request: { api_key: apiKey, file_path: filePath, model },
      });

      showToast(result, "success");
    } catch (error) {
      console.error("マジックコメント処理エラー:", error);
      showToast(`マジックコメント処理に失敗: ${error}`, "error");
    } finally {
      state.processingMagicFiles.delete(filePath);
    }
  });
}
