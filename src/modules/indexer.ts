import { invoke } from "@tauri-apps/api/core";

import { dom } from "./dom";
import { state } from "./state";
import { updateIndexStatus } from "./statusBar";

export async function buildIndex() {
  if (!dom.apiKeyInput) return;

  const apiKey = dom.apiKeyInput.value.trim();
  if (!apiKey) {
    if (dom.indexStatusEl) {
      dom.indexStatusEl.textContent = "APIキーを入力してください";
    }
    return;
  }

  try {
    if (dom.indexStatusEl) {
      dom.indexStatusEl.textContent = "インデックスを構築中...（時間がかかる場合があります）";
    }
    if (dom.buildIndexBtn) {
      dom.buildIndexBtn.disabled = true;
    }

    const count = await invoke<number>("build_vector_index", { apiKey });

    state.indexBuilt = true;

    if (dom.indexStatusEl) {
      dom.indexStatusEl.textContent = `インデックス構築完了 ✅ (${count} チャンク)`;
    }
    updateIndexStatus("indexed");
  } catch (error) {
    console.error("インデックス構築エラー:", error);
    if (dom.indexStatusEl) {
      dom.indexStatusEl.textContent = `インデックス構築に失敗しました: ${error}`;
    }
  } finally {
    if (dom.buildIndexBtn) {
      dom.buildIndexBtn.disabled = false;
    }
  }
}

// ファイル変更を検知したら、変更チャンクだけ自動で再ベクトル化する
export async function reindexChangedChunks() {
  if (!state.indexBuilt || state.reindexing || !dom.apiKeyInput) return;

  const apiKey = dom.apiKeyInput.value.trim();
  if (!apiKey) return;

  state.reindexing = true;
  try {
    updateIndexStatus("updating");
    if (dom.indexStatusEl) {
      dom.indexStatusEl.textContent = "ソース変更を検知、インデックスを更新中...";
    }

    const updated = await invoke<number>("reindex_changed_chunks", { apiKey });

    if (dom.indexStatusEl) {
      dom.indexStatusEl.textContent =
        updated > 0
          ? `インデックスを更新しました ✅ (${updated} チャンク再ベクトル化)`
          : "インデックスは最新です ✅";
    }
    updateIndexStatus("updated");
  } catch (error) {
    console.error("インデックス自動更新エラー:", error);
    if (dom.indexStatusEl) {
      dom.indexStatusEl.textContent = `インデックス自動更新に失敗しました: ${error}`;
    }
  } finally {
    state.reindexing = false;
  }
}
