// YamanohaRemoteDesktop - Frontend Entry Point

type ViewId = "home-view" | "host-view" | "connect-view" | "remote-view";

function showView(viewId: ViewId): void {
  const views: ViewId[] = ["home-view", "host-view", "connect-view", "remote-view"];
  for (const id of views) {
    const el = document.getElementById(id);
    if (el) {
      el.classList.toggle("hidden", id !== viewId);
    }
  }
}

function setConnectionStatus(text: string, color: string): void {
  const status = document.getElementById("connection-status");
  if (!status) return;
  const dot = status.querySelector("span:first-child");
  const label = status.querySelector("span:last-child");
  if (dot) dot.className = `w-2 h-2 rounded-full bg-${color}-500`;
  if (label) label.textContent = text;
}

// Host button
document.getElementById("host-btn")?.addEventListener("click", async () => {
  showView("host-view");
  setConnectionStatus("ホスト待機中", "yellow");

  try {
    const { invoke } = await import("@tauri-apps/api/core");
    const code = await invoke<string>("start_host");
    const codeEl = document.getElementById("connection-code");
    if (codeEl) codeEl.textContent = code;
  } catch (err) {
    console.error("Host start failed:", err);
    setConnectionStatus("エラー", "red");
  }
});

// Cancel host
document.getElementById("cancel-host-btn")?.addEventListener("click", async () => {
  try {
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke("disconnect_session");
  } catch (err) {
    console.error("Disconnect failed:", err);
  }
  showView("home-view");
  setConnectionStatus("未接続", "gray");
});

// Connect button
document.getElementById("connect-btn")?.addEventListener("click", () => {
  showView("connect-view");
});

// Connect submit
document.getElementById("connect-submit-btn")?.addEventListener("click", async () => {
  const input = document.getElementById("code-input") as HTMLInputElement | null;
  if (!input || !input.value.trim()) return;

  setConnectionStatus("接続中...", "yellow");

  try {
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke("connect_to_host", { code: input.value.trim() });
    showView("remote-view");
    setConnectionStatus("接続済み", "green");
  } catch (err) {
    console.error("Connection failed:", err);
    setConnectionStatus("接続失敗", "red");
  }
});

// Back button
document.getElementById("back-btn")?.addEventListener("click", () => {
  showView("home-view");
});

// Disconnect
document.getElementById("disconnect-btn")?.addEventListener("click", async () => {
  try {
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke("disconnect_session");
  } catch (err) {
    console.error("Disconnect failed:", err);
  }
  showView("home-view");
  setConnectionStatus("未接続", "gray");
});
