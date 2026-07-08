// スマホ向けタッチ操作（トラックパッドモード）

import { sendInput } from "./connection";
import { setLastTouchTime } from "./input";

// 再接続時に重複登録を防ぐため、タッチハンドラを保存
let touchTarget: HTMLElement | null = null;
let touchStartHandler: ((e: TouchEvent) => void) | null = null;
let touchMoveHandler: ((e: TouchEvent) => void) | null = null;
let touchEndHandler: ((e: TouchEvent) => void) | null = null;

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
