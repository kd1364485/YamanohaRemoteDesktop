// アプリ内で使用するすべてのDOM要素参照を集約するオブジェクト。
// initDom() を DOMContentLoaded 後に1度呼び出して各要素を取得する。
// オブジェクトは参照渡しのため、各モジュールから dom.xxx で同じ要素を参照できる。
export const dom = {
  selectedPathEl: null as HTMLElement | null,
  selectFolderBtn: null as HTMLButtonElement | null,
  startWatchBtn: null as HTMLButtonElement | null,
  stopWatchBtn: null as HTMLButtonElement | null,
  buildIndexBtn: null as HTMLButtonElement | null,
  indexStatusEl: null as HTMLElement | null,
  watchStatusEl: null as HTMLElement | null,

  // モーダル関連
  settingsBtn: null as HTMLButtonElement | null,
  closeSettingsBtn: null as HTMLButtonElement | null,
  settingsModal: null as HTMLElement | null,

  // ステータスバー関連
  statusWatch: null as HTMLElement | null,
  statusIndex: null as HTMLElement | null,
  selectedPathStatus: null as HTMLElement | null,

  // 拡張子関連
  extensionInput: null as HTMLInputElement | null,
  addExtensionBtn: null as HTMLButtonElement | null,
  extensionsTagsEl: null as HTMLElement | null,
  loadExtensionsBtn: null as HTMLButtonElement | null,
  saveExtensionsBtn: null as HTMLButtonElement | null,
  extensionsStatusEl: null as HTMLElement | null,

  // チャット関連
  apiKeyInput: null as HTMLInputElement | null,
  modelSelect: null as HTMLSelectElement | null,
  chatInput: null as HTMLInputElement | null,
  sendChatBtn: null as HTMLButtonElement | null,
  chatMessagesEl: null as HTMLElement | null,
  chatStatusEl: null as HTMLElement | null,

  // プロンプト関連
  promptInput: null as HTMLTextAreaElement | null,
  loadPromptBtn: null as HTMLButtonElement | null,
  savePromptBtn: null as HTMLButtonElement | null,
  resetPromptBtn: null as HTMLButtonElement | null,
  promptStatusEl: null as HTMLElement | null,

  // トースト通知
  toastContainer: null as HTMLElement | null,
};

export function initDom() {
  dom.selectedPathEl = document.querySelector("#selected-path");
  dom.selectFolderBtn = document.querySelector("#select-folder-btn");
  dom.startWatchBtn = document.querySelector("#start-watch-btn");
  dom.stopWatchBtn = document.querySelector("#stop-watch-btn");
  dom.buildIndexBtn = document.querySelector("#build-index-btn");
  dom.indexStatusEl = document.querySelector("#index-status");
  dom.watchStatusEl = document.querySelector("#watch-status");

  // トースト通知
  dom.toastContainer = document.querySelector("#toast-container");

  // モーダル関連
  dom.settingsBtn = document.querySelector("#settings-btn");
  dom.closeSettingsBtn = document.querySelector("#close-settings-btn");
  dom.settingsModal = document.querySelector("#settings-modal");

  // ステータスバー関連
  dom.statusWatch = document.querySelector("#status-watch");
  dom.statusIndex = document.querySelector("#status-index");
  dom.selectedPathStatus = document.querySelector("#selected-path-status");

  // 拡張子関連
  dom.extensionInput = document.querySelector("#extension-input");
  dom.addExtensionBtn = document.querySelector("#add-extension-btn");
  dom.extensionsTagsEl = document.querySelector("#extensions-tags");
  dom.loadExtensionsBtn = document.querySelector("#load-extensions-btn");
  dom.saveExtensionsBtn = document.querySelector("#save-extensions-btn");
  dom.extensionsStatusEl = document.querySelector("#extensions-status");

  // プロンプト関連
  dom.promptInput = document.querySelector("#prompt-input") as HTMLTextAreaElement;
  dom.loadPromptBtn = document.querySelector("#load-prompt-btn");
  dom.savePromptBtn = document.querySelector("#save-prompt-btn");
  dom.resetPromptBtn = document.querySelector("#reset-prompt-btn");
  dom.promptStatusEl = document.querySelector("#prompt-status");

  // チャット関連
  dom.apiKeyInput = document.querySelector("#api-key-input");
  dom.modelSelect = document.querySelector("#model-select");
  dom.chatInput = document.querySelector("#chat-input");
  dom.sendChatBtn = document.querySelector("#send-chat-btn");
  dom.chatMessagesEl = document.querySelector("#chat-messages");
  dom.chatStatusEl = document.querySelector("#chat-status");
}
