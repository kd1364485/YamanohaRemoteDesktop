import { WebSocketServer } from "ws";
import http from "http";
import bcrypt from "bcrypt";//パスワードハッシュ化ライブラリ
import jwt from "jsonwebtoken";
import Database from "better-sqlite3";//SQLiteデータベースドライバ

const PORT = process.env.PORT || 8080;
const JWT_SECRET = process.env.JWT_SECRET || "yamanoha-remote-desktop-secret-change-me";
const DB_PATH = process.env.DB_PATH || "data.db";

// --- データベース初期化 ---
const db = new Database(DB_PATH);
db.exec(`
  CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,//ユーザーID
    username TEXT UNIQUE NOT NULL,//ユーザー名
    password_hash TEXT NOT NULL,//パスワードハッシュ
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP//作成日時
  )
`);

// --- インメモリ状態 ---
// hosts: hostId -> { ws, userId, username, deviceName }
// clients: hostId -> ws
const hosts = new Map();
const clients = new Map();

function sendJSON(ws, data) {
  if (ws.readyState === ws.OPEN) {
    ws.send(JSON.stringify(data));
  }
}

//JWT（JSON Web Token）を検証する
function verifyToken(token) {
  try {
    return jwt.verify(token, JWT_SECRET);
  } catch {
    return null;
  }
}

// JSONボディをパース
function parseBody(req) {
  return new Promise((resolve) => {
    let body = "";
    req.on("data", (chunk) => (body += chunk));
    req.on("end", () => {
      try {
        resolve(JSON.parse(body));
      } catch {
        resolve(null);
      }
    });
  });
}

// --- HTTPサーバー（認証API） ---
const server = http.createServer(async (req, res) => {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type");

  if (req.method === "OPTIONS") {
    res.writeHead(204);
    res.end();
    return;
  }

  // ヘルスチェック（fly.io の死活監視・動作確認用）
  if (req.url === "/" || req.url === "/health") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ status: "ok" }));
    return;
  }

  // ユーザー登録
  if (req.url === "/api/register" && req.method === "POST") {
    const body = await parseBody(req);
    if (!body?.username || !body?.password) {
      res.writeHead(400, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "ユーザー名とパスワードが必要です" }));
      return;
    }

    const existing = db.prepare("SELECT id FROM users WHERE username = ?").get(body.username);
    if (existing) {
      res.writeHead(409, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "このユーザー名は既に使用されています" }));
      return;
    }

    const hash = bcrypt.hashSync(body.password, 10);
    const result = db.prepare("INSERT INTO users (username, password_hash) VALUES (?, ?)").run(body.username, hash);
    const token = jwt.sign({ userId: result.lastInsertRowid, username: body.username }, JWT_SECRET, { expiresIn: "7d" });

    res.writeHead(201, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ token, username: body.username }));
    return;
  }

  // ログイン
  if (req.url === "/api/login" && req.method === "POST") {
    const body = await parseBody(req);
    if (!body?.username || !body?.password) {
      res.writeHead(400, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "ユーザー名とパスワードが必要です" }));
      return;
    }

    const user = db.prepare("SELECT * FROM users WHERE username = ?").get(body.username);
    if (!user || !bcrypt.compareSync(body.password, user.password_hash)) {
      res.writeHead(401, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "ユーザー名またはパスワードが間違っています" }));
      return;
    }

    const token = jwt.sign({ userId: user.id, username: user.username }, JWT_SECRET, { expiresIn: "7d" });
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ token, username: user.username }));
    return;
  }

  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(JSON.stringify({ error: "Not found" }));
});

// --- WebSocketサーバー（HTTPサーバーに統合） ---
const wss = new WebSocketServer({ server });

