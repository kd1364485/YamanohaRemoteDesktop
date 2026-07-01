// YamanohaRemoteDesktop - Frontend Entry Point

type ViewId = "login-view" | "register-view" | "home-view" | "host-view" | "connect-view" | "remote-view";

type HostInfo = { hostId: string; deviceName: string };

let authToken: string | null = localStorage.getItem("authToken");
let authUsername: string | null = localStorage.getItem("authUsername");
let listWs: WebSocket | null = null;

type DisplayInfo = { id: string; label: string; width: number; height: number; primary: boolean };
let remoteDisplays: DisplayInfo[] = [];
let currentDisplayId: string | null = null;

// クラウド（fly.io）にデプロイしたシグナリングサーバーのホスト名。
// `fly launch` で割り当てられた実際のアプリ名に合わせて変更してください。
// 例: アプリ名が "yamanoha-signaling" なら "yamanoha-signaling.fly.dev"
// 空文字 "" にするとローカル（同じWi-Fi）モードに戻ります。
const CLOUD_HOST = "yamanoha-signaling.fly.dev";

// サーバーURLを生成（WebSocket用）
// CLOUD_HOST が設定されていればクラウド（wss://...）を使い、
// 空ならアクセス中のホスト名からローカルURL（ws://10.17.41.5:8080）を生成
function buildDefaultServerUrl(): string {
  if (CLOUD_HOST) {
    return `wss://${CLOUD_HOST}`;
  }
  const hostname = window.location.hostname || "localhost";
  const protocol = window.location.protocol === "https:" ? "wss" : "ws";
  return `${protocol}://${hostname}:8080`;
}

const DEFAULT_SERVER_URL = buildDefaultServerUrl();

let clientWs: WebSocket | null = null;

// WebRTC関連
let pc: RTCPeerConnection | null = null;
let remoteStream: MediaStream | null = null;
let currentCursor: { x: number; y: number } | null = null;
// remoteDescriptionセット前に到着したICE candidateを一時保持するバッファ
let pendingIceCandidates: RTCIceCandidateInit[] = [];
// 入力/カーソル用DataChannel（P2P直送）。ホストが作成しondatachannelで受信する。
let inputChannel: RTCDataChannel | null = null;
// 受信側ジッターバッファの目標(ミリ秒)。超低遅延優先に設定。
const JITTER_BUFFER_TARGET_MS = 30;

// 入力をホストへ送信する。DataChannel(P2P直送)優先、未接続時はWebSocketリレーにフォールバック。
function sendInput(obj: object): void {
  const payload = JSON.stringify(obj);
  if (inputChannel && inputChannel.readyState === "open") {
    try { inputChannel.send(payload); return; } catch {}
  }
  if (clientWs && clientWs.readyState === WebSocket.OPEN) {
    clientWs.send(payload);
  }
}

