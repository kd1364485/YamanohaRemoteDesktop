import { WebSocket } from "ws";
import screenshot from "screenshot-desktop";
import sharp from "sharp";
import { mouse, keyboard, Button, Key } from "@nut-tree-fork/nut-js";

const SERVER_URL = process.env.SIGNALING_URL || "ws://localhost:8080";
const AUTH_TOKEN = process.env.SIGNALING_TOKEN || null;
const DEVICE_NAME = process.env.SIGNALING_DEVICE_NAME || "PC";
// プロセス起動時に一度だけ生成する固定ID。再接続時の重複登録を一意に識別するために使う。
const INSTANCE_ID = `inst-${Date.now()}-${Math.random().toString(36).slice(2, 10)}`;

// --- 画質・パフォーマンス設定（ラグ対策） ---
const TARGET_WIDTH = Number(process.env.STREAM_WIDTH) || 1280; // 送信画像の横幅（縮小）
const JPEG_QUALITY = Number(process.env.STREAM_QUALITY) || 55; // JPEG品質 0-100
const TARGET_FPS = Number(process.env.STREAM_FPS) || 30;       // 目標フレームレート

mouse.config.mouseSpeed = 0;
keyboard.config.autoDelayMs = 0;

let ws;
let isStreaming = false;
let displays = [];          // 利用可能なディスプレイ一覧
let currentDisplay = null;  // 現在キャプチャ中のディスプレイ
let trackedCursor = null;    // カーソル位置（論理座標）を自前で追跡。nut-jsのgetPosition()がセカンダリディスプレイで不正確なため。

// ディスプレイ一覧を取得し、現在のディスプレイを決定する
async function loadDisplays() {
  try {
    const list = await screenshot.listDisplays();
    // screenshot-desktop は物理ピクセル、nut-js は論理ピクセル(DPIスケール適用後)を使う。
    // マウス制御・カーソル位置は nut-js 座標系に揃えるため、論理座標(l*)を保持する。
    // プライマリディスプレイのDPIスケールを取得（仮想画面オフセットの変換に使う）
    const primary = list.find((d) => d.left === 0 && d.top === 0) || list[0];
    const primaryScale = (primary && primary.dpiScale) || 1;

    displays = list.map((d, i) => {
      const scale = d.dpiScale || 1;
      return {
        id: d.id,
        label: `ディスプレイ${i + 1} (${d.width}×${d.height})`,
        width: d.width,   // 物理ピクセル（ラベル表示用）
        height: d.height,
        // left/top は仮想画面上の物理ピクセル座標 → プライマリのスケールで論理化
        // width/height は各ディスプレイ自身のスケールで論理化
        lLeft: d.left / primaryScale,
        lTop: d.top / primaryScale,
        lWidth: d.width / scale,
        lHeight: d.height / scale,
        primary: d.left === 0 && d.top === 0,
      };
    });
    // 未選択 or 選択中の画面がなくなった場合はプライマリ（なければ先頭）を選択
    if (!currentDisplay || !displays.find((d) => d.id === currentDisplay.id)) {
      currentDisplay = displays.find((d) => d.primary) || displays[0] || null;
    }
  } catch (err) {
    console.error("Display list error:", err.message);
    displays = [];
    currentDisplay = null;
  }
}

// クライアントへディスプレイ一覧を送信
function sendDisplayList() {
  if (!ws || ws.readyState !== ws.OPEN) return;
  ws.send(JSON.stringify({
    type: "display-list",
    displays: displays.map(({ id, label, width, height, primary }) => ({ id, label, width, height, primary })),
    currentId: currentDisplay?.id || null,
  }));
}

// 正規化座標(0..1)を選択ディスプレイ上のグローバル座標に変換
function toGlobalPosition(nx, ny) {
  if (typeof nx !== "number" || typeof ny !== "number" || !currentDisplay) return null;
  // nut-js は論理座標を使うので論理バウンドで換算
  return {
    x: Math.round(currentDisplay.lLeft + nx * currentDisplay.lWidth),
    y: Math.round(currentDisplay.lTop + ny * currentDisplay.lHeight),
  };
}

