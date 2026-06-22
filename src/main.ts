// YamanohaRemoteDesktop - Frontend Entry Point

type ViewId = "home-view" | "host-view" | "connect-view" | "remote-view";

const DEFAULT_SERVER_URL = "ws://localhost:8080";

let clientWs: WebSocket | null = null;
let hostWs: WebSocket | null = null;

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

  try {
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke("start_host", { serverUrl });
  } catch (err) {
    console.error("Host start failed:", err);
    setConnectionStatus("エラー", "red");
    return;
  }

  hostWs = new WebSocket(serverUrl);

  hostWs.onopen = () => {
    hostWs?.send(JSON.stringify({ type: "host" }));
  };

  hostWs.onmessage = (event) => {
    let msg;
    try {
      msg = JSON.parse(event.data);
    } catch {
      return;
    }

    switch (msg.type) {
      case "host-ready":
        const codeEl = document.getElementById("connection-code");
        if (codeEl) codeEl.textContent = msg.code;
        break;

      case "client-connected":
        setConnectionStatus("クライアント接続済み", "green");
        break;

      case "disconnected":
        setConnectionStatus("クライアント切断", "yellow");
        break;
    }
  };

  hostWs.onerror = () => {
    setConnectionStatus("エラー", "red");
  };
});

document.getElementById("cancel-host-btn")?.addEventListener("click", async () => {
  if (hostWs) {
    hostWs.close();
    hostWs = null;
  }

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
  }
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