// WebRTC接続をセットアップ
async function setupWebRTC(): Promise<void> {
  console.log("[client] setupWebRTC called");
  // 既存の接続があればクリーンアップ
  if (pc) {
    try { pc.close(); } catch {}
    pc = null;
  }
  remoteStream = null;
  inputChannel = null;
  // ICE candidateバッファをリセット
  pendingIceCandidates = [];

  // RTCPeerConnection作成
  pc = new RTCPeerConnection({
    iceServers: [{ urls: "stun:stun.l.google.com:19302" }],
  });
  console.log("[client] RTCPeerConnection created");

  // ホストが作成した入力DataChannelを受信。controlは旧ネイティブ版との互換用。
  pc.ondatachannel = (event) => {
    if (event.channel.label !== "input" && event.channel.label !== "control") return;
    const channel = event.channel;
    console.log(`[client] input DataChannel received: ${channel.label}`);
    inputChannel = channel;
    channel.onopen = () => {
      console.log(`[client] input DataChannel open: ${channel.label}`);
    };
    channel.onclose = () => {
      console.log(`[client] input DataChannel closed: ${channel.label}`);
      if (inputChannel === channel) inputChannel = null;
    };
    channel.onerror = (error) => {
      console.error("[client] input DataChannel error:", error);
    };
    // ホストからのカーソル位置通知をDataChannelで受信
    channel.onmessage = (e) => {
      try {
        const msg = JSON.parse(e.data);
        if (msg.type === "cursor") {
          currentCursor = { x: msg.x, y: msg.y };
          drawCursorOverlay();
        }
      } catch {}
    };
  };

  // Trackイベント: リモートストリームを受信
  pc.ontrack = (event) => {
    console.log("Received remote track:", event.track.kind);
    remoteStream = event.streams[0];

    // 受信側ジッターバッファを軽めに設定して再生遅延を抑える。
    // 0にするとモバイル回線のパケット並べ替え/遅延でデコード失敗→フリーズを誘発するため、
    // 完全な無効化は避け、控えめな目標値(既定120ms)に留める。
    try {
      const receiver = event.receiver as RTCRtpReceiver & {
        jitterBufferTarget?: number | null;
        playoutDelayHint?: number | null;
      };
      // 新しいAPI: jitterBufferTarget（ミリ秒）
      if ("jitterBufferTarget" in receiver) {
        receiver.jitterBufferTarget = JITTER_BUFFER_TARGET_MS;
      }
      // 旧API: playoutDelayHint（秒）
      if ("playoutDelayHint" in receiver) {
        receiver.playoutDelayHint = JITTER_BUFFER_TARGET_MS / 1000;
      }
    } catch (err) {
      console.warn("jitterBufferTarget setting failed:", err);
    }

    const video = document.getElementById("remote-video") as HTMLVideoElement | null;
    if (video && remoteStream) {
      video.srcObject = remoteStream;
      video.play().catch((err) => console.error("Video play error:", err));
    }
  };

  // ICE candidateイベント
  pc.onicecandidate = (event) => {
    if (event.candidate && clientWs && clientWs.readyState === WebSocket.OPEN) {
      clientWs.send(JSON.stringify({
        type: "webrtc-ice",
        candidate: event.candidate.toJSON(),
      }));
    }
  };

  console.log("WebRTC setup complete");
}

// WebRTC Offer受信処理
async function handleWebRTCOffer(sdp: string): Promise<void> {
  // 既存の接続があればクリーンアップ（ディスプレイ切り替え時など）
  if (pc) {
    try { pc.close(); } catch {}
    pc = null;
    remoteStream = null;
  }

  // 新しい接続を作成
  await setupWebRTC();
  if (!pc) return;

  try {
    await (pc as RTCPeerConnection).setRemoteDescription({ type: "offer", sdp });
    console.log("[client] remoteDescription set");

    // remoteDescriptionセット前に到着してバッファリングしたICE candidateをフラッシュ
    if (pendingIceCandidates.length > 0) {
      console.log(`[client] Flushing ${pendingIceCandidates.length} buffered ICE candidates`);
      for (const c of pendingIceCandidates) {
        try {
          await (pc as RTCPeerConnection).addIceCandidate(c);
        } catch (err) {
          console.error("WebRTC addIceCandidate (buffered) error:", err);
        }
      }
      pendingIceCandidates = [];
    }

    const answer = await (pc as RTCPeerConnection).createAnswer();
    await (pc as RTCPeerConnection).setLocalDescription(answer);
    if (clientWs && clientWs.readyState === WebSocket.OPEN) {
      clientWs.send(JSON.stringify({
        type: "webrtc-answer",
        sdp: (pc as RTCPeerConnection).localDescription?.sdp,
      }));
    }
    console.log("WebRTC answer sent");
  } catch (err) {
    console.error("WebRTC offer handling error:", err);
  }
}

