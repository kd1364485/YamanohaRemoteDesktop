// YamanohaRemoteDesktop - Frontend Entry Point

import { HostInfo, DisplayInfo, DEFAULT_SERVER_URL } from "./types";
import { showView, setConnectionStatus, getServerUrl } from "./ui";
import { getAuthToken, getAuthUsername, clearAuth, initAuthUI } from "./auth";
import {
  setupWebRTC,
  handleWebRTCOffer,
  handleWebRTCIce,
  stopFreezeDetection,
} from "./webrtc";
import {
  clientWs,
  setClientWs,
  closeClientWs,
  closeListWs,
  setListWs,
  sendInput,
} from "./connection";
import { setupRemoteCanvas, detachRemoteKeyboard } from "./input";
import { setupTouchControls, detachTouchControls } from "./touch";

let remoteDisplays: DisplayInfo[] = [];
let currentDisplayId: string | null = null;

// ページ読み込み時にサーバーURL入力欄へ自動入力 + 認証状態チェック
window.addEventListener("DOMContentLoaded", () => {
  const input = document.getElementById("server-url") as HTMLInputElement | null;
  if (input && !input.value.trim()) {
    input.value = DEFAULT_SERVER_URL;
  }

  if (getAuthToken() && getAuthUsername()) {
    showView("home-view");
    const userEl = document.getElementById("logged-in-user");
    if (userEl) userEl.textContent = getAuthUsername();
  } else {
    showView("login-view");
  }

  initAuthUI();
});

// 画面切替ボタン
document.getElementById("show-register-btn")?.addEventListener("click", () => showView("register-view"));
document.getElementById("show-login-btn")?.addEventListener("click", () => showView("login-view"));

// ログアウト
document.getElementById("logout-btn")?.addEventListener("click", () => {
  clearAuth();
  closeClientWs();
  closeListWs();
  showView("login-view");
});

// --- Host mode ---

// ホストの状態を、画面に反映する(Rust の signaling::emit_status から届く)
type HostState = "waiting" | "asking" | "streaming" | "error";

function showHostState(state: HostState, user: string): void {
  const codeEl = document.getElementById("connection-code");
  const detailEl = document.getElementById("host-detail");
  const spinnerEl = document.getElementById("host-spinner");
  const streaming = state === "streaming";
  const text: Record<HostState, [string, string, string, string]> = {
    // [大きい表示, 説明, ヘッダーの表示, ヘッダーの色]
    waiting: ["待機中...", "接続を待っています...", "ホスト待機中", "yellow"],
    asking: ["許可待ち", `${user} さんが接続しようとしています。ダイアログで許可してください`, "許可待ち", "yellow"],
    streaming: ["配信中", `${user} さんに、この PC の画面を配信しています`, "配信中", "green"],
    error: ["エラー", "サーバーとの接続が切れました。キャンセルして、やり直してください", "エラー", "red"],
  };
  const [big, detail, header, color] = text[state];
  if (codeEl) {
    codeEl.textContent = big;
    codeEl.classList.toggle("text-green-400", streaming);
    codeEl.classList.toggle("text-blue-400", !streaming);
  }
  if (detailEl) detailEl.textContent = detail;
  if (spinnerEl) spinnerEl.classList.toggle("hidden", streaming);
  setConnectionStatus(header, color);
}

// ブラウザ(スマホ)で開いたときは Tauri がないので、登録しない
if ("__TAURI_INTERNALS__" in window) {
  import("@tauri-apps/api/event").then(({ listen }) => {
    listen<{ state: HostState; user: string }>("host-status", (event) => {
      showHostState(event.payload.state, event.payload.user);
    });
  });
}

