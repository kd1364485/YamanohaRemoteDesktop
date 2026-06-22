import { API_KEY_STORAGE_KEY, PROMPT_STORAGE_KEY } from "./constants";

export function saveApiKey(key: string) {
  localStorage.setItem(API_KEY_STORAGE_KEY, key);
}

export function loadApiKey(): string | null {
  return localStorage.getItem(API_KEY_STORAGE_KEY);
}

export function savePrompt(prompt: string) {
  localStorage.setItem(PROMPT_STORAGE_KEY, prompt);
}

export function loadPrompt(): string | null {
  return localStorage.getItem(PROMPT_STORAGE_KEY);
}
