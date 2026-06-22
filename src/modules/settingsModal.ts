import { dom } from "./dom";

export function openSettingsModal() {
  if (dom.settingsModal) {
    dom.settingsModal.classList.remove("hidden");
  }
}

export function closeSettingsModal() {
  if (dom.settingsModal) {
    dom.settingsModal.classList.add("hidden");
  }
}
