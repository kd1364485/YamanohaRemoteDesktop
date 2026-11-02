# YamanohaRemoteDesktop

Windows PC の画面を、WebRTC でスマートフォンやブラウザへ低遅延で配信し、手元から操作できるリモートデスクトップです。
画面キャプチャ、GPU エンコード、ネットワーク配信、入力の逆送信までを、自分で組み上げています。

- 期間: 2026/06/22 〜 07/08(25 コミット)
- 対象 OS: Windows(Desktop Duplication API / Direct3D 11 / NVENC / Media Foundation を使用)
- 動作確認した操作端末: Windows、Google Pixel 9(タッチ操作、無線キーボード)

## 何を作ったか

```
 ┌─────────────── ホスト PC (Windows) ────────────────┐         ┌──────── クライアント ────────┐
 │ Tauri アプリ (Rust + TypeScript)                    │         │ ブラウザ / スマホ (TS)        │
 │   │ FFI (C API)                                     │         │                              │
 │   ▼                                                 │  WebRTC │  映像を表示                  │
 │ yamanoha_core.dll (C++20)                           │ ──────▶ │  マウス/キー/タッチを送信     │
 │   Capture → Encode → Send   (3 スレッド)            │ ◀────── │                              │
 │   └ libdatachannel (DTLS-SRTP / mbedTLS)            │ DataCh. └──────────────▲───────────────┘
 └──────────────▲──────────────────────────────────────┘                        │
                │ WebSocket (SDP / ICE / 操作メッセージ)                         │
                └──────────────  シグナリングサーバー (Node.js)  ────────────────┘
                                 JWT 認証 / bcrypt / SQLite / TURN 対応
```

| 層 | 技術 | 場所 |
|---|---|---|
| 配信エンジン | C++20、Direct3D 11、DXGI Desktop Duplication、NVENC、Media Foundation、libdatachannel | `native/` |
| ホストアプリ | Rust、Tauri 2(C++ DLL を FFI で呼び出し、`SendInput` で入力を再現) | `src-tauri/` |
| クライアント | TypeScript、Vite、Tailwind(マウス・キー・タッチ・ディスプレイ切り替え) | `src/` |
| シグナリング | Node.js、ws、JWT、bcrypt、better-sqlite3(Render / Fly.io 用の設定つき) | `signaling-server/` |

## 設計のポイント

### 1. 3 スレッドのパイプライン(`native/src/pipeline/frame_pipeline.cpp`)

```
CaptureLoop ──▶ EncoderLoop ──▶ SenderLoop
 (画面取得)       (H.264 化)      (WebRTC へ送信)
```

- キャプチャ、エンコード、送信を別スレッドに分け、`BlockingQueue` と mutex / condition_variable / atomic でつないでいます。
- **遅延を溜めない**: 処理中のフレームが 3 枚(`kMaximumFramesInFlight`)に達したら、新しいフレームのキャプチャを**スキップ**します。詰まったときに古い映像を送り続けて遅れが増えるのを避けるためです。
- **静止画面への対策**: 画面に変化がないときはエンコードを省略しますが、**3 秒ごとにキーフレームを強制**します(コミット「video freeze on static desktop」)。途中参加や欠落からの復帰を保証し、クライアント側でもフリーズを検知します。
- **送信失敗からの復帰**: 送信に失敗したら、次のフレームをキーフレームにして、受信側のデコーダを再同期させます。
- エンコード用のテクスチャは **4 枚のプールを事前確保**して使い回します。フレームごとの確保コストを避けるためです。

### 2. 複数 GPU 環境への対応(ハイブリッド GPU)

ノート PC のように、画面を出している GPU(Intel など)と、NVENC を持つ GPU(NVIDIA)が別のことがあります。

- エンコード用のデバイスは **NVIDIA を優先して自動選択**し、見つからなければキャプチャと同じアダプタを使います。
- GPU をまたぐときは、キャプチャ GPU → CPU 上のステージングテクスチャ → エンコード GPU の順でコピーします(`CopyToEncodeTexture`)。この途中でカーソルも合成し、画面の回転(縦向きディスプレイ)にも対応しています。

### 3. エンコーダの抽象化とフォールバック

- `VideoEncoder` という抽象クラスの下に、`NvencEncoder`(NVENC SDK を直接使用)と `MfEncoder`(Media Foundation)を置いています。
- **NVENC を優先し、失敗したら Media Foundation に自動で切り替えます。** これで NVIDIA 以外の GPU(Intel QuickSync / AMD など)でも動きます(コミット「Nvidia GPU以外でもOK」)。
- 設定は低遅延重視です(P1 プリセット、B フレームなし、CBR、GOP 30)。

### 4. 動的ビットレート(`adaptive_bitrate_controller.cpp`)

