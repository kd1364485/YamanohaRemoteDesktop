import { invoke } from "@tauri-apps/api/core";

import { dom } from "./dom";
import { state } from "./state";

export function renderTags() {
  if (!dom.extensionsTagsEl) return;

  dom.extensionsTagsEl.innerHTML = state.extensionsList.map((ext) => `
    <span class="inline-flex items-center gap-2 px-3 py-1.5 bg-gradient-to-r from-purple-500/20 to-blue-500/20 border border-purple-500/30 rounded-full text-sm text-purple-300 hover:from-purple-500/40 hover:to-blue-500/40 hover:border-purple-500/50 hover:scale-105 hover:shadow-lg hover:shadow-purple-500/20 transition-all duration-300 cursor-pointer">
      <span>.${ext}</span>
      <button onclick="removeExtension('${ext}')" class="text-purple-400 hover:text-red-400 transition-colors duration-200">
        <svg xmlns="http://www.w3.org/2000/svg" class="h-4 w-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12" />
        </svg>
      </button>
    </span>
  `).join('');
}

export function addExtension() {
  if (!dom.extensionInput) return;

  const ext = dom.extensionInput.value.trim().toLowerCase();
  if (ext && !state.extensionsList.includes(ext)) {
    state.extensionsList.push(ext);
    renderTags();
    dom.extensionInput.value = "";
    if (dom.extensionsStatusEl) {
      dom.extensionsStatusEl.textContent = "設定を保存してください";
    }
  }
}

export function removeExtension(ext: string) {
  state.extensionsList = state.extensionsList.filter((e) => e !== ext);
  renderTags();
  if (dom.extensionsStatusEl) {
    dom.extensionsStatusEl.textContent = "設定を保存してください";
  }
}

export async function loadExtensions() {
  try {
    const extensions = await invoke<string[]>("get_extensions");
    state.extensionsList = extensions;
    renderTags();
    if (dom.extensionsStatusEl) {
      dom.extensionsStatusEl.textContent = "設定を読み込みました";
    }
  } catch (error) {
    console.error("拡張子読み込みエラー:", error);
    if (dom.extensionsStatusEl) {
      dom.extensionsStatusEl.textContent = `設定の読み込みに失敗しました: ${error}`;
    }
  }
}

export async function saveExtensions() {
  try {
    await invoke("save_extensions", { extensions: state.extensionsList });
    if (dom.extensionsStatusEl) {
      dom.extensionsStatusEl.textContent = "設定を保存しました";
    }
  } catch (error) {
    console.error("拡張子保存エラー:", error);
    if (dom.extensionsStatusEl) {
      dom.extensionsStatusEl.textContent = `設定の保存に失敗しました: ${error}`;
    }
  }
}