wss.on("connection", (ws) => {
  let role = null;
  let userId = null;
  let username = null;
  let hostId = null;
  let deviceName = null;
  let authenticated = false;

  // heartbeat: pongを受け取ったら生存とみなす
  ws.isAlive = true;
  ws.on("pong", () => { ws.isAlive = true; });

  ws.on("message", (raw, isBinary) => {
    // バイナリメッセージ = 映像フレーム。JSONパースせずホスト→クライアントへ素通し。
    // 巨大なbase64文字列のparse/stringifyを避け、中継サーバーの負荷を最小化する。
    if (isBinary) {
      if (role === "host" && hostId) {
        const clientWs = clients.get(hostId);
        if (clientWs && clientWs.readyState === clientWs.OPEN) {
          clientWs.send(raw, { binary: true });
        }
      }
      return;
    }

    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      sendJSON(ws, { type: "error", message: "Invalid JSON" });
      return;
    }

    // 最初のメッセージは認証
    if (!authenticated) {
      if (msg.type !== "auth") {
        sendJSON(ws, { type: "error", message: "認証が必要です" });
        ws.close();
        return;
      }
      const payload = verifyToken(msg.token);//トークン検証
      if (!payload) {
        sendJSON(ws, { type: "error", message: "無効なトークンです" });
        ws.close();
        return;
      }
      authenticated = true;
      userId = payload.userId;
      username = payload.username;
      sendJSON(ws, { type: "auth-ok", username });
      return;
    }

    switch (msg.type) {
      case "host": {
        role = "host";
        deviceName = msg.deviceName || "PC";
        const instanceId = msg.instanceId || null;
        // 同一プロセス(instanceId一致)の古い登録だけを掃除する。
        // デバイス名で一括削除すると、再接続のたびに別IDで登録される自分自身や
        // 別プロセスを閉じ合い、切断ループになるため instanceId で限定する。
        if (instanceId) {
          for (const [hid, h] of hosts) {
            if (h.userId === userId && h.instanceId === instanceId) {
              hosts.delete(hid);
              try { h.ws.close(); } catch {}
              console.log(`Removed stale host: ${hid}`);
            }
          }
        }
        hostId = `host-${userId}-${Date.now()}`;//ホストID生成
        hosts.set(hostId, { ws, userId, username, deviceName, instanceId });//ホストを登録
        ws._hostId = hostId;//ホストIDを設定
        sendJSON(ws, { type: "host-ready", hostId, deviceName });//ホスト準備完了を送信
        console.log(`Host registered: ${username} (${deviceName}) -> ${hostId}`);//ログ出力
        break;
      }

      case "list-hosts": {
        const userHosts = [];
        for (const [hid, h] of hosts) {
          if (h.userId === userId) {
            userHosts.push({ hostId: hid, deviceName: h.deviceName });
          }
        }
        sendJSON(ws, { type: "host-list", hosts: userHosts });
        break;
      }

      case "connect": {
        role = "client";
        const targetHostId = msg.hostId;
        const hostEntry = hosts.get(targetHostId);
        if (!hostEntry || hostEntry.userId !== userId) {
          sendJSON(ws, { type: "error", message: "ホストが見つかりません" });
          return;
        }
        hostId = targetHostId;
        clients.set(hostId, ws);
        ws._hostId = hostId;
        sendJSON(ws, { type: "connected" });//接続完了を送信
        sendJSON(hostEntry.ws, { type: "client-connected" });//ホストに接続通知を送信
        console.log(`Client connected to host: ${hostId}`);//ログ出力
        break;
      }

      case "input": {
        if (role !== "client") return;
        const hostEntry = hosts.get(hostId);
        if (hostEntry) {
          hostEntry.ws.send(raw.toString());//入力データをホストに送信
        }
        break;
      }

      case "screen-info": {
        if (role !== "host") return;
        const clientWs = clients.get(hostId);
        if (clientWs) {
          clientWs.send(raw.toString());//スクリーン情報クライアントに送信
        }
        break;
      }

      case "display-list": {
        // ホスト → クライアント: 利用可能なディスプレイ一覧
        if (role !== "host") return;
        const clientWs = clients.get(hostId);
        if (clientWs) {
          clientWs.send(raw.toString());//ディスプレイ一覧をクライアントに送信
        }
        break;
      }

      case "cursor": {
        // ホスト → クライアント: カーソル位置通知
        if (role !== "host") return;
        const clientWs = clients.get(hostId);
        if (clientWs) {
          clientWs.send(raw.toString());
        }
        break;
      }

      case "select-display": {
        // クライアント → ホスト: 表示するディスプレイの切替え要求
        if (role !== "client") return;
        const hostEntry = hosts.get(hostId);
        if (hostEntry) {
          hostEntry.ws.send(raw.toString());//ディスプレイ選択をホストに送信
        }
        break;
      }

      case "webrtc-offer": {
        // ホスト → クライアント: WebRTC SDP offer
        if (role !== "host") return;
        const clientWs = clients.get(hostId);
        if (clientWs) {
          clientWs.send(raw.toString());
        }
        break;
      }

      case "webrtc-answer": {
        // クライアント → ホスト: WebRTC SDP answer
        if (role !== "client") return;
        const hostEntry = hosts.get(hostId);
        if (hostEntry) {
          hostEntry.ws.send(raw.toString());//SDP answerをホストに送信
        }
        break;
      }

      case "webrtc-ice": {
        // 双方向: WebRTC ICE candidate
        if (role === "host") {
          const clientWs = clients.get(hostId);
          if (clientWs) {
            clientWs.send(raw.toString());
          }
        } else if (role === "client") {
          const hostEntry = hosts.get(hostId);
          if (hostEntry) {
            hostEntry.ws.send(raw.toString());
          }
        }
        break;
      }

      default:
        break;
    }
  });

  ws.on("close", () => {
    if (role === "host" && hostId) {
      hosts.delete(hostId);
      const clientWs = clients.get(hostId);
      if (clientWs) {
        sendJSON(clientWs, { type: "disconnected", message: "ホストが切断しました" });
        clients.delete(hostId);
      }
      console.log(`Host disconnected: ${hostId}`);
    } else if (role === "client" && hostId) {
      clients.delete(hostId);
      const hostEntry = hosts.get(hostId);
      if (hostEntry) {
        sendJSON(hostEntry.ws, { type: "disconnected", message: "クライアントが切断しました" });
      }
      console.log(`Client disconnected from: ${hostId}`);
    }
  });
});

// heartbeat: 30秒ごとに死活確認し、応答のない接続を切断（ゴースト登録の除去）
const heartbeatInterval = setInterval(() => {
  for (const ws of wss.clients) {
    if (ws.isAlive === false) {
      ws.terminate(); // close イベントが発火し、hosts/clients から除去される
      continue;
    }
    ws.isAlive = false;
    try { ws.ping(); } catch {}
  }
}, 30000);
wss.on("close", () => clearInterval(heartbeatInterval));

server.listen(PORT, () => {
  console.log(`Signaling server running on port ${PORT}`);
});