- 受信側が推定した帯域の **85%** を目標ビットレートにします(余裕を残すためです)。
- 変更は **1 秒に 1 回まで**、かつ**現在値から 10% 以上(最低 100kbps)**変わるときだけです。細かい揺れで再設定を繰り返さないようにしています。
- 変更したあとは、画質の急変をやわらげるためにキーフレームを要求します。
- この部分は `native/test/pipeline_primitives_test.cpp` で、帯域が減る場合と回復する場合の動きをテストしています。

### 5. 複数ディスプレイと切り替え

- ディスプレイを列挙して、クライアントから選べます。
- 切り替えたときは、**WebRTC の接続を保ったまま**、パイプラインだけを作り直します(`DisplaySessionManager::SwitchDisplay`)。

### 6. C++ と Rust の境界

- C++ 側は `extern "C"` の小さな C API(`core.h`)だけを公開し、Rust(Tauri)から FFI で呼び出します。コールバック(SDP、ICE、メッセージ、接続状態)は関数ポインタで渡します。
- DLL(`yamanoha_core.dll`、`datachannel.dll`)は、Rust の `build.rs` が出力先に自動コピーします。

### 7. WebRTC まわりの調整

- libdatachannel(v0.24.5)を、暗号ライブラリ mbedTLS(v3.6.6)で構成し、CMake の `FetchContent` で取得しています。
- **mbedTLS の TLS 1.3 を無効化**しています。mbedTLS は DTLS 1.3 を実装していないため、有効だと DTLS のハンドシェイクが `MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE` で失敗するためです(`CMakeLists.txt` に理由を記載)。
- 公開環境向けに TURN サーバー対応も入れています。

### 8. 認証とシグナリング

- シグナリングサーバーは、ユーザー登録とログイン(bcrypt でハッシュ化、JWT を発行)を持ち、WebSocket の接続時にトークンを検証します。
- ホストとクライアントの役割を分け、役割に合わないメッセージは処理しません(例:`input` はクライアントからのみ受け付ける)。
- 接続の生死を、ping / pong のハートビートで確認しています。

## ビルドと実行

前提: Windows、Visual Studio(MSVC)、CMake 3.20 以上、Rust、Node.js。NVENC を使うには NVIDIA GPU が必要です(なければ Media Foundation に切り替わります)。

```powershell
# 1. C++ のコアライブラリ(依存の libdatachannel / mbedTLS は自動取得)
cmake -S native -B native/build
cmake --build native/build --config Release

# 2. ホストアプリ(Tauri)
npm install
npm run tauri dev

# 3. シグナリングサーバー(ローカル)
cd signaling-server
npm install
npm start          # 既定のポートは 8080(環境変数 PORT で変更)
```

JWT の署名鍵は、環境変数 `JWT_SECRET` で指定してください(未指定時は開発用の既定値が使われます)。

### テスト

`native/test/` に 4 つのテスト用プログラムがあります。

| ファイル | 内容 |
|---|---|
| `capture_test.cpp` | 画面キャプチャ |
| `encode_test.cpp` | キャプチャからエンコードまで |
| `webrtc_test.cpp` | WebRTC 配信の統合テスト |
| `pipeline_primitives_test.cpp` | 動的ビットレート制御と `BlockingQueue`(GPU 不要) |

## 開発の流れ(コミット履歴より)

| 日付 | 内容 |
|---|---|
| 06/22 | 土台づくり、スマホ(Pixel 9)での操作 |
| 06/24 | アカウント認証 |
| 06/25 | 同一 Wi-Fi の制限を解除、無線キーボード対応 |
| 07/01 | プロトタイプ完成(安定性に懸念あり)、カーソル表示、リファクタリング |
| 07/02 | マルチディスプレイ、切り替えの高速化、安定化、静止画面のフリーズ修正、Media Foundation へのフォールバック、TURN、動的ビットレート |
| 07/08 | コメント追加、リファクタリング |

## 現状の制限と今後

- **性能の測定値は、まだありません。** エンドツーエンドの遅延(キャプチャからクライアント表示まで)、FPS、各段階の処理時間を測り、ボトルネックを数値で示すのが、次にやることです。パイプラインには処理段階ごとのカウンタ(`captured` / `encoded` / `sent` など)があるので、それを土台にできます。
- GPU をまたぐコピーが CPU 経由なので、ハイブリッド GPU 環境では、ここが遅延の候補です(未測定)。
- 自動テストは、GPU を使わない部分(ビットレート制御とキュー)に限られます。キャプチャとエンコードのテストは、実機の GPU が必要な手動のものです。
- `signaling-server/README.md` のプロトコル表は、WebSocket でフレームを送っていた初期の設計のままで、現在の WebRTC 方式(`webrtc-offer` / `webrtc-answer` / `webrtc-ice` など)に追いついていません。
- Windows 専用です。