document.getElementById("host-btn")?.addEventListener("click", async () => {
  if (!getAuthToken()) { showView("login-view"); return; }
  showView("host-view");
  showHostState("waiting", "");

  const serverUrl = getServerUrl();
  const deviceName = "PC";

  // ネイティブC++エンジンを起動（画面キャプチャ + エンコード + WebRTC配信）
  try {
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke("start_host", { serverUrl, token: getAuthToken(), deviceName });
  } catch (err) {
    console.error("Host start failed:", err);
    setConnectionStatus("エラー: エンジン起動失敗", "red");
  }
});

document.getElementById("cancel-host-btn")?.addEventListener("click", async () => {
  try {
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke("stop_host");
  } catch (err) {
    console.error("Stop host failed:", err);
  }

  showView("home-view");
  setConnectionStatus("未接続", "gray");
});

// --- Client mode ---

document.getElementById("connect-btn")?.addEventListener("click", () => {
  if (!getAuthToken()) { showView("login-view"); return; }
  showView("connect-view");
  fetchHostList();
});

document.getElementById("refresh-hosts-btn")?.addEventListener("click", () => {
  fetchHostList();
});

function fetchHostList(): void {
  const serverUrl = getServerUrl();
  const container = document.getElementById("host-list-container");
  if (!container) return;

  container.innerHTML = '<div class="text-center text-gray-400 py-4">読み込み中...</div>';

  closeListWs();

  const ws = new WebSocket(serverUrl);
  setListWs(ws);

  //2段階メッセージフロー[1] auth送信 → [2] auth-ok受信 → [3] list-hosts送信 → [4] host-list受信 → close
  ws.onopen = () => {
    ws.send(JSON.stringify({ type: "auth", token: getAuthToken() }));
  };

  ws.onmessage = (event) => {
    let msg;
    try { msg = JSON.parse(event.data); } catch { return; }

    if (msg.type === "auth-ok") {
      ws.send(JSON.stringify({ type: "list-hosts" }));
      return;
    }

    if (msg.type === "host-list") {
      renderHostList(msg.hosts as HostInfo[]);
      ws.close();
      setListWs(null);
      return;
    }

    if (msg.type === "error") {
      container.innerHTML = `<div class="text-center text-red-400 py-4">${msg.message}</div>`;
    }
  };

  ws.onerror = () => {
    container.innerHTML = '<div class="text-center text-red-400 py-4">サーバーに接続できません</div>';
  };
}

function renderHostList(hosts: HostInfo[]): void {
  const container = document.getElementById("host-list-container");
  if (!container) return;

  if (hosts.length === 0) {
    container.innerHTML = `
      <div class="text-center text-gray-400 py-8">
        <p>オンラインのホストがいません</p>
        <p class="text-sm mt-2">ホストPCで「このPCを共有」を起動してください</p>
      </div>`;
    return;
  }

  container.innerHTML = "";
  for (const h of hosts) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "w-full px-4 py-3 bg-gray-900 hover:bg-gray-800 border border-white/10 hover:border-cyan-400/50 rounded-lg flex items-center gap-3 transition-all";
    btn.innerHTML = `
      <div class="w-10 h-10 rounded-lg bg-cyan-500/10 flex items-center justify-center">
        <svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5 text-cyan-400" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="1.5">
          <path stroke-linecap="round" stroke-linejoin="round" d="M9 17.25v1.007a3 3 0 01-.879 2.122L7.5 21h9l-.621-.621A3 3 0 0115 18.257V17.25m6-12V15a2.25 2.25 0 01-2.25 2.25H5.25A2.25 2.25 0 013 15V5.25m18 0A2.25 2.25 0 0018.75 3H5.25A2.25 2.25 0 003 5.25m18 0V12a2.25 2.25 0 01-2.25 2.25H5.25A2.25 2.25 0 013 12V5.25" />
        </svg>
      </div>
      <div class="flex-1 text-left">
        <div class="text-gray-100 font-medium">${h.deviceName}</div>
        <div class="text-xs text-green-400 flex items-center gap-1">
          <span class="w-1.5 h-1.5 rounded-full bg-green-500"></span>オンライン
        </div>
      </div>
      <span class="text-cyan-400 text-sm">接続 →</span>
    `;
    btn.addEventListener("click", () => connectToHost(h.hostId));
    container.appendChild(btn);
  }
}