// WebRTC ICE candidate受信処理
async function handleWebRTCIce(candidate: RTCIceCandidateInit): Promise<void> {
  if (!pc) return;
  // remoteDescriptionがまだセットされていない場合はバッファに貯めて後でフラッシュ
  if (!pc.remoteDescription) {
    pendingIceCandidates.push(candidate);
    console.log(`[client] ICE candidate buffered (total: ${pendingIceCandidates.length})`);
    return;
  }
  try {
    await pc.addIceCandidate(candidate);
  } catch (err) {
    console.error("WebRTC addIceCandidate error:", err);
  }
}

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
  if (CLOUD_HOST) {
    return `https://${CLOUD_HOST}`;
  }
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

  // 前回接続のディスプレイ情報をリセット
  remoteDisplays = [];
  currentDisplayId = null;

  setConnectionStatus("接続中...", "yellow");

  clientWs = new WebSocket(serverUrl);
  // 映像フレームはバイナリで受け取る（base64より軽量・高速デコード）
  clientWs.binaryType = "arraybuffer";

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
        console.log("[client] connected message received");
        showView("remote-view");
        setConnectionStatus("接続済み", "green");
        setupRemoteCanvas();//canvasの初期化 + 入力イベント登録
        setupWebRTC();
        break;

      case "display-list"://ホストのディスプレイ一覧を受信
        remoteDisplays = (msg.displays as DisplayInfo[]) || [];
        currentDisplayId = msg.currentId || null;
        renderDisplaySwitcher();
        break;

      case "cursor"://ホストからのカーソル位置通知
        currentCursor = { x: msg.x, y: msg.y };
        drawCursorOverlay();
        break;

      case "webrtc-offer":
        handleWebRTCOffer(msg.sdp);
        break;

      case "webrtc-ice":
        handleWebRTCIce(msg.candidate);
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
    // キーボード捕捉を解除（押しっぱなしキーも解放）
    detachRemoteKeyboard();
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

// --- ディスプレイ切替え ---

// ホストのディスプレイ一覧をボタンとして表示（複数ある場合のみ）
function renderDisplaySwitcher(): void {
  const bar = document.getElementById("display-switcher");
  if (!bar) return;
  bar.innerHTML = "";

  // ディスプレイが1つ以下なら切替え不要なので非表示
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

// 表示するディスプレイを切り替える
function selectDisplay(displayId: string): void {
  if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
  currentDisplayId = displayId;
  clientWs.send(JSON.stringify({ type: "select-display", displayId }));
  renderDisplaySwitcher();
}

// --- Remote canvas & input handling ---

// タッチ操作直後の合成マウスイベントを無視するためのタイマー
let lastTouchTime = 0;
const TOUCH_EVENT_COOLDOWN_MS = 500;

function setupRemoteCanvas(): void {
  const video = document.getElementById("remote-video") as HTMLVideoElement | null;
  const canvas = document.getElementById("remote-canvas") as HTMLCanvasElement | null;
  const placeholder = document.getElementById("remote-placeholder");
  if (placeholder) placeholder.style.display = "none";
  if (video) {
    video.style.display = "block";
    if (canvas) canvas.style.display = "block";

    video.onmousemove = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      if (Date.now() - lastTouchTime < TOUCH_EVENT_COOLDOWN_MS) return;
      const rect = video.getBoundingClientRect();
      // object-contain を考慮して映像領域内の正規化座標を計算
      const { nx, ny } = videoToNormalized(video, rect, e.clientX, e.clientY);
      sendInput({ type: "input", action: "mouse-move", nx, ny });
    };

    video.onmousedown = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      if (Date.now() - lastTouchTime < TOUCH_EVENT_COOLDOWN_MS) { e.preventDefault(); return; }
      const rect = video.getBoundingClientRect();
      const { nx, ny } = videoToNormalized(video, rect, e.clientX, e.clientY);
      const button = e.button === 2 ? "right" : "left";
      sendInput({ type: "input", action: "mouse-down", button, nx, ny });
    };

    video.onmouseup = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      if (Date.now() - lastTouchTime < TOUCH_EVENT_COOLDOWN_MS) { e.preventDefault(); return; }
      const button = e.button === 2 ? "right" : "left";
      sendInput({ type: "input", action: "mouse-up", button });
    };

    video.onclick = (e) => {
      // タッチ直後の合成クリックイベントを無視
      if (Date.now() - lastTouchTime < TOUCH_EVENT_COOLDOWN_MS) { e.preventDefault(); return; }
    };

    video.oncontextmenu = (e) => e.preventDefault();

    video.onwheel = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      sendInput({ type: "input", action: "mouse-scroll", deltaY: e.deltaY });
      e.preventDefault();
    };

    video.tabIndex = 0;
    video.focus();
    attachRemoteKeyboard();

    setupTouchControls(video);
  }
}