function connect() {
  ws = new WebSocket(SERVER_URL);

  ws.on("open", () => {
    console.log("Connected to signaling server");
    ws.send(JSON.stringify({ type: "auth", token: AUTH_TOKEN }));
  });

  ws.on("message", async (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      return;
    }

    switch (msg.type) {
      case "auth-ok":
        console.log(`Authenticated as ${msg.username}`);
        ws.send(JSON.stringify({ type: "host", deviceName: DEVICE_NAME, instanceId: INSTANCE_ID }));
        break;

      case "host-ready":
        console.log(`Host ready! hostId: ${msg.hostId}, device: ${msg.deviceName}`);
        break;

      case "client-connected":
        console.log("Client connected, starting stream...");
        await loadDisplays();
        sendDisplayList();
        startStreaming();
        break;

      case "select-display": {
        const target = displays.find((d) => d.id === msg.displayId);
        if (target) {
          currentDisplay = target;
          console.log(`Display switched to: ${target.label}`);
          // カーソルを選択ディスプレイの中央へ移動し、追跡位置も更新
          const cx = Math.round(target.lLeft + target.lWidth / 2);
          const cy = Math.round(target.lTop + target.lHeight / 2);
          trackedCursor = { x: cx, y: cy };
          try {
            await mouse.setPosition(trackedCursor);
            // setPosition した座標と実際の位置が異なる場合があるため、
            // getPosition() で実際の位置を取り直して描画位置を同期させる
            trackedCursor = await mouse.getPosition();
          } catch {}
        }
        break;
      }

      case "input":
        await handleInput(msg);
        break;

      case "disconnected":
        console.log("Client disconnected:", msg.message);
        stopStreaming();
        break;
    }
  });

  ws.on("close", () => {
    console.log("Disconnected from server, reconnecting in 3s...");
    stopStreaming();
    setTimeout(connect, 3000);
  });

  ws.on("error", (err) => {
    console.error("WebSocket error:", err.message);
  });
}

async function startStreaming() {
  if (isStreaming) return;
  isStreaming = true;

  const minFrameMs = 1000 / TARGET_FPS;

  // 逐次ループ: 1フレームのキャプチャ→圧縮→送信が完了してから次へ進む。
  // setIntervalと違い処理が詰まらず、遅延の蓄積を防ぐ。
  while (isStreaming) {
    const frameStart = Date.now();

    // WebSocketが開いていない場合、少し待機してから再試行
    if (!ws || ws.readyState !== ws.OPEN) {
      await sleep(50);
      continue;
    }

    // 送信バッファが詰まっている（前フレームが送り切れていない）ら、
    // 新しいフレームを送らずスキップして遅延の蓄積を防ぐ。
    if (ws.bufferedAmount > 1_000_000) {
      await sleep(minFrameMs);
      continue;
    }

    try {
      // PNG形式で取得し、sharpで縮小+JPEG圧縮（データ量を大幅削減）
      // currentDisplay が指定されていればそのディスプレイのみキャプチャ
      const captureOpts = { format: "png" };
      if (currentDisplay) captureOpts.screen = currentDisplay.id;
      const raw = await screenshot(captureOpts);
      const jpeg = await sharp(raw)
        .resize({ width: TARGET_WIDTH, withoutEnlargement: true })
        .jpeg({ quality: JPEG_QUALITY, mozjpeg: true })
        .toBuffer();

      // カーソル位置を取得し、選択ディスプレイ基準の正規化座標(0..1)に変換。
      // 特定ディスプレイ指定キャプチャではOSがカーソルを描画しないため、
      // クライアント側で重ねて描画できるよう位置を一緒に送る。
      // getPosition()がセカンダリディスプレイで不正確なため trackedCursor を優先。
      let cursor = null;
      if (currentDisplay && trackedCursor) {
        const cx = (trackedCursor.x - currentDisplay.lLeft) / currentDisplay.lWidth;
        const cy = (trackedCursor.y - currentDisplay.lTop) / currentDisplay.lHeight;
        if (cx >= 0 && cx <= 1 && cy >= 0 && cy <= 1) cursor = { x: cx, y: cy };
      }

      ws.send(JSON.stringify({ type: "frame", data: jpeg.toString("base64"), cursor }));
    } catch (err) {
      console.error("Capture error:", err.message);
    }

    // フレームレート上限を超えないよう待機
    const elapsed = Date.now() - frameStart;
    if (elapsed < minFrameMs) {
      await sleep(minFrameMs - elapsed);
    }
  }
}

