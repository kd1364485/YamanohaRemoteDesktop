// 認証機能

import { showView, getHttpBaseUrl } from "./ui";

export let authToken: string | null = localStorage.getItem("authToken");
export let authUsername: string | null = localStorage.getItem("authUsername");

export function getAuthToken(): string | null {
  return authToken;
}

export function getAuthUsername(): string | null {
  return authUsername;
}

export function saveAuth(token: string, username: string): void {
  authToken = token;
  authUsername = username;
  localStorage.setItem("authToken", token);
  localStorage.setItem("authUsername", username);
}

export function clearAuth(): void {
  authToken = null;
  authUsername = null;
  localStorage.removeItem("authToken");
  localStorage.removeItem("authUsername");
}

export async function apiRequest(
  endpoint: string,
  username: string,
  password: string,
): Promise<{ token?: string; username?: string; error?: string }> {
  try {
    const res = await fetch(`${getHttpBaseUrl()}${endpoint}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username, password }),
    });
    const data = await res.json();
    if (!res.ok) return { error: data.error || "エラーが発生しました" };
    return data;
  } catch {
    return { error: "サーバーに接続できません" };
  }
}

export function initAuthUI(): void {
  // ログインボタン
  document.getElementById("login-btn")?.addEventListener("click", async () => {
    const username = (document.getElementById("login-username") as HTMLInputElement)?.value?.trim();
    const password = (document.getElementById("login-password") as HTMLInputElement)?.value;
    const errorEl = document.getElementById("login-error");

    if (!username || !password) {
      if (errorEl) { errorEl.textContent = "ユーザー名とパスワードを入力してください"; errorEl.classList.remove("hidden"); }
      return;
    }

    const result = await apiRequest("/api/login", username, password);
    if (result.error) {
      if (errorEl) { errorEl.textContent = result.error; errorEl.classList.remove("hidden"); }
      return;
    }

    if (errorEl) errorEl.classList.add("hidden");
    saveAuth(result.token!, result.username!);
    const userEl = document.getElementById("logged-in-user");
    if (userEl) userEl.textContent = result.username!;
    showView("home-view");
  });

  // 新規登録ボタン
  document.getElementById("register-btn")?.addEventListener("click", async () => {
    const username = (document.getElementById("register-username") as HTMLInputElement)?.value?.trim();
    const password = (document.getElementById("register-password") as HTMLInputElement)?.value;
    const errorEl = document.getElementById("register-error");

    if (!username || !password) {
      if (errorEl) { errorEl.textContent = "ユーザー名とパスワードを入力してください"; errorEl.classList.remove("hidden"); }
      return;
    }

    const result = await apiRequest("/api/register", username, password);
    if (result.error) {
      if (errorEl) { errorEl.textContent = result.error; errorEl.classList.remove("hidden"); }
      return;
    }

    if (errorEl) errorEl.classList.add("hidden");
    saveAuth(result.token!, result.username!);
    const userEl = document.getElementById("logged-in-user");
    if (userEl) userEl.textContent = result.username!;
    showView("home-view");
  });
}
