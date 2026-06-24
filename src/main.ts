// YamanohaRemoteDesktop - Frontend Entry Point

type ViewId = "login-view" | "register-view" | "home-view" | "host-view" | "connect-view" | "remote-view";

type HostInfo = { hostId: string; deviceName: string };

let authToken: string | null = localStorage.getItem("authToken");
let authUsername: string | null = localStorage.getItem("authUsername");
let listWs: WebSocket | null = null;

// アクセス中のホスト名からサーバーURLを自動生成
// 例: スマホが http://10.17.41.5:1420 でアクセス → ws://10.17.41.5:8080
function buildDefaultServerUrl(): string {
  const hostname = window.location.hostname || "localhost";
  const protocol = window.location.protocol === "https:" ? "wss" : "ws";
  return `${protocol}://${hostname}:8080`;
}

const DEFAULT_SERVER_URL = buildDefaultServerUrl();

let clientWs: WebSocket | null = null;

// ページ読み込み時にサーバーURL入力欄へ自動入力 + 認証状態チェック
window.addEventListener("DOMContentLoaded", () => {
  const input = document.getElementById("server-url") as HTMLInputElement | null;
  if (input && !input.value.trim()) {
    input.value = DEFAULT_SERVER_URL;
  }

  if (authToken && authUsername) {
    showView("home-view");
    const userEl = document.getElementById("logged-in-user");
    if (userEl) userEl.textContent = authUsername;
  } else {
    showView("login-view");
  }
});

// --- 認証機能 ---

function getHttpBaseUrl(): string {
  const hostname = window.location.hostname || "localhost";
  const protocol = window.location.protocol === "https:" ? "https" : "http";
  return `${protocol}://${hostname}:8080`;
}

