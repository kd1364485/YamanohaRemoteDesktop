# Yamanoha Signaling Server

リモートデスクトップの待ち合わせ用WebSocketサーバー。

## ローカル実行

```bash
cd signaling-server
npm install
npm start
```

サーバーは `ws://localhost:8080` で起動します。

## Render へのデプロイ

1. このプロジェクトをGitHubにpush
2. [Render](https://render.com) で「New > Web Service」を作成
3. リポジトリを選択し、Root Directoryを `signaling-server` に設定
4. Build Command: `npm install`
5. Start Command: `npm start`
6. Plan: Free

デプロイ後、`wss://your-app.onrender.com` でWebSocket接続可能になります。

## プロトコル

### ホスト → サーバー
| メッセージ | 内容 |
|---|---|
| `{ type: "host" }` | ホストとして登録、接続コードを受け取る |
| `{ type: "frame", data: "..." }` | キャプチャした画面フレームをクライアントへ転送 |
| `{ type: "screen-info", width, height }` | 画面サイズ情報をクライアントへ送信 |

### クライアント → サーバー
| メッセージ | 内容 |
|---|---|
| `{ type: "connect", code: "XXXXXX" }` | 接続コードでホストに接続 |
| `{ type: "input", action: "mouse-move", x, y }` | マウス移動 |
| `{ type: "input", action: "mouse-click", button, x, y }` | マウスクリック |
| `{ type: "input", action: "key", key, isDown }` | キー入力 |

### サーバー → クライアント/ホスト
| メッセージ | 内容 |
|---|---|
| `{ type: "host-ready", code }` | ホストに接続コードを通知 |
| `{ type: "connected" }` | クライアントに接続成功を通知 |
| `{ type: "client-connected" }` | ホストにクライアント接続を通知 |
| `{ type: "disconnected", message }` | 相手の切断を通知 |
| `{ type: "error", message }` | エラー通知 |
