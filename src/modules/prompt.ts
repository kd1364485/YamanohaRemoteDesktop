import { DEFAULT_PROMPT } from "./constants";
import { dom } from "./dom";
import { state } from "./state";
import { loadPrompt, savePrompt } from "./storage";

export function loadPromptSettings() {
  const savedPrompt = loadPrompt();
  if (savedPrompt && dom.promptInput) {
    dom.promptInput.value = savedPrompt;
    state.savedPromptText = savedPrompt;
  } else if (dom.promptInput) {
    dom.promptInput.value = DEFAULT_PROMPT;
    state.savedPromptText = DEFAULT_PROMPT;
  }
  if (dom.promptStatusEl) {
    dom.promptStatusEl.textContent = "";
  }
}

export function checkPromptChanges() {
  if (!dom.promptInput || !dom.promptStatusEl) return;
  if (dom.promptInput.value !== state.savedPromptText) {
    dom.promptStatusEl.textContent = "⚠️ 変更があります。保存を忘れずに行ってください";
  } else {
    dom.promptStatusEl.textContent = "";
  }
}

export function savePromptSettings() {
  if (dom.promptInput) {
    savePrompt(dom.promptInput.value);
    state.savedPromptText = dom.promptInput.value;
    if (dom.promptStatusEl) {
      dom.promptStatusEl.textContent = "プロンプトを保存しました ✅";
      setTimeout(() => {
        if (dom.promptStatusEl) {
          dom.promptStatusEl.textContent = "";
        }
      }, 2000);
    }
  }
}

export function resetPromptSettings() {
  if (dom.promptInput) {
    dom.promptInput.value = DEFAULT_PROMPT;
    if (dom.promptStatusEl) {
      dom.promptStatusEl.textContent = "デフォルトプロンプトに戻しました ✅";
      setTimeout(() => {
        if (dom.promptStatusEl) {
          dom.promptStatusEl.textContent = "";
        }
      }, 2000);
    }
  }
}
