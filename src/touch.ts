// スマホ向けタッチ操作（トラックパッドモード）

import { sendInput } from "./connection";
import { setLastTouchTime } from "./input";

// 再接続時に重複登録を防ぐため、タッチハンドラを保存
let touchTarget: HTMLElement | null = null;
let touchStartHandler: ((e: TouchEvent) => void) | null = null;
let touchMoveHandler: ((e: TouchEvent) => void) | null = null;
let touchEndHandler: ((e: TouchEvent) => void) | null = null;

// 遅延の測定用: URL に ?tapmark を付けて開くと、クリックを送った瞬間に、画面の左上へ赤い四角を出す。
// スマホの画面録画だけで「クリックを送った瞬間」と「PC の画面の変化が映った瞬間」が分かる
// (タップ → PC → 映像 → スマホ の往復遅延)。付けないときは、何もしない。
// スマホの Chrome では URL の入力で ?tapmark が落ちやすいので、起動時の環境変数
// VITE_TAPMARK=1 でも有効にできる(例(Git Bash): VITE_TAPMARK=1 npm run tauri dev)。
const tapMarkEnabled =
  new URLSearchParams(window.location.search).has("tapmark") ||
  import.meta.env.VITE_TAPMARK === "1";
let tapMarkTimer: ReturnType<typeof setTimeout> | null = null;

function flashTapMark(): void {
  let mark = document.getElementById("tap-mark");
  if (!mark) {
    mark = document.createElement("div");
    mark.id = "tap-mark";
    mark.style.cssText =
      "position:fixed;left:0;top:0;width:18vmin;height:18vmin;background:#f00;" +
      "z-index:99999;pointer-events:none;display:none;";
    document.body.appendChild(mark);
  }
  mark.style.display = "block";
  if (tapMarkTimer) clearTimeout(tapMarkTimer);
  tapMarkTimer = setTimeout(() => { mark!.style.display = "none"; }, 200);
}

export function detachTouchControls(): void {
  if (touchTarget) {
    if (touchStartHandler) touchTarget.removeEventListener("touchstart", touchStartHandler, { passive: false } as AddEventListenerOptions);
    if (touchMoveHandler) touchTarget.removeEventListener("touchmove", touchMoveHandler, { passive: false } as AddEventListenerOptions);
    if (touchEndHandler) touchTarget.removeEventListener("touchend", touchEndHandler, { passive: false } as AddEventListenerOptions);
  }
  touchTarget = null;
  touchStartHandler = null;
  touchMoveHandler = null;
  touchEndHandler = null;
}

export function setupTouchControls(el: HTMLElement): void {
  detachTouchControls();

  // 測定モードが有効なことを、録画の前に目で確かめられるよう、右下に小さく表示する
  if (tapMarkEnabled && !document.getElementById("tap-mark-label")) {
    const label = document.createElement("div");
    label.id = "tap-mark-label";
    label.textContent = "tapmark ON";
    label.style.cssText =
      "position:fixed;right:4px;bottom:4px;padding:2px 6px;font:12px sans-serif;" +
      "color:#fff;background:#c00;z-index:99999;pointer-events:none;";
    document.body.appendChild(label);
  }

  el.style.touchAction = "none";
  (el.style as any).overscrollBehavior = "none";

  const MOVE_SENSITIVITY = 2.0;
  const TAP_THRESHOLD_PX = 10;
  const TAP_THRESHOLD_MS = 250;
  const DOUBLE_TAP_MS = 300;

  let lastX = 0;
  let lastY = 0;
  let startX = 0;
  let startY = 0;
  let startTime = 0;
  let moved = false;
  let touchCount = 0;
  let lastScrollY = 0;

  let lastTapTime = 0;
  let dragCandidate = false;
  let isDragging = false;

  const send = (obj: object) => {
    sendInput(obj);
    if (tapMarkEnabled && (obj as { action?: string }).action === "mouse-click") {
      flashTapMark();
    }
  };

  touchStartHandler = (e: TouchEvent) => {
    e.preventDefault();
    setLastTouchTime(Date.now());
    el.focus();
    touchCount = e.touches.length;
    const t = e.touches[0];
    lastX = t.clientX;
    lastY = t.clientY;
    startX = t.clientX;
    startY = t.clientY;
    startTime = Date.now();
    moved = false;

    dragCandidate = e.touches.length === 1 && (startTime - lastTapTime) < DOUBLE_TAP_MS;

    if (e.touches.length === 2) {
      lastScrollY = (e.touches[0].clientY + e.touches[1].clientY) / 2;
    }
  };

  touchMoveHandler = (e: TouchEvent) => {
    e.preventDefault();
    setLastTouchTime(Date.now());

    if (e.touches.length === 2) {
      const midY = (e.touches[0].clientY + e.touches[1].clientY) / 2;
      const dy = midY - lastScrollY;
      lastScrollY = midY;
      if (Math.abs(dy) > 1) {
        send({ type: "input", action: "mouse-scroll", deltaY: -dy * 3 });
        moved = true;
      }
      return;
    }

    const t = e.touches[0];
    const dx = (t.clientX - lastX) * MOVE_SENSITIVITY;
    const dy = (t.clientY - lastY) * MOVE_SENSITIVITY;
    lastX = t.clientX;
    lastY = t.clientY;

    const distFromStart = Math.hypot(t.clientX - startX, t.clientY - startY);
    if (distFromStart > TAP_THRESHOLD_PX) moved = true;

    if (dragCandidate && moved && !isDragging) {
      isDragging = true;
      send({ type: "input", action: "mouse-down", button: "left" });
    }

    if (dx !== 0 || dy !== 0) {
      send({ type: "input", action: "mouse-move-rel", dx: Math.round(dx), dy: Math.round(dy) });
    }
  };

  touchEndHandler = (e: TouchEvent) => {
    e.preventDefault();
    setLastTouchTime(Date.now());
    const elapsed = Date.now() - startTime;

    if (isDragging) {
      send({ type: "input", action: "mouse-up", button: "left" });
      isDragging = false;
      lastTapTime = 0;
      dragCandidate = false;
      touchCount = 0;
      return;
    }

    if (!moved && elapsed < TAP_THRESHOLD_MS) {
      if (touchCount >= 2) {
        send({ type: "input", action: "mouse-click", button: "right" });
        lastTapTime = 0;
      } else if (dragCandidate) {
        send({ type: "input", action: "mouse-double-click", button: "left" });
        lastTapTime = 0;
      } else {
        send({ type: "input", action: "mouse-click", button: "left" });
        lastTapTime = Date.now();
      }
    } else {
      lastTapTime = 0;
    }
    dragCandidate = false;
    touchCount = 0;
  };

  touchTarget = el;
  el.addEventListener("touchstart", touchStartHandler, { passive: false } as AddEventListenerOptions);
  el.addEventListener("touchmove", touchMoveHandler, { passive: false } as AddEventListenerOptions);
  el.addEventListener("touchend", touchEndHandler, { passive: false } as AddEventListenerOptions);
}