function connectToHost(hostId: string): void {
  if (!getAuthToken()) { showView("login-view"); return; }
  const serverUrl = getServerUrl();

  // 前回接続のディスプレイ情報をリセット
  remoteDisplays = [];
  currentDisplayId = null;

  setConnectionStatus("接続中...", "yellow");

  closeClientWs();
  const ws = new WebSocket(serverUrl);
  ws.binaryType = "arraybuffer";
  setClientWs(ws);

  //3段階メッセージフロー
  //[1] auth送信 → [2] auth-ok受信 → [3] connect送信 → [4] connected受信 → 画面共有開始
  ws.onopen = () => {
    ws.send(JSON.stringify({ type: "auth", token: getAuthToken() }));
  };

  ws.onmessage = (event) => {
    let msg;
    try {
      msg = JSON.parse(event.data);
    } catch {
      return;
    }

    if (msg.type === "auth-ok") {
      ws.send(JSON.stringify({ type: "connect", hostId }));
      return;
    }

    switch (msg.type) {
      case "connected":
        console.log("[client] connected message received");
        showView("remote-view");
        setConnectionStatus("接続済み", "green");
        setupRemoteCanvas(ws, setupTouchControls);
        setupWebRTC();
        break;

      case "display-list":
        remoteDisplays = (msg.displays as DisplayInfo[]) || [];
        currentDisplayId = msg.currentId || null;
        renderDisplaySwitcher();
        break;

      case "webrtc-offer":
        handleWebRTCOffer(msg.sdp);
        break;

      case "webrtc-ice":
        handleWebRTCIce(msg.candidate);
        break;

      case "disconnected":
        setConnectionStatus("切断されました", "red");
        showView("home-view");
        closeClientWs();
        break;

      case "error":
        setConnectionStatus("エラー: " + msg.message, "red");
        showView("home-view");
        closeClientWs();
        break;
    }
  };

  ws.onerror = () => {
    setConnectionStatus("接続エラー", "red");
  };

  ws.onclose = () => {
    detachRemoteKeyboard();
    detachTouchControls();
    stopFreezeDetection();
    if (!document.getElementById("remote-view")?.classList.contains("hidden")) {
      showView("home-view");
      setConnectionStatus("未接続", "gray");
    }
  };
}

document.getElementById("back-btn")?.addEventListener("click", () => {
  showView("home-view");
});

document.getElementById("disconnect-btn")?.addEventListener("click", () => {
  closeClientWs();
  showView("home-view");
  setConnectionStatus("未接続", "gray");
});

// --- ディスプレイ切替え ---

function renderDisplaySwitcher(): void {
  const bar = document.getElementById("display-switcher");
  if (!bar) return;
  bar.innerHTML = "";

  if (remoteDisplays.length <= 1) {
    bar.classList.add("hidden");
    return;
  }
  bar.classList.remove("hidden");

  for (const d of remoteDisplays) {
    const btn = document.createElement("button");
    btn.type = "button";
    const active = d.id === currentDisplayId;
    btn.className = `px-3 py-1.5 rounded-lg text-xs font-medium transition-colors ${
      active ? "bg-cyan-500 text-white" : "bg-gray-700 text-gray-300 hover:bg-gray-600"
    }`;
    btn.textContent = d.label;
    btn.addEventListener("click", () => selectDisplay(d.id));
    bar.appendChild(btn);
  }
}

function selectDisplay(displayId: string): void {
  const ws = clientWs;
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  currentDisplayId = displayId;
  ws.send(JSON.stringify({ type: "select-display", displayId }));
  renderDisplaySwitcher();
}

// 再エクスポート（他モジュールからの参照用）
export { sendInput };
