import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

import { MODEL_STORAGE_KEY, PATH_STORAGE_KEY } from "./modules/constants";
import { dom, initDom } from "./modules/dom";
import { state } from "./modules/state";
import { loadApiKey, saveApiKey } from "./modules/storage";
import { updateIndexStatus, updateSelectedPathStatus } from "./modules/statusBar";
import { openSettingsModal, closeSettingsModal } from "./modules/settingsModal";
import { selectFolder, startWatching, stopWatching } from "./modules/watcher";
import { buildIndex, reindexChangedChunks } from "./modules/indexer";
import {
  loadPromptSettings,
  checkPromptChanges,
  savePromptSettings,
  resetPromptSettings,
} from "./modules/prompt";
import {
  addExtension,
  loadExtensions,
  removeExtension,
  saveExtensions,
} from "./modules/extensions";
import { sendChat } from "./modules/chat";
import { setupMagicCommentListener } from "./modules/magicComment";

window.addEventListener("DOMContentLoaded", () => {
  // DOM要素参照を初期化
  initDom();

  // 保存されたAPIキーを復元
  const savedApiKey = loadApiKey();
  if (savedApiKey && dom.apiKeyInput) {
    dom.apiKeyInput.value = savedApiKey;
  }

  // 保存されたモデル選択を復元
  const savedModel = localStorage.getItem(MODEL_STORAGE_KEY);
  if (savedModel && dom.modelSelect) {
    dom.modelSelect.value = savedModel;
  }
  // モデル変更時に自動保存
  dom.modelSelect?.addEventListener("change", () => {
    if (dom.modelSelect) {
      localStorage.setItem(MODEL_STORAGE_KEY, dom.modelSelect.value);
    }
  });

  // プロンプト設定を読み込み
  loadPromptSettings();

  // 保存されたパスがあれば自動で監視を開始
  const savedPath = localStorage.getItem(PATH_STORAGE_KEY);
  if (savedPath) {
    state.currentPath = savedPath;
    if (dom.selectedPathEl) {
      dom.selectedPathEl.textContent = `選択されたパス: ${savedPath}`;
    }
    updateSelectedPathStatus(savedPath);
    if (dom.startWatchBtn) {
      dom.startWatchBtn.disabled = false;
    }
    if (dom.buildIndexBtn) {
      dom.buildIndexBtn.disabled = false;
    }
    // 自動で監視を開始
    startWatching();
  }

  // 永続化済みインデックスがあれば、自動増分更新を有効化する
  (async () => {
    try {
      state.indexBuilt = await invoke<boolean>("has_vector_index");
      if (state.indexBuilt && dom.indexStatusEl) {
        dom.indexStatusEl.textContent = "構築済みインデックスを読み込みました ✅";
        updateIndexStatus("indexed");
      }
    } catch (error) {
      console.error("インデックス状態の確認に失敗:", error);
    }
  })();

  // プロジェクト状態の更新イベントを購読
  (async () => {
    await listen("project-updated", () => {
      if (dom.watchStatusEl) {
        dom.watchStatusEl.textContent = "プロジェクト状態が更新されました ✅";
        setTimeout(() => {
          if (dom.watchStatusEl) {
            dom.watchStatusEl.textContent = "監視中";
          }
        }, 2000);
      }
      // インデックス構築済みなら、変更チャンクだけ自動で再ベクトル化する
      reindexChangedChunks();
    });
  })();

  // マジックコメント検知時の自動コード生成リスナーを登録
  setupMagicCommentListener();

  // ボタンのイベント配線
  dom.selectFolderBtn?.addEventListener("click", selectFolder);
  dom.startWatchBtn?.addEventListener("click", startWatching);
  dom.stopWatchBtn?.addEventListener("click", stopWatching);
  dom.buildIndexBtn?.addEventListener("click", buildIndex);
  dom.addExtensionBtn?.addEventListener("click", addExtension);
  dom.loadExtensionsBtn?.addEventListener("click", loadExtensions);
  dom.saveExtensionsBtn?.addEventListener("click", saveExtensions);
  dom.sendChatBtn?.addEventListener("click", sendChat);
  dom.loadPromptBtn?.addEventListener("click", loadPromptSettings);
  dom.savePromptBtn?.addEventListener("click", savePromptSettings);
  dom.resetPromptBtn?.addEventListener("click", resetPromptSettings);

  // モーダル開閉
  dom.settingsBtn?.addEventListener("click", openSettingsModal);
  dom.closeSettingsBtn?.addEventListener("click", closeSettingsModal);
  dom.settingsModal?.addEventListener("click", (e) => {
    if (e.target === dom.settingsModal) {
      closeSettingsModal();
    }
  });

  // プロンプト入力時に変更を検知
  dom.promptInput?.addEventListener("input", checkPromptChanges);

  // Enterキーで拡張子を追加
  dom.extensionInput?.addEventListener("keypress", (e) => {
    if (e.key === "Enter") {
      addExtension();
    }
  });

  // Enterキーでチャットを送信
  dom.chatInput?.addEventListener("keypress", (e) => {
    if (e.key === "Enter") {
      sendChat();
    }
  });

  // APIキー変更時に自動保存
  dom.apiKeyInput?.addEventListener("input", (e) => {
    const target = e.target as HTMLInputElement;
    saveApiKey(target.value);
  });

  // グローバル関数として登録（タグの削除ボタンの onclick から呼ばれる）
  (window as any).removeExtension = removeExtension;

  // 初期ロード時に拡張子設定を読み込む
  loadExtensions();
});
