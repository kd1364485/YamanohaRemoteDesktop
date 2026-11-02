# Yamanoha Signaling Server

リモートデスクトップの待ち合わせ用サーバー(HTTP の認証 API + WebSocket)。
ホストとクライアントの間で、WebRTC の SDP / ICE と、操作のメッセージを中継する。映像そのものは、WebRTC で、ホストとクライアントが直接やり取りする。

## 必ず設定するもの(環境変数)

| 変数 | 内容 |
|---|---|
| **`JWT_SECRET`** | **必須。32 文字以上のランダムな値。** 未設定、または短いときは、サーバーが起動しない。ログインのトークンの署名に使う。この値を知っている人は、パスワードなしで、他人のトークンを作れる(=その人のホストの画面を見て、操作できる)ので、**ソースコードやリポジトリには、書かない**。 |
| `ALLOW_REGISTRATION` | `1` のときだけ、2 人目以降の新規登録を受け付ける。**既定は「閉じる」**。最初の 1 人だけは、自由に登録できる(初回セットアップ)。 |
| `PORT` | 待ち受けるポート(既定 8080) |
| `DB_PATH` | ユーザー DB(SQLite)の場所(既定 `data.db`)。**DB ファイルは、git に入れない。** |

ランダムな値の作り方(PowerShell):

```powershell
$b = New-Object byte[] 48; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b); [Convert]::ToBase64String($b)
```

## ローカル実行

```powershell
cd signaling-server
npm install
$env:JWT_SECRET = '<上で作った値>'
npm start
```

サーバーは `ws://localhost:8080` で起動する。

## テスト

```powershell
cd signaling-server
npm test
```

認証まわり(署名鍵の必須化、以前の初期値で作ったトークンの拒否、登録の制限、ログインの失敗回数の制限、接続の拒否、クライアントの置き換え)を、サーバーを実際に起動して確かめる。

## fly.io へのデプロイ

```powershell
fly secrets set JWT_SECRET=<値> -a <アプリ名>     # 先に設定する(未設定のままデプロイすると、サーバーが起動しない)
fly secrets list -a <アプリ名>                     # 名前だけが表示される。JWT_SECRET があることを確かめる
fly deploy
```

`JWT_SECRET` を設定(または変更)すると、すでに発行されたトークンは、すべて無効になる(アプリで、ログインし直す)。

## Render へのデプロイ

`render.yaml` で、`JWT_SECRET` を自動生成する設定にしてある。Root Directory は `signaling-server`、Build Command は `npm install`、Start Command は `npm start`。

## 認証・接続の仕組みと、対策

- **ログイン**: ユーザー名とパスワード(bcrypt でハッシュ化して保存)。成功すると、7 日間有効な JWT を返す。
- **ログインの失敗回数の制限**: 15 分の間に、同じユーザー名で 8 回、同じ IP から 20 回失敗すると、15 分間ロックする(HTTP 429、`Retry-After` つき)。成功すると、そのユーザー名の記録は消える。メモリ上の記録なので、サーバーを再起動すると、リセットされる。
- **ユーザーの有無を悟られにくくする**: 存在しないユーザー名でも、bcrypt の比較を 1 回行い、同じエラーメッセージを返す。
- **新規登録**: 既定は閉じる(最初の 1 人だけ登録できる)。パスワードは 10 文字以上 72 文字以内。同じ IP から 15 分に 10 回までの試行。
- **接続できる相手**: クライアントは、**同じアカウントで登録されたホストだけ**を、見つけて接続できる。
- **ホストの許可**: クライアントが接続すると、ホストに `client-connected`(アカウント名と接続元の IP つき)が届く。**ホストのアプリが、許可のダイアログを出し、「許可する」を押すまで、画面の配信も、マウス・キーボードの受け付けも、始まらない。** 拒否すると、ホストが `reject-client` を送り、サーバーがクライアントを切断する。
- **1 つのホストに、クライアントは 1 つだけ**: 新しいクライアントが接続すると、古いクライアントは切断される。古いクライアントの操作は、中継されない。
- **WebSocket の認証**: 接続後 10 秒以内に、`auth` を送らない接続は、切断される。
- **リクエストのサイズ**: 16KB を超える HTTP のボディは、拒否する。

## プロトコル(WebSocket、すべて JSON)

最初のメッセージは、必ず `auth`。

| 方向 | メッセージ | 内容 |
|---|---|---|
| 共通 → サーバー | `{ type: "auth", token }` | JWT で認証。成功すると `{ type: "auth-ok", username }` |
| ホスト → サーバー | `{ type: "host", deviceName, instanceId }` | ホストとして登録。`{ type: "host-ready", hostId, deviceName }` が返る |
| クライアント → サーバー | `{ type: "list-hosts" }` | 自分のアカウントのホスト一覧。`{ type: "host-list", hosts }` が返る |
| クライアント → サーバー | `{ type: "connect", hostId }` | ホストに接続。`{ type: "connected" }` が返り、ホストに `client-connected` が届く |
| サーバー → ホスト | `{ type: "client-connected", username, clientIp }` | クライアントが接続した(許可を求める) |
| ホスト → サーバー | `{ type: "reject-client" }` | 接続を拒否する。クライアントは切断される |
| クライアント → ホスト | `{ type: "input", action, ... }` | マウス・キーボード(ホストが許可するまで、無視される) |
| クライアント → ホスト | `{ type: "select-display", displayId }` | 配信するディスプレイの切り替え |
| ホスト → クライアント | `{ type: "screen-info" }`, `{ type: "display-list" }`, `{ type: "cursor" }` | 画面の情報 |
| 双方向 | `{ type: "webrtc-offer" }`, `{ type: "webrtc-answer" }`, `{ type: "webrtc-ice" }` | WebRTC のシグナリング(offer はホスト → クライアント、answer はクライアント → ホスト) |
| サーバー → 相手 | `{ type: "disconnected", message }` | 相手が切断した、または拒否・置き換えられた |
| サーバー → 本人 | `{ type: "error", message }` | エラー |

HTTP: `GET /health`、`POST /api/register`、`POST /api/login`(いずれも JSON で、成功すると `{ token, username }`)。
