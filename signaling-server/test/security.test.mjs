// signaling-server の認証まわりのテスト。外部のサービスは使わず、サーバーを子プロセスで起動して確かめる。
// 実行: cd signaling-server && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { createServer } from "node:net";
import WebSocket from "ws";
import jwt from "jsonwebtoken";

const SERVER_JS = fileURLToPath(new URL("../server.js", import.meta.url));
const GOOD_SECRET = "test-secret-".padEnd(48, "x");
const OLD_DEFAULT_SECRET = "yamanoha-remote-desktop-secret-change-me"; // 以前、ソースに書かれていた初期値
const TEMP_PREFIX = "yamanoha-test-";

function freePort() {
  return new Promise((resolve) => {
    const s = createServer();
    s.listen(0, "127.0.0.1", () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
  });
}

// テストが作った一時フォルダだけを消す。(OS の一時フォルダの直下にあり、名前がこのテストのものでなければ、消さない)
function removeTempDir(dir) {
  if (dirname(dir) === tmpdir() && basename(dir).startsWith(TEMP_PREFIX)) {
    rmSync(dir, { recursive: true, force: true });
  }
}

// サーバーを起動して、待ち受けが始まるのを待つ
async function startServer(env = {}) {
  const dir = mkdtempSync(join(tmpdir(), TEMP_PREFIX));
  const port = await freePort();
  const proc = spawn(process.execPath, [SERVER_JS], {
    env: {
      PATH: process.env.PATH,
      PORT: String(port),
      DB_PATH: join(dir, "test.db"),
      JWT_SECRET: GOOD_SECRET,
      ...env,
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdout = "";
  let stderr = "";
  proc.stdout.on("data", (d) => (stdout += d));
  proc.stderr.on("data", (d) => (stderr += d));
  const exited = new Promise((resolve) => proc.on("exit", (code) => resolve(code)));

  let pollTimer;
  const ready = await Promise.race([
    new Promise((resolve) => {
      pollTimer = setInterval(() => {
        if (stdout.includes("Signaling server running")) resolve(true);
      }, 25);
    }),
    exited.then(() => false),
  ]);
  clearInterval(pollTimer);
  const stop = async () => {
    proc.kill();
    await exited;
    removeTempDir(dir);
  };
  return { port, exited, ready, stop, cleanup: () => removeTempDir(dir), out: () => ({ stdout, stderr }) };
}

async function post(port, path, body, headers = {}) {
  const res = await fetch(`http://127.0.0.1:${port}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json", ...headers },
    body: typeof body === "string" ? body : JSON.stringify(body),
  });
  let json = null;
  try { json = await res.json(); } catch {}
  return { status: res.status, json, headers: res.headers };
}

const register = (port, username, password = "correct-horse-battery") =>
  post(port, "/api/register", { username, password });
const login = (port, username, password) => post(port, "/api/login", { username, password });

// WebSocket のメッセージを、順に取り出せるようにする
function openWs(port) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}`);
    const queue = [];
    const waiters = [];
    ws.on("message", (raw, isBinary) => {
      if (isBinary) return;
      const msg = JSON.parse(raw.toString());
      const w = waiters.findIndex((x) => x.pred(msg));
      if (w >= 0) waiters.splice(w, 1)[0].resolve(msg);
      else queue.push(msg);
    });
    const closed = new Promise((res) => ws.on("close", () => res(true)));
    ws.on("error", reject);
    ws.on("open", () =>
      resolve({
        ws,
        closed,
        send: (obj) => ws.send(JSON.stringify(obj)),
        next: (pred = () => true, timeoutMs = 3000) =>
          new Promise((res, rej) => {
            const i = queue.findIndex(pred);
            if (i >= 0) return res(queue.splice(i, 1)[0]);
            const waiter = { pred, resolve: res };
            waiters.push(waiter);
            setTimeout(() => {
              const k = waiters.indexOf(waiter);
              if (k >= 0) { waiters.splice(k, 1); rej(new Error("timeout waiting for message")); }
            }, timeoutMs);
          }),
        // 一定時間、条件に合うメッセージが来ないことを確かめる
        expectNone: async (pred, ms = 400) => {
          await new Promise((r) => setTimeout(r, ms));
          assert.equal(queue.findIndex(pred), -1, "来てはいけないメッセージが来た");
        },
      })
    );
  });
}

async function authedWs(port, token) {
  const c = await openWs(port);
  c.send({ type: "auth", token });
  await c.next((m) => m.type === "auth-ok");
  return c;
}

// ---------------------------------------------------------------------------

test("JWT_SECRET が未設定なら、起動しない", async () => {
  const s = await startServer({ JWT_SECRET: "" });
  const code = await s.exited;
  assert.equal(s.ready, false);
  assert.equal(code, 1);
  assert.match(s.out().stderr, /JWT_SECRET/);
  s.cleanup();
});

test("JWT_SECRET が 32 文字未満なら、起動しない", async () => {
  const s = await startServer({ JWT_SECRET: "short-secret" });
  const code = await s.exited;
  assert.equal(s.ready, false);
  assert.equal(code, 1);
  s.cleanup();
});

test("以前の初期値で署名したトークンは、拒否される(パスワードなしでは入れない)", async () => {
  const s = await startServer();
  try {
    assert.ok(s.ready);
    const forged = jwt.sign({ userId: 1, username: "someone" }, OLD_DEFAULT_SECRET);
    const c = await openWs(s.port);
    c.send({ type: "auth", token: forged });
    const msg = await c.next((m) => m.type === "error");
    assert.match(msg.message, /無効なトークン/);
    await c.closed;
  } finally {
    await s.stop();
  }
});

test("正しい署名鍵のトークンなら、認証できる", async () => {
  const s = await startServer();
  try {
    const reg = await register(s.port, "owner");
    assert.equal(reg.status, 201);
    const c = await authedWs(s.port, reg.json.token);
    c.ws.close();
  } finally {
    await s.stop();
  }
});

test("登録: 最初の 1 人だけ登録できて、2 人目は拒否される", async () => {
  const s = await startServer();
  try {
    assert.equal((await register(s.port, "owner")).status, 201);
    const second = await register(s.port, "intruder");
    assert.equal(second.status, 403);
    assert.match(second.json.error, /登録は受け付けていません/);
  } finally {
    await s.stop();
  }
});

test("登録: ALLOW_REGISTRATION=1 なら、2 人目も登録できる", async () => {
  const s = await startServer({ ALLOW_REGISTRATION: "1" });
  try {
    assert.equal((await register(s.port, "owner")).status, 201);
    assert.equal((await register(s.port, "friend")).status, 201);
  } finally {
    await s.stop();
  }
});

test("登録: 短いパスワード・長すぎるユーザー名・文字列でない値は、400", async () => {
  const s = await startServer();
  try {
    assert.equal((await post(s.port, "/api/register", { username: "a", password: "short" })).status, 400);
    assert.equal((await post(s.port, "/api/register", { username: "a".repeat(65), password: "long-enough-password" })).status, 400);
    assert.equal((await post(s.port, "/api/register", { username: { $ne: 1 }, password: "long-enough-password" })).status, 400);
    assert.equal((await post(s.port, "/api/register", "{ not json")).status, 400);
    // 失敗しても、登録の枠(最初の 1 人)は、使われない
    assert.equal((await register(s.port, "owner")).status, 201);
  } finally {
    await s.stop();
  }
});

test("大きすぎるリクエストは、400 で拒否される", async () => {
  const s = await startServer();
  try {
    const big = JSON.stringify({ username: "owner", password: "x".repeat(40000) });
    assert.equal((await post(s.port, "/api/login", big)).status, 400);
  } finally {
    await s.stop();
  }
});

test("ログイン: 同じユーザー名で 8 回失敗すると、正しいパスワードでもロックされる", async () => {
  const s = await startServer();
  try {
    await register(s.port, "owner", "correct-horse-battery");
    for (let i = 0; i < 8; i++) {
      assert.equal((await login(s.port, "owner", "wrong-password")).status, 401);
    }
    const locked = await login(s.port, "owner", "correct-horse-battery");
    assert.equal(locked.status, 429);
    assert.ok(Number(locked.headers.get("retry-after")) > 0);
  } finally {
    await s.stop();
  }
});

test("ログイン: 成功すると、そのユーザー名の失敗の記録が消える", async () => {
  const s = await startServer();
  try {
    await register(s.port, "owner", "correct-horse-battery");
    for (let i = 0; i < 5; i++) await login(s.port, "owner", "wrong-password");
    assert.equal((await login(s.port, "owner", "correct-horse-battery")).status, 200);
    for (let i = 0; i < 5; i++) {
      assert.equal((await login(s.port, "owner", "wrong-password")).status, 401);
    }
    assert.equal((await login(s.port, "owner", "correct-horse-battery")).status, 200);
  } finally {
    await s.stop();
  }
});

test("ログイン: 同じ IP から 20 回失敗すると、別のユーザー名でもロックされる", async () => {
  const s = await startServer();
  try {
    await register(s.port, "owner", "correct-horse-battery");
    for (let i = 0; i < 20; i++) {
      assert.equal((await login(s.port, `ghost-${i}`, "whatever-password")).status, 401);
    }
    assert.equal((await login(s.port, "owner", "correct-horse-battery")).status, 429);
  } finally {
    await s.stop();
  }
});

test("ログイン: 存在しないユーザーと、パスワード違いは、同じエラーメッセージ", async () => {
  const s = await startServer();
  try {
    await register(s.port, "owner", "correct-horse-battery");
    const a = await login(s.port, "owner", "wrong-password");
    const b = await login(s.port, "nobody", "wrong-password");
    assert.equal(a.status, 401);
    assert.equal(b.status, 401);
    assert.equal(a.json.error, b.json.error);
  } finally {
    await s.stop();
  }
});

test("認証メッセージを送らない WebSocket 接続は、時間が来たら切られる", async () => {
  const s = await startServer({ AUTH_TIMEOUT_MS: "300" });
  try {
    const c = await openWs(s.port);
    const msg = await c.next((m) => m.type === "error", 2000);
    assert.match(msg.message, /タイムアウト/);
    await c.closed;
  } finally {
    await s.stop();
  }
});

test("接続要求: ホストに、クライアントのアカウント名と IP が届く。同じアカウントだけが接続できる", async () => {
  const s = await startServer({ ALLOW_REGISTRATION: "1" });
  try {
    const owner = (await register(s.port, "owner")).json.token;
    const other = (await register(s.port, "other")).json.token;

    const host = await authedWs(s.port, owner);
    host.send({ type: "host", deviceName: "TestPC" });
    const ready = await host.next((m) => m.type === "host-ready");

    // 別のアカウントは、ホストを見つけられない
    const stranger = await authedWs(s.port, other);
    stranger.send({ type: "list-hosts" });
    assert.deepEqual((await stranger.next((m) => m.type === "host-list")).hosts, []);
    stranger.send({ type: "connect", hostId: ready.hostId });
    assert.match((await stranger.next((m) => m.type === "error")).message, /ホストが見つかりません/);
    await host.expectNone((m) => m.type === "client-connected");

    // 同じアカウントは、接続できる。ホストに、誰が・どこから来たかが届く
    const client = await authedWs(s.port, owner);
    client.send({ type: "connect", hostId: ready.hostId });
    await client.next((m) => m.type === "connected");
    const notice = await host.next((m) => m.type === "client-connected");
    assert.equal(notice.username, "owner");
    assert.equal(typeof notice.clientIp, "string");
    assert.ok(notice.clientIp.length > 0);
  } finally {
    await s.stop();
  }
});

test("拒否: ホストが reject-client を送ると、クライアントは切断され、入力は届かない", async () => {
  const s = await startServer();
  try {
    const token = (await register(s.port, "owner")).json.token;
    const host = await authedWs(s.port, token);
    host.send({ type: "host", deviceName: "TestPC" });
    const { hostId } = await host.next((m) => m.type === "host-ready");

    const client = await authedWs(s.port, token);
    client.send({ type: "connect", hostId });
    await host.next((m) => m.type === "client-connected");

    host.send({ type: "reject-client" });
    const bye = await client.next((m) => m.type === "disconnected");
    assert.match(bye.message, /拒否/);
    await client.closed;

    // 拒否したあとに、(切断済みの)クライアントが送った入力は、ホストに届かない
    if (client.ws.readyState === WebSocket.OPEN) {
      client.send({ type: "input", action: "mouse-move", x: 1, y: 1 });
    }
    await host.expectNone((m) => m.type === "input");
  } finally {
    await s.stop();
  }
});

test("置き換え: 新しいクライアントが接続すると、古いクライアントは切られ、入力は新しいほうだけが届く", async () => {
  const s = await startServer();
  try {
    const token = (await register(s.port, "owner")).json.token;
    const host = await authedWs(s.port, token);
    host.send({ type: "host", deviceName: "TestPC" });
    const { hostId } = await host.next((m) => m.type === "host-ready");

    const first = await authedWs(s.port, token);
    first.send({ type: "connect", hostId });
    await first.next((m) => m.type === "connected");
    await host.next((m) => m.type === "client-connected");

    const second = await authedWs(s.port, token);
    second.send({ type: "connect", hostId });
    await second.next((m) => m.type === "connected");
    await host.next((m) => m.type === "client-connected");

    const bye = await first.next((m) => m.type === "disconnected");
    assert.match(bye.message, /別のクライアント/);
    await first.closed;

    // 古いクライアントの切断で、新しいクライアントの登録が消えていない: 入力が、新しいほうから届く
    second.send({ type: "input", action: "mouse-move", x: 5, y: 6 });
    const input = await host.next((m) => m.type === "input");
    assert.equal(input.x, 5);
  } finally {
    await s.stop();
  }
});