async function apiRequest(endpoint: string, username: string, password: string): Promise<{ token?: string; username?: string; error?: string }> {
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

function saveAuth(token: string, username: string): void {
  authToken = token;
  authUsername = username;
  localStorage.setItem("authToken", token);
  localStorage.setItem("authUsername", username);
}

function clearAuth(): void {
  authToken = null;
  authUsername = null;
  localStorage.removeItem("authToken");
  localStorage.removeItem("authUsername");
}

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

// 画面切替ボタン
document.getElementById("show-register-btn")?.addEventListener("click", () => showView("register-view"));
document.getElementById("show-login-btn")?.addEventListener("click", () => showView("login-view"));

// ログアウト
document.getElementById("logout-btn")?.addEventListener("click", () => {
  clearAuth();
  if (clientWs) { clientWs.close(); clientWs = null; }
  if (listWs) { listWs.close(); listWs = null; }
  showView("login-view");
});

function showView(viewId: ViewId): void {
  const views: ViewId[] = ["login-view", "register-view", "home-view", "host-view", "connect-view", "remote-view"];
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

function getServerUrl(): string {
  const input = document.getElementById("server-url") as HTMLInputElement | null;
  return input?.value?.trim() || DEFAULT_SERVER_URL;
}

// --- Host mode ---

document.getElementById("host-btn")?.addEventListener("click", async () => {
  if (!authToken) { showView("login-view"); return; }
  showView("host-view");
  setConnectionStatus("ホスト待機中", "yellow");

  const serverUrl = getServerUrl();
  const deviceName = "PC";

  // 接続コード表示エリアを待機中表示に変更
  const codeEl = document.getElementById("connection-code");
  if (codeEl) codeEl.textContent = "待機中...";

  // host.js プロセスを起動（画面キャプチャ + 入力制御）。トークンを渡して登録させる。
  try {
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke("start_host", { serverUrl, token: authToken, deviceName });
  } catch (err) {
    console.error("Host start failed:", err);
    setConnectionStatus("エラー: host.js起動失敗", "red");
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
  if (!authToken) { showView("login-view"); return; }
  showView("connect-view");
  fetchHostList();//ホスト一覧
});

document.getElementById("refresh-hosts-btn")?.addEventListener("click", () => {
  fetchHostList();
});

function fetchHostList(): void {
  const serverUrl = getServerUrl();
  const container = document.getElementById("host-list-container");
  if (!container) return;

  // 既存のリストをクリア
  container.innerHTML = '<div class="text-center text-gray-400 py-4">読み込み中...</div>';

  if (listWs) { listWs.close(); listWs = null; }

  listWs = new WebSocket(serverUrl);


  //2段階メッセージフロー[1] auth送信 → [2] auth-ok受信 → [3] list-hosts送信 → [4] host-list受信 → close
  listWs.onopen = () => {
    listWs?.send(JSON.stringify({ type: "auth", token: authToken }));//ステップ1
  };

  listWs.onmessage = (event) => {
    let msg;
    try { msg = JSON.parse(event.data); } catch { return; }

    if (msg.type === "auth-ok") {
      listWs?.send(JSON.stringify({ type: "list-hosts" }));//ステップ3
      return;
    }

    if (msg.type === "host-list") {
      renderHostList(msg.hosts as HostInfo[]);//ステップ4
      listWs?.close();
      listWs = null;
      return;
    }

    if (msg.type === "error") {
      container.innerHTML = `<div class="text-center text-red-400 py-4">${msg.message}</div>`;
    }
  };

  listWs.onerror = () => {
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
  if (!authToken) { showView("login-view"); return; }
  const serverUrl = getServerUrl();

  setConnectionStatus("接続中...", "yellow");

  clientWs = new WebSocket(serverUrl);

  //3段階メッセージフロー
  //[1] auth送信 → [2] auth-ok受信 → [3] connect送信 → [4] connected受信 → 画面共有開始
  clientWs.onopen = () => {
    clientWs?.send(JSON.stringify({ type: "auth", token: authToken }));//ステップ1
  };

  clientWs.onmessage = (event) => {
    let msg;
    try {
      msg = JSON.parse(event.data);
    } catch {
      return;
    }

    // 認証成功後、接続要求を送信
    if (msg.type === "auth-ok") {
      clientWs?.send(JSON.stringify({ type: "connect", hostId }));//ステップ3
      return;
    }

    switch (msg.type) {
      case "connected"://ステップ4
        showView("remote-view");
        setConnectionStatus("接続済み", "green");
        setupRemoteCanvas();//canvasの初期化 + 入力イベント登録
        break;

      case "frame"://5画面受信(継続)
        drawFrame(msg.data);
        break;

      case "disconnected"://切断通知
        setConnectionStatus("切断されました", "red");
        showView("home-view");
        if (clientWs) {
          clientWs.close();
          clientWs = null;
        }
        break;

      case "error"://エラー
        setConnectionStatus("エラー: " + msg.message, "red");
        showView("home-view");
        if (clientWs) {
          clientWs.close();
          clientWs = null;
        }
        break;
    }
  };

  clientWs.onerror = () => {
    setConnectionStatus("接続エラー", "red");
  };

  clientWs.onclose = () => {
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
  if (clientWs) {
    clientWs.close();
    clientWs = null;
  }
  showView("home-view");
  setConnectionStatus("未接続", "gray");
});

// --- Remote canvas & input handling ---

function setupRemoteCanvas(): void {
  const canvas = document.getElementById("remote-canvas") as HTMLCanvasElement | null;
  const placeholder = document.getElementById("remote-placeholder");
  if (placeholder) placeholder.style.display = "none";
  if (canvas) {
    canvas.style.display = "block";
    canvas.width = 1920;
    canvas.height = 1080;

    canvas.onmousemove = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      const rect = canvas.getBoundingClientRect();
      const x = Math.round((e.clientX - rect.left) / rect.width * canvas.width);
      const y = Math.round((e.clientY - rect.top) / rect.height * canvas.height);
      clientWs.send(JSON.stringify({ type: "input", action: "mouse-move", x, y }));
    };

    canvas.onmousedown = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      const rect = canvas.getBoundingClientRect();
      const x = Math.round((e.clientX - rect.left) / rect.width * canvas.width);
      const y = Math.round((e.clientY - rect.top) / rect.height * canvas.height);
      const button = e.button === 2 ? "right" : "left";
      clientWs.send(JSON.stringify({ type: "input", action: "mouse-down", button, x, y }));
    };

    canvas.onmouseup = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      const button = e.button === 2 ? "right" : "left";
      clientWs.send(JSON.stringify({ type: "input", action: "mouse-up", button }));
    };

    canvas.oncontextmenu = (e) => e.preventDefault();

    canvas.onwheel = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      clientWs.send(JSON.stringify({ type: "input", action: "mouse-scroll", deltaY: e.deltaY }));
      e.preventDefault();
    };

    canvas.onkeydown = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      clientWs.send(JSON.stringify({ type: "input", action: "key", key: e.key, isDown: true }));
      if (["Tab", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Backspace"].includes(e.key)) {
        e.preventDefault();
      }
    };

    canvas.onkeyup = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      clientWs.send(JSON.stringify({ type: "input", action: "key", key: e.key, isDown: false }));
    };

    canvas.tabIndex = 0;

    setupTouchControls(canvas);
  }
}

