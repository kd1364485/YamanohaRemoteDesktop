import { WebSocket } from "ws";
import screenshot from "screenshot-desktop";
import { mouse, keyboard, Button, Key } from "@nut-tree-fork/nut-js";

const SERVER_URL = process.env.SIGNALING_URL || "ws://localhost:8080";

mouse.config.mouseSpeed = 0;
keyboard.config.autoDelayMs = 0;

let ws;
let isStreaming = false;
let streamInterval;

function connect() {
  ws = new WebSocket(SERVER_URL);

  ws.on("open", () => {
    console.log("Connected to signaling server");
    ws.send(JSON.stringify({ type: "host" }));
  });

  ws.on("message", async (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      return;
    }

    switch (msg.type) {
      case "host-ready":
        console.log(`Host ready! Code: ${msg.code}`);
        break;

      case "client-connected":
        console.log("Client connected, starting stream...");
        startStreaming();
        break;

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

  const interval = 1000 / 15;

  streamInterval = setInterval(async () => {
    if (!ws || ws.readyState !== ws.OPEN) return;

    try {
      const img = await screenshot({ format: "jpg" });
      ws.send(JSON.stringify({ type: "frame", data: img.toString("base64") }));
    } catch (err) {
      console.error("Capture error:", err.message);
    }
  }, interval);
}

function stopStreaming() {
  isStreaming = false;
  if (streamInterval) {
    clearInterval(streamInterval);
    streamInterval = null;
  }
}

async function handleInput(msg) {
  try {
    switch (msg.action) {
      case "mouse-move":
        await mouse.setPosition({ x: msg.x, y: msg.y });
        break;

      case "mouse-click":
        await mouse.setPosition({ x: msg.x, y: msg.y });
        if (msg.button === "left") {
          await mouse.click(Button.LEFT);
        } else if (msg.button === "right") {
          await mouse.click(Button.RIGHT);
        }
        break;

      case "mouse-down":
        if (msg.button === "left") await mouse.pressButton(Button.LEFT);
        else if (msg.button === "right") await mouse.pressButton(Button.RIGHT);
        break;

      case "mouse-up":
        if (msg.button === "left") await mouse.releaseButton(Button.LEFT);
        else if (msg.button === "right") await mouse.releaseButton(Button.RIGHT);
        break;

      case "mouse-scroll":
        await mouse.scrollDown(msg.deltaY > 0 ? msg.deltaY : -msg.deltaY);
        break;

      case "key":
        const key = mapKey(msg.key);
        if (key) {
          if (msg.isDown) {
            await keyboard.pressKey(key);
          } else {
            await keyboard.releaseKey(key);
          }
        }
        break;

      case "key-type":
        const typeKey = mapKey(msg.key);
        if (typeKey) {
          await keyboard.type(typeKey);
        }
        break;
    }
  } catch (err) {
    console.error("Input error:", err.message);
  }
}

function mapKey(key) {
  const keyMap = {
    "a": Key.A, "b": Key.B, "c": Key.C, "d": Key.D, "e": Key.E,
    "f": Key.F, "g": Key.G, "h": Key.H, "i": Key.I, "j": Key.J,
    "k": Key.K, "l": Key.L, "m": Key.M, "n": Key.N, "o": Key.O,
    "p": Key.P, "q": Key.Q, "r": Key.R, "s": Key.S, "t": Key.T,
    "u": Key.U, "v": Key.V, "w": Key.W, "x": Key.X, "y": Key.Y,
    "z": Key.Z,
    "0": Key.Num0, "1": Key.Num1, "2": Key.Num2, "3": Key.Num3,
    "4": Key.Num4, "5": Key.Num5, "6": Key.Num6, "7": Key.Num7,
    "8": Key.Num8, "9": Key.Num9,
    "Enter": Key.Enter, "Escape": Key.Escape, "Backspace": Key.Backspace,
    "Tab": Key.Tab, "Space": Key.Space,
    "Shift": Key.LeftShift, "Control": Key.LeftControl,
    "Alt": Key.LeftAlt, "Meta": Key.LeftSuper,
    "ArrowUp": Key.Up, "ArrowDown": Key.Down,
    "ArrowLeft": Key.Left, "ArrowRight": Key.Right,
    "Delete": Key.Delete, "Home": Key.Home, "End": Key.End,
  };
  return keyMap[key] || null;
}

connect();
