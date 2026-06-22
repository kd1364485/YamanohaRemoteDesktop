import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";

import { PATH_STORAGE_KEY } from "./constants";
import { dom } from "./dom";
import { state } from "./state";
import { updateSelectedPathStatus, updateWatchStatus } from "./statusBar";

export async function selectFolder() {
  try {
    const selected = await open({
      directory: true,
      multiple: false,
      title: "フォルダを選択",
    });

    if (selected && typeof selected === "string") {
      state.currentPath = selected;
      localStorage.setItem(PATH_STORAGE_KEY, selected);
      if (dom.selectedPathEl) {
        dom.selectedPathEl.textContent = `選択されたパス: ${selected}`;
      }
      updateSelectedPathStatus(selected);
      if (dom.startWatchBtn) {
        dom.startWatchBtn.disabled = false;
      }
      if (dom.buildIndexBtn) {
        dom.buildIndexBtn.disabled = false;
      }

      // 監視中なら一旦停止してから再開
      if (dom.watchStatusEl && dom.watchStatusEl.textContent === "監視中") {
        await stopWatching();
      }

      // 新しいフォルダで監視を開始
      await startWatching();

      if (dom.watchStatusEl) {
        dom.watchStatusEl.textContent = "監視対象が切り替わりました ✅";
        setTimeout(() => {
          if (dom.watchStatusEl) {
            dom.watchStatusEl.textContent = "監視中";
          }
        }, 2000);
      }
    }
  } catch (error) {
    console.error("フォルダ選択エラー:", error);
  }
}

export async function startWatching() {
  if (!state.currentPath) {
    if (dom.watchStatusEl) {
      dom.watchStatusEl.textContent = "フォルダが選択されていません";
    }
    return;
  }

  try {
    if (dom.watchStatusEl) {
      dom.watchStatusEl.textContent = "監視を開始中...";
    }

    await invoke("start_watching", { path: state.currentPath });

    if (dom.watchStatusEl) {
      dom.watchStatusEl.textContent = "監視中";
    }
    updateWatchStatus("watching");
    if (dom.startWatchBtn) {
      dom.startWatchBtn.disabled = true;
    }
    if (dom.stopWatchBtn) {
      dom.stopWatchBtn.disabled = false;
    }
  } catch (error) {
    console.error("監視開始エラー:", error);
    if (dom.watchStatusEl) {
      dom.watchStatusEl.textContent = `監視開始に失敗しました: ${error}`;
    }
  }
}

export async function stopWatching() {
  try {
    await invoke("stop_watching");

    if (dom.watchStatusEl) {
      dom.watchStatusEl.textContent = "監視停止";
    }
    updateWatchStatus("stopped");
    if (dom.startWatchBtn) {
      dom.startWatchBtn.disabled = false;
    }
    if (dom.stopWatchBtn) {
      dom.stopWatchBtn.disabled = true;
    }
  } catch (error) {
    console.error("監視停止エラー:", error);
    if (dom.watchStatusEl) {
      dom.watchStatusEl.textContent = `監視停止に失敗しました: ${error}`;
    }
  }
}
