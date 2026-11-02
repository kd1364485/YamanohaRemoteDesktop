import { WebSocketServer } from "ws";
import http from "http";
import bcrypt from "bcrypt";//パスワードハッシュ化ライブラリ
import jwt from "jsonwebtoken";
import Database from "better-sqlite3";//SQLiteデータベースドライバ

const PORT = process.env.PORT || 8080;
const DB_PATH = process.env.DB_PATH || "data.db";

// JWT の署名鍵は環境変数から必ず受け取る。
// 以前は、未設定のとき、ソースコードに書かれた初期値を使っていた。その値を知っている人は、
// パスワードなしで、他人のトークンを作れてしまう(=画面の閲覧と遠隔操作ができる)ため、
// 未設定、または短すぎる場合は、起動しない。
const JWT_SECRET = process.env.JWT_SECRET;
if (!JWT_SECRET || JWT_SECRET.length < 32) {
  console.error(
    "JWT_SECRET が未設定か、32文字未満です。サーバーを起動しません。\n" +
    "  ランダムな値の作り方(PowerShell): " +
    "$b = New-Object byte[] 48; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b); [Convert]::ToBase64String($b)\n" +
    "  fly.io: fly secrets set JWT_SECRET=<値> -a <アプリ名>   /   ローカル: $env:JWT_SECRET = '<値>'"
  );
  process.exit(1);
}

// 新規登録: 最初のユーザー(初回セットアップ)だけは自由に登録できる。
// 2人目以降は、ALLOW_REGISTRATION=1 を設定したときだけ登録できる(個人用の既定は「登録を閉じる」)。
const ALLOW_REGISTRATION = process.env.ALLOW_REGISTRATION === "1";

