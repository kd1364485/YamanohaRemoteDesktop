import { WebSocketServer } from "ws";

const PORT = process.env.PORT || 8080;

const wss = new WebSocketServer({ port: PORT });

const hosts = new Map();
const clients = new Map();

function generateCode() {
  return Math.random().toString(36).substring(2, 8).toUpperCase();
}

function sendJSON(ws, data) {
  if (ws.readyState === ws.OPEN) {
    ws.send(JSON.stringify(data));
  }
}

wss.on("connection", (ws) => {
  let role = null;
  let code = null;

  ws.on("message", (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      sendJSON(ws, { type: "error", message: "Invalid JSON" });
      return;
    }

    switch (msg.type) {
      case "host": {
        role = "host";
        code = msg.code ? String(msg.code).toUpperCase() : generateCode();
        hosts.set(code, ws);
        ws._code = code;
        sendJSON(ws, { type: "host-ready", code });
        console.log(`Host registered: ${code}`);
        break;
      }

      case "connect": {
        role = "client";
        code = msg.code?.toUpperCase();
        const hostWs = hosts.get(code);
        if (!hostWs) {
          sendJSON(ws, { type: "error", message: "接続コードが見つかりません" });
          return;
        }
        clients.set(code, ws);
        ws._code = code;
        sendJSON(ws, { type: "connected" });
        sendJSON(hostWs, { type: "client-connected" });
        console.log(`Client connected to host: ${code}`);
        break;
      }

      case "frame": {
        if (role !== "host") return;
        const clientWs = clients.get(code);
        if (clientWs) {
          clientWs.send(raw.toString());
        }
        break;
      }

      case "input": {
        if (role !== "client") return;
        const hostWs = hosts.get(code);
        if (hostWs) {
          hostWs.send(raw.toString());
        }
        break;
      }

      case "screen-info": {
        if (role !== "host") return;
        const clientWs = clients.get(code);
        if (clientWs) {
          clientWs.send(raw.toString());
        }
        break;
      }

      default:
        break;
    }
  });

  ws.on("close", () => {
    if (role === "host" && code) {
      hosts.delete(code);
      const clientWs = clients.get(code);
      if (clientWs) {
        sendJSON(clientWs, { type: "disconnected", message: "ホストが切断しました" });
        clients.delete(code);
      }
      console.log(`Host disconnected: ${code}`);
    } else if (role === "client" && code) {
      clients.delete(code);
      const hostWs = hosts.get(code);
      if (hostWs) {
        sendJSON(hostWs, { type: "disconnected", message: "クライアントが切断しました" });
      }
      console.log(`Client disconnected from: ${code}`);
    }
  });
});

console.log(`Signaling server running on port ${PORT}`);