// video要素のobject-contain描画領域を考慮して、マウス座標を0..1の正規化座標に変換
function videoToNormalized(video: HTMLVideoElement, rect: DOMRect, clientX: number, clientY: number): { nx: number; ny: number } {
  if (!video.videoWidth || !video.videoHeight) {
    return { nx: (clientX - rect.left) / rect.width, ny: (clientY - rect.top) / rect.height };
  }
  const videoAspect = video.videoWidth / video.videoHeight;
  const rectAspect = rect.width / rect.height;
  let renderW: number, renderH: number, offsetX: number, offsetY: number;
  if (videoAspect > rectAspect) {
    renderW = rect.width;
    renderH = rect.width / videoAspect;
    offsetX = rect.left;
    offsetY = rect.top + (rect.height - renderH) / 2;
  } else {
    renderH = rect.height;
    renderW = rect.height * videoAspect;
    offsetX = rect.left + (rect.width - renderW) / 2;
    offsetY = rect.top;
  }
  const nx = Math.max(0, Math.min(1, (clientX - offsetX) / renderW));
  const ny = Math.max(0, Math.min(1, (clientY - offsetY) / renderH));
  return { nx, ny };
}

// --- リモート向けキーボード入力（window全体で捕捉） ---

// 修飾キー（単体では文字を生成しないキー）。"OS" はWindowsキーの別名（一部ブラウザ）。
const MODIFIER_KEYS = new Set(["Shift", "Control", "Alt", "Meta", "AltGraph", "OS"]);
// keydownで「キー押下」として送ったキーを記録し、keyupで確実に離す
const heldKeys = new Set<string>();
let keyboardAttached = false;

function sendKeyInput(obj: object): void {
  sendInput(obj);
}

function handleRemoteKeyDown(e: KeyboardEvent): void {
  const k = e.key;
  // Ctrl/Alt/Meta併用（ショートカット）かどうか
  const combo = e.ctrlKey || e.altKey || e.metaKey;

  if (MODIFIER_KEYS.has(k)) {
    // 修飾キーそのもの（Shift/Ctrl/Alt/Windows等）→ 押下を送る
    heldKeys.add(k);
    sendKeyInput({ type: "input", action: "key", key: k, isDown: true });
  } else if (combo) {
    // Ctrl+C などのショートカット → キー押下として送る（keyupで離す）
    heldKeys.add(k);
    sendKeyInput({ type: "input", action: "key", key: k, isDown: true });
  } else if (k.length === 1) {
    // 印字可能な1文字（記号・大文字・数字含む）→ そのまま文字入力
    sendKeyInput({ type: "input", action: "type-char", char: k });
  } else {
    // Enter / Backspace / Delete / 矢印 などの特殊キー → キー押下として送る
    heldKeys.add(k);
    sendKeyInput({ type: "input", action: "key", key: k, isDown: true });
  }
  // ブラウザ既定動作（スクロール・タブ移動・戻る等）を抑止
  e.preventDefault();
}

function handleRemoteKeyUp(e: KeyboardEvent): void {
  const k = e.key;
  // keydownで押下として送ったキーのみ離す（type-charは離さない）
  if (heldKeys.has(k)) {
    heldKeys.delete(k);
    sendKeyInput({ type: "input", action: "key", key: k, isDown: false });
    e.preventDefault();
  }
}