// --- データベース初期化 ---
const db = new Database(DB_PATH);
db.exec(`
  CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT, -- ユーザーID
    username TEXT UNIQUE NOT NULL, -- ユーザー名
    password_hash TEXT NOT NULL, -- パスワードハッシュ
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP -- 作成日時
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

// JSONボディをパース(大きすぎるボディは、読み捨てて null を返す)
const MAX_BODY_BYTES = 16 * 1024;
function parseBody(req) {
  return new Promise((resolve) => {
    let body = "";
    let tooLarge = false;
    req.on("data", (chunk) => {
      if (tooLarge) return;
      body += chunk;
      if (body.length > MAX_BODY_BYTES) {
        tooLarge = true;
        body = "";
      }
    });
    req.on("end", () => {
      if (tooLarge) return resolve(null);
      try {
        resolve(JSON.parse(body));
      } catch {
        resolve(null);
      }
    });
  });
}

// --- ログイン・登録の失敗回数の制限(メモリ上。サーバーを再起動すると、リセットされる) ---
// 「IP ごと」と「ユーザー名ごと」の両方で数える。IP だけだと、偽装や複数の IP で回避され、
// ユーザー名だけだと、ほかの人のアカウントを、わざと止められるので、両方を組み合わせる。
const LIMIT_WINDOW_MS = 15 * 60 * 1000;   // 失敗を数える期間
const LIMIT_LOCK_MS = 15 * 60 * 1000;     // 上限に達したときの、ロック時間
const MAX_FAILS_PER_IP = 20;
const MAX_FAILS_PER_USER = 8;
const MAX_REGISTER_PER_IP = 10;
const attempts = new Map(); // key -> { fails: number[], lockedUntil: number }

function lockedForMs(key, now = Date.now()) {
  const e = attempts.get(key);
  return e && e.lockedUntil > now ? e.lockedUntil - now : 0;
}

function recordAttempt(key, max, now = Date.now()) {
  const e = attempts.get(key) || { fails: [], lockedUntil: 0 };
  e.fails = e.fails.filter((t) => now - t < LIMIT_WINDOW_MS);
  e.fails.push(now);
  if (e.fails.length >= max) {
    e.lockedUntil = now + LIMIT_LOCK_MS;
    e.fails = [];
  }
  attempts.set(key, e);
}

setInterval(() => {
  const now = Date.now();
  for (const [key, e] of attempts) {
    if (e.lockedUntil <= now && e.fails.every((t) => now - t >= LIMIT_WINDOW_MS)) {
      attempts.delete(key);
    }
  }
}, 10 * 60 * 1000).unref();

// 接続元の IP。fly.io は、Fly-Client-IP を付ける。そのほかは、X-Forwarded-For の先頭を使う。
function clientIp(req) {
  const fly = req.headers["fly-client-ip"];
  if (typeof fly === "string" && fly) return fly;
  const xff = req.headers["x-forwarded-for"];
  if (typeof xff === "string" && xff) return xff.split(",")[0].trim();
  return req.socket?.remoteAddress || "unknown";
}

// ユーザーが存在しないときも、同じ時間のかかる bcrypt の比較をして、
// 「そのユーザー名があるか」を、応答時間から推測されにくくする。
const DUMMY_HASH = bcrypt.hashSync("dummy-password-for-timing", 10);

function registrationOpen() {
  if (ALLOW_REGISTRATION) return true;
  return db.prepare("SELECT COUNT(*) AS n FROM users").get().n === 0;
}

function sendError(res, status, message, extraHeaders = {}) {
  res.writeHead(status, { "Content-Type": "application/json", ...extraHeaders });
  res.end(JSON.stringify({ error: message }));
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
    // 登録が閉じているとき(2人目以降で、ALLOW_REGISTRATION=1 でない)は、受け付けない
    if (!registrationOpen()) {
      sendError(res, 403, "新規登録は受け付けていません(管理者が許可したときだけ登録できます)");
      return;
    }

    const ipKey = `register:${clientIp(req)}`;
    const wait = lockedForMs(ipKey);
    if (wait > 0) {
      sendError(res, 429, "試行が多すぎます。しばらくしてからやり直してください", {
        "Retry-After": String(Math.ceil(wait / 1000)),
      });
      return;
    }
    recordAttempt(ipKey, MAX_REGISTER_PER_IP);

    const body = await parseBody(req);
    if (typeof body?.username !== "string" || typeof body?.password !== "string" ||
        !body.username || !body.password) {
      sendError(res, 400, "ユーザー名とパスワードが必要です");
      return;
    }
    if (body.username.length > 64) {
      sendError(res, 400, "ユーザー名は 64 文字以内にしてください");
      return;
    }
    // bcrypt は、72 バイトより後ろを無視する。短すぎるパスワードは、総当たりで破られやすい。
    if (body.password.length < 10 || body.password.length > 72) {
      sendError(res, 400, "パスワードは 10 文字以上 72 文字以内にしてください");
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
    if (typeof body?.username !== "string" || typeof body?.password !== "string" ||
        !body.username || !body.password) {
      sendError(res, 400, "ユーザー名とパスワードが必要です");
      return;
    }

    // 失敗が続いているときは、パスワードを確かめる前に、受け付けない(IP ごと・ユーザー名ごと)
    const ipKey = `ip:${clientIp(req)}`;
    const userKey = `user:${body.username.toLowerCase()}`;
    const wait = Math.max(lockedForMs(ipKey), lockedForMs(userKey));
    if (wait > 0) {
      sendError(res, 429, "ログインの失敗が続いたため、しばらくロックしています。後でやり直してください", {
        "Retry-After": String(Math.ceil(wait / 1000)),
      });
      return;
    }

    const user = db.prepare("SELECT * FROM users WHERE username = ?").get(body.username);
    // ユーザーがいなくても、bcrypt の比較は、必ず 1 回行う(応答時間で、ユーザーの有無を悟られないため)
    const passwordOk = bcrypt.compareSync(body.password, user ? user.password_hash : DUMMY_HASH);
    if (!user || !passwordOk) {
      recordAttempt(ipKey, MAX_FAILS_PER_IP);
      recordAttempt(userKey, MAX_FAILS_PER_USER);
      sendError(res, 401, "ユーザー名またはパスワードが間違っています");
      return;
    }
    attempts.delete(userKey); // ログインに成功したら、そのユーザー名の失敗の記録を消す

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

const AUTH_TIMEOUT_MS = Number(process.env.AUTH_TIMEOUT_MS) || 10000;

wss.on("connection", (ws, req) => {
  let role = null;
  let userId = null;
  let username = null;
  let hostId = null;
  let deviceName = null;
  let authenticated = false;
  const remoteIp = clientIp(req);

  // 認証のメッセージを送らない接続は、一定時間で切る(接続だけを増やす嫌がらせへの対策)
  const authTimer = setTimeout(() => {
    if (!authenticated) {
      sendJSON(ws, { type: "error", message: "認証がタイムアウトしました" });
      ws.close();
    }
  }, AUTH_TIMEOUT_MS);
  ws.on("close", () => clearTimeout(authTimer));

  // 「今、このホストに接続している、唯一のクライアント」か。古いクライアントの操作は中継しない。
  const isCurrentClient = () => role === "client" && hostId && clients.get(hostId) === ws;

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
        // すでに別のクライアントがいるときは、古いほうを切る(1 つのホストに、クライアントは 1 つだけ)
        const previous = clients.get(hostId);
        clients.set(hostId, ws);
        if (previous && previous !== ws) {
          sendJSON(previous, { type: "disconnected", message: "別のクライアントが接続しました" });
          try { previous.close(); } catch {}
        }
        ws._hostId = hostId;
        sendJSON(ws, { type: "connected" });//接続完了を送信
        // ホストに接続要求を通知する。ホストは、アカウント名と接続元の IP を表示して、許可を求める。
        // ホストが許可するまで、画面の配信も、入力の受け付けも、始まらない。
        sendJSON(hostEntry.ws, { type: "client-connected", username, clientIp: remoteIp });
        console.log(`Client connected to host: ${hostId} (user=${username}, ip=${remoteIp})`);//ログ出力
        break;
      }

      case "reject-client": {
        // ホスト → サーバー: 接続要求を拒否する。クライアントを切断する。
        if (role !== "host") return;
        const clientWs = clients.get(hostId);
        if (clientWs) {
          clients.delete(hostId);
          sendJSON(clientWs, { type: "disconnected", message: "ホストが接続を拒否しました" });
          try { clientWs.close(); } catch {}
        }
        console.log(`Host rejected client: ${hostId}`);
        break;
      }

      case "input": {
        if (!isCurrentClient()) return;
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
        if (!isCurrentClient()) return;
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
        if (!isCurrentClient()) return;
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
        } else if (isCurrentClient()) {
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
      // 置き換えられた古いクライアントの切断は、新しいクライアントの登録を消さない
      if (clients.get(hostId) === ws) {
        clients.delete(hostId);
        const hostEntry = hosts.get(hostId);
        if (hostEntry) {
          sendJSON(hostEntry.ws, { type: "disconnected", message: "クライアントが切断しました" });
        }
        console.log(`Client disconnected from: ${hostId}`);
      }
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
