// YamanohaRemoteDesktop - Frontend Entry Point

type ViewId = "home-view" | "host-view" | "connect-view" | "remote-view";

// アクセス中のホスト名からサーバーURLを自動生成
// 例: スマホが http://10.17.41.5:1420 でアクセス → ws://10.17.41.5:8080
function buildDefaultServerUrl(): string {
  const hostname = window.location.hostname || "localhost";
  const protocol = window.location.protocol === "https:" ? "wss" : "ws";
  return `${protocol}://${hostname}:8080`;
}

const DEFAULT_SERVER_URL = buildDefaultServerUrl();

let clientWs: WebSocket | null = null;

function generateCode(): string {
  const chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  let code = "";
  for (let i = 0; i < 6; i++) {
    code += chars[Math.floor(Math.random() * chars.length)];
  }
  return code;
}

// ページ読み込み時にサーバーURL入力欄へ自動入力
window.addEventListener("DOMContentLoaded", () => {
  const input = document.getElementById("server-url") as HTMLInputElement | null;
  if (input && !input.value.trim()) {
    input.value = DEFAULT_SERVER_URL;
  }
});

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

function getServerUrl(): string {
  const input = document.getElementById("server-url") as HTMLInputElement | null;
  return input?.value?.trim() || DEFAULT_SERVER_URL;
}

// --- Host mode ---

document.getElementById("host-btn")?.addEventListener("click", async () => {
  showView("host-view");
  setConnectionStatus("ホスト待機中", "yellow");

  const serverUrl = getServerUrl();
  const code = generateCode();

  // 接続コードを表示
  const codeEl = document.getElementById("connection-code");
  if (codeEl) codeEl.textContent = code;

  // host.js プロセスを起動（画面キャプチャ + 入力制御）。コードを渡して登録させる。
  try {
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke("start_host", { serverUrl, code });
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
  showView("connect-view");
});

document.getElementById("connect-submit-btn")?.addEventListener("click", () => {
  const input = document.getElementById("code-input") as HTMLInputElement | null;
  if (!input || !input.value.trim()) return;

  const code = input.value.trim().toUpperCase();
  const serverUrl = getServerUrl();

  setConnectionStatus("接続中...", "yellow");

  clientWs = new WebSocket(serverUrl);

  clientWs.onopen = () => {
    clientWs?.send(JSON.stringify({ type: "connect", code }));
  };

  clientWs.onmessage = (event) => {
    let msg;
    try {
      msg = JSON.parse(event.data);
    } catch {
      return;
    }

    switch (msg.type) {
      case "connected":
        showView("remote-view");
        setConnectionStatus("接続済み", "green");
        setupRemoteCanvas();
        break;

      case "frame":
        drawFrame(msg.data);
        break;

      case "disconnected":
        setConnectionStatus("切断されました", "red");
        showView("home-view");
        if (clientWs) {
          clientWs.close();
          clientWs = null;
        }
        break;

      case "error":
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
});

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