function attachRemoteKeyboard(): void {
  if (keyboardAttached) return;
  keyboardAttached = true;
  window.addEventListener("keydown", handleRemoteKeyDown, true);
  window.addEventListener("keyup", handleRemoteKeyUp, true);
}

function detachRemoteKeyboard(): void {
  if (!keyboardAttached) return;
  keyboardAttached = false;
  window.removeEventListener("keydown", handleRemoteKeyDown, true);
  window.removeEventListener("keyup", handleRemoteKeyUp, true);
  // 押しっぱなしのキーがあれば解放（キーの押しっぱなし防止）
  for (const k of heldKeys) {
    sendKeyInput({ type: "input", action: "key", key: k, isDown: false });
  }
  heldKeys.clear();
}

// --- スマホ向けタッチ操作（トラックパッドモード） ---
// 指のドラッグ → カーソルを相対移動 / タップ → クリック
// 2本指ドラッグ → スクロール / 2本指タップ → 右クリック
function setupTouchControls(el: HTMLElement): void {
  // ブラウザのスクロール・ズーム・プルダウン更新を無効化
  el.style.touchAction = "none";
  (el.style as any).overscrollBehavior = "none";

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
    sendInput(obj);
  };

  el.addEventListener("touchstart", (e) => {
    e.preventDefault();
    lastTouchTime = Date.now();
    // タップで再フォーカス（物理キーボード入力を拾えるように）
    el.focus();
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

  el.addEventListener("touchmove", (e) => {
    e.preventDefault();
    lastTouchTime = Date.now();

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

  el.addEventListener("touchend", (e) => {
    e.preventDefault();
    lastTouchTime = Date.now();
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

// --- カーソルオーバーレイ描画 ---
// canvas は video の上に重ねて配置。video の描画サイズに合わせて
// カーソルを描画する。
function drawCursorOverlay(): void {
  const canvas = document.getElementById("remote-canvas") as HTMLCanvasElement | null;
  const video = document.getElementById("remote-video") as HTMLVideoElement | null;
  if (!canvas || !video || !currentCursor) return;

  // video の実際の表示サイズに canvas を合わせる
  const container = canvas.parentElement;
  if (container) {
    const containerRect = container.getBoundingClientRect();
    canvas.width = containerRect.width;
    canvas.height = containerRect.height;
  }

  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  // video の object-contain 描画領域を計算
  const videoAspect = video.videoWidth / video.videoHeight;
  const containerAspect = canvas.width / canvas.height;
  let renderW: number, renderH: number, offsetX: number, offsetY: number;
  if (videoAspect > containerAspect) {
    renderW = canvas.width;
    renderH = canvas.width / videoAspect;
    offsetX = 0;
    offsetY = (canvas.height - renderH) / 2;
  } else {
    renderH = canvas.height;
    renderW = canvas.height * videoAspect;
    offsetX = (canvas.width - renderW) / 2;
    offsetY = 0;
  }

  const cx = offsetX + currentCursor.x * renderW;
  const cy = offsetY + currentCursor.y * renderH;
  drawCursor(ctx, cx, cy, renderW);
}

function drawCursor(ctx: CanvasRenderingContext2D, x: number, y: number, renderWidth: number): void {
  const s = Math.max(14, renderWidth / 60);
  const pts: [number, number][] = [
    [0, 0], [0, 1], [0.28, 0.75], [0.45, 1.15],
    [0.6, 1.08], [0.42, 0.68], [0.7, 0.68],
  ];
  ctx.save();
  ctx.beginPath();
  ctx.moveTo(x + pts[0][0] * s, y + pts[0][1] * s);
  for (let i = 1; i < pts.length; i++) {
    ctx.lineTo(x + pts[i][0] * s, y + pts[i][1] * s);
  }
  ctx.closePath();
  ctx.fillStyle = "#ffffff";
  ctx.strokeStyle = "#000000";
  ctx.lineWidth = Math.max(1.5, s / 12);
  ctx.lineJoin = "round";
  ctx.stroke();
  ctx.fill();
  ctx.restore();
}