// --- スマホ向けタッチ操作（トラックパッドモード） ---
// 指のドラッグ → カーソルを相対移動 / タップ → クリック
// 2本指ドラッグ → スクロール / 2本指タップ → 右クリック
function setupTouchControls(canvas: HTMLCanvasElement): void {
  // ブラウザのスクロール・ズーム・プルダウン更新を無効化
  canvas.style.touchAction = "none";
  (canvas.style as any).overscrollBehavior = "none";

  const MOVE_SENSITIVITY = 2.0; // カーソル移動の感度
  const TAP_THRESHOLD_PX = 10;  // この距離以内ならタップとみなす
  const TAP_THRESHOLD_MS = 250; // この時間以内ならタップとみなす
  const DOUBLE_TAP_MS = 300;    // 前回タップからこの時間以内なら2回目のタップとみなす

  let lastX = 0;
  let lastY = 0;
  let startX = 0;
  let startY = 0;
  let startTime = 0;
  let moved = false;
  let touchCount = 0;
  let lastScrollY = 0;

  let lastTapTime = 0;     // 直前にタップした時刻（ダブルタップ判定用）
  let dragCandidate = false; // 2回目のタッチ（ダブルタップ起点）かどうか
  let isDragging = false;    // 範囲選択中（左ボタン押しっぱなし）かどうか

  const send = (obj: object) => {
    if (clientWs && clientWs.readyState === WebSocket.OPEN) {
      clientWs.send(JSON.stringify(obj));
    }
  };

  canvas.addEventListener("touchstart", (e) => {
    e.preventDefault();
    touchCount = e.touches.length;
    const t = e.touches[0];
    lastX = t.clientX;
    lastY = t.clientY;
    startX = t.clientX;
    startY = t.clientY;
    startTime = Date.now();
    moved = false;

    // 1本指タッチで、直前のタップから短時間以内なら「ダブルタップ起点」とみなす
    dragCandidate = e.touches.length === 1 && (startTime - lastTapTime) < DOUBLE_TAP_MS;

    if (e.touches.length === 2) {
      lastScrollY = (e.touches[0].clientY + e.touches[1].clientY) / 2;
    }
  }, { passive: false });

  canvas.addEventListener("touchmove", (e) => {
    e.preventDefault();

    if (e.touches.length === 2) {
      // 2本指 → スクロール
      const midY = (e.touches[0].clientY + e.touches[1].clientY) / 2;
      const dy = midY - lastScrollY;
      lastScrollY = midY;
      if (Math.abs(dy) > 1) {
        send({ type: "input", action: "mouse-scroll", deltaY: -dy * 3 });
        moved = true;
      }
      return;
    }

    // 1本指 → カーソル相対移動
    const t = e.touches[0];
    const dx = (t.clientX - lastX) * MOVE_SENSITIVITY;
    const dy = (t.clientY - lastY) * MOVE_SENSITIVITY;
    lastX = t.clientX;
    lastY = t.clientY;

    const distFromStart = Math.hypot(t.clientX - startX, t.clientY - startY);
    if (distFromStart > TAP_THRESHOLD_PX) moved = true;

    // ダブルタップ起点で動かし始めたら → 左ボタンを押して範囲選択モードへ
    if (dragCandidate && moved && !isDragging) {
      isDragging = true;
      send({ type: "input", action: "mouse-down", button: "left" });
    }

    if (dx !== 0 || dy !== 0) {
      send({ type: "input", action: "mouse-move-rel", dx: Math.round(dx), dy: Math.round(dy) });
    }
  }, { passive: false });

  canvas.addEventListener("touchend", (e) => {
    e.preventDefault();
    const elapsed = Date.now() - startTime;

    if (isDragging) {
      // 範囲選択終了 → 左ボタンを離す
      send({ type: "input", action: "mouse-up", button: "left" });
      isDragging = false;
      lastTapTime = 0;
      dragCandidate = false;
      touchCount = 0;
      return;
    }

    // 短時間 & ほぼ動いていない → タップ
    if (!moved && elapsed < TAP_THRESHOLD_MS) {
      if (touchCount >= 2) {
        // 2本指タップ → 右クリック
        send({ type: "input", action: "mouse-click", button: "right" });
        lastTapTime = 0;
      } else if (dragCandidate) {
        // ダブルタップ（動かず離した）→ ダブルクリック
        send({ type: "input", action: "mouse-double-click", button: "left" });
        lastTapTime = 0;
      } else {
        // 単一タップ → 左クリック。次のタップとの間隔を測るため時刻を記録
        send({ type: "input", action: "mouse-click", button: "left" });
        lastTapTime = Date.now();
      }
    } else {
      lastTapTime = 0;
    }
    dragCandidate = false;
    touchCount = 0;
  }, { passive: false });
}

function drawFrame(base64Data: string): void {
  const canvas = document.getElementById("remote-canvas") as HTMLCanvasElement | null;
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;

  const img = new Image();
  img.onload = () => {
    if (canvas.width !== img.width || canvas.height !== img.height) {
      canvas.width = img.width;
      canvas.height = img.height;
    }
    ctx.drawImage(img, 0, 0);
  };
  img.src = `data:image/jpeg;base64,${base64Data}`;
}
