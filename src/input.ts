// マウス・キーボード入力処理

import { sendInput } from "./connection";

// タッチ操作直後の合成マウスイベントを無視するためのタイマー
export let lastTouchTime = 0;
export const TOUCH_EVENT_COOLDOWN_MS = 500;

export function setLastTouchTime(t: number): void {
  lastTouchTime = t;
}

// 修飾キー（単体では文字を生成しないキー）。"OS" はWindowsキーの別名（一部ブラウザ）。
const MODIFIER_KEYS = new Set(["Shift", "Control", "Alt", "Meta", "AltGraph", "OS"]);
// keydownで「キー押下」として送ったキーを記録し、keyupで確実に離す
const heldKeys = new Set<string>();
let keyboardAttached = false;

function handleRemoteKeyDown(e: KeyboardEvent): void {
  const k = e.key;
  const combo = e.ctrlKey || e.altKey || e.metaKey;

  if (MODIFIER_KEYS.has(k)) {
    heldKeys.add(k);
    sendInput({ type: "input", action: "key", key: k, isDown: true });
  } else if (combo) {
    heldKeys.add(k);
    sendInput({ type: "input", action: "key", key: k, isDown: true });
  } else if (k.length === 1) {
    sendInput({ type: "input", action: "type-char", char: k });
  } else {
    heldKeys.add(k);
    sendInput({ type: "input", action: "key", key: k, isDown: true });
  }
  e.preventDefault();
}

function handleRemoteKeyUp(e: KeyboardEvent): void {
  const k = e.key;
  if (heldKeys.has(k)) {
    heldKeys.delete(k);
    sendInput({ type: "input", action: "key", key: k, isDown: false });
    e.preventDefault();
  }
}

export function attachRemoteKeyboard(): void {
  if (keyboardAttached) return;
  keyboardAttached = true;
  window.addEventListener("keydown", handleRemoteKeyDown, true);
  window.addEventListener("keyup", handleRemoteKeyUp, true);
}

export function detachRemoteKeyboard(): void {
  if (!keyboardAttached) return;
  keyboardAttached = false;
  window.removeEventListener("keydown", handleRemoteKeyDown, true);
  window.removeEventListener("keyup", handleRemoteKeyUp, true);
  for (const k of heldKeys) {
    sendInput({ type: "input", action: "key", key: k, isDown: false });
  }
  heldKeys.clear();
}

// video要素のobject-contain描画領域を考慮して、マウス座標を0..1の正規化座標に変換
function videoToNormalized(
  video: HTMLVideoElement,
  rect: DOMRect,
  clientX: number,
  clientY: number,
): { nx: number; ny: number } {
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

export function setupRemoteCanvas(
  clientWs: WebSocket | null,
  setupTouchControls: (el: HTMLElement) => void,
): void {
  const video = document.getElementById("remote-video") as HTMLVideoElement | null;
  const placeholder = document.getElementById("remote-placeholder");
  if (placeholder) placeholder.style.display = "none";
  if (video) {
    video.style.display = "block";

    video.onmousemove = (e) => {
      if (!clientWs || clientWs.readyState !== WebSocket.OPEN) return;
      if (Date.now() - lastTouchTime < TOUCH_EVENT_COOLDOWN_MS) return;
      const rect = video.getBoundingClientRect();
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