function stopStreaming() {
  isStreaming = false;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function handleInput(msg) {
  try {
    switch (msg.action) {
      case "mouse-move": {
        const p = toGlobalPosition(msg.nx, msg.ny);
        if (p) {
          await mouse.setPosition(p);
          try { trackedCursor = await mouse.getPosition(); } catch {}
        }
        break;
      }

      case "mouse-move-rel": {
        // 現在位置からの相対移動（スマホのトラックパッド操作用）
        // getPosition()がセカンダリディスプレイで不正確なため、自前追跡値を使う
        if (!trackedCursor) {
          try { trackedCursor = await mouse.getPosition(); } catch { break; }
        }
        let nx = trackedCursor.x + msg.dx;
        let ny = trackedCursor.y + msg.dy;
        // カーソルが現在のディスプレイ範囲外に出ないようクランプ
        if (currentDisplay) {
          nx = Math.max(currentDisplay.lLeft, Math.min(nx, currentDisplay.lLeft + currentDisplay.lWidth - 1));
          ny = Math.max(currentDisplay.lTop, Math.min(ny, currentDisplay.lTop + currentDisplay.lHeight - 1));
        }
        trackedCursor = { x: nx, y: ny };
        await mouse.setPosition(trackedCursor);
        break;
      }

      case "mouse-click": {
        // 正規化座標が指定されていれば移動してからクリック、なければ現在位置でクリック
        const p = toGlobalPosition(msg.nx, msg.ny);
        if (p) {
          await mouse.setPosition(p);
          try { trackedCursor = await mouse.getPosition(); } catch {}
        }
        if (msg.button === "right") {
          await mouse.click(Button.RIGHT);
        } else {
          await mouse.click(Button.LEFT);
        }
        break;
      }

      case "mouse-double-click": {
        const p = toGlobalPosition(msg.nx, msg.ny);
        if (p) {
          await mouse.setPosition(p);
          try { trackedCursor = await mouse.getPosition(); } catch {}
        }
        await mouse.doubleClick(msg.button === "right" ? Button.RIGHT : Button.LEFT);
        break;
      }

      case "mouse-down": {
        const p = toGlobalPosition(msg.nx, msg.ny);
        if (p) {
          await mouse.setPosition(p);
          try { trackedCursor = await mouse.getPosition(); } catch {}
        }
        if (msg.button === "left") await mouse.pressButton(Button.LEFT);
        else if (msg.button === "right") await mouse.pressButton(Button.RIGHT);
        break;
      }

      case "mouse-up":
        if (msg.button === "left") await mouse.releaseButton(Button.LEFT);
        else if (msg.button === "right") await mouse.releaseButton(Button.RIGHT);
        break;

      case "mouse-scroll": {
        // deltaY > 0 で下方向、< 0 で上方向。符号で向きを切り替える。
        const amount = Math.max(1, Math.round(Math.abs(msg.deltaY)));
        if (msg.deltaY > 0) {
          await mouse.scrollDown(amount);
        } else if (msg.deltaY < 0) {
          await mouse.scrollUp(amount);
        }
        break;
      }

      case "key": {
        const key = mapKey(msg.key);
        if (key !== null) {
          if (msg.isDown) {
            await keyboard.pressKey(key);
          } else {
            await keyboard.releaseKey(key);
          }
        }
        break;
      }

      case "type-char": {
        // 印字可能な文字（記号・大文字含む）をそのまま入力
        if (typeof msg.char === "string" && msg.char.length > 0) {
          await keyboard.type(msg.char);
        }
        break;
      }
    }
  } catch (err) {
    console.error("Input error:", err.message);
  }
}

function mapKey(key) {
  if (typeof key !== "string") return null;

  // 1文字の英字は大文字・小文字どちらも同じ物理キーへ（Ctrl+C など併用時用）
  if (key.length === 1) {
    const lower = key.toLowerCase();
    if (lower >= "a" && lower <= "z") return Key[lower.toUpperCase()];
    const digit = { "0": Key.Num0, "1": Key.Num1, "2": Key.Num2, "3": Key.Num3,
      "4": Key.Num4, "5": Key.Num5, "6": Key.Num6, "7": Key.Num7,
      "8": Key.Num8, "9": Key.Num9, " ": Key.Space };
    if (digit[key] !== undefined) return digit[key];
  }

  const keyMap = {
    "Enter": Key.Enter, "Escape": Key.Escape, "Backspace": Key.Backspace,
    "Tab": Key.Tab, "Space": Key.Space, "CapsLock": Key.CapsLock,
    "Shift": Key.LeftShift, "Control": Key.LeftControl,
    "Alt": Key.LeftAlt, "Meta": Key.LeftSuper, "OS": Key.LeftSuper,
    "ArrowUp": Key.Up, "ArrowDown": Key.Down,
    "ArrowLeft": Key.Left, "ArrowRight": Key.Right,
    "Delete": Key.Delete, "Home": Key.Home, "End": Key.End,
    "PageUp": Key.PageUp, "PageDown": Key.PageDown, "Insert": Key.Insert,
    "F1": Key.F1, "F2": Key.F2, "F3": Key.F3, "F4": Key.F4,
    "F5": Key.F5, "F6": Key.F6, "F7": Key.F7, "F8": Key.F8,
    "F9": Key.F9, "F10": Key.F10, "F11": Key.F11, "F12": Key.F12,
  };
  return keyMap[key] !== undefined ? keyMap[key] : null;
}

connect();
