# Synapse - プロジェクト技術ドキュメント

## 概要

Synapseは、Tauri 2.0を使用して構築されたデスクトップアプリケーションです。ローカルのソースコードプロジェクトを監視し、Google Gemini APIを活用したRAG（Retrieval-Augmented Generation）検索により、AIがプロジェクト全体を理解して質問に答えることができます。

## 技術スタック

### フロントエンド
- **フレームワーク**: Tauri 2.0 (デスクトップアプリケーションフレームワーク)
- **ビルドツール**: Vite 6.0.3
- **言語**: TypeScript 5.6.2
- **スタイリング**: TailwindCSS 4.3.0
- **UIライブラリ**: カスタムHTML/CSS（TailwindCSSベース）

### バックエンド (Rust)
- **Tauri**: 2.0
- **HTTPクライアント**: reqwest 0.12
- **ファイル監視**: notify 6
- **並列処理**: rayon 1.8
- **シリアライズ**: serde 1.0, serde_json 1.0
- **非同期ランタイム**: tokio 1.0
- **ファイルシステム**: walkdir 2, ignore 0.4

### 外部API
- **Google Gemini API**: チャット生成 (gemini-2.5-flash, gemini-3.5-flashなど)
- **Gemini Embedding API**: ベクトル化 (gemini-embedding-001)

## アーキテクチャ

### 全体構成

```
┌─────────────────────────────────────────────────────────┐
│                    フロントエンド (TypeScript)            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐ │
│  │   UI層       │  │  状態管理     │  │  設定管理     │ │
│  │  (HTML/TSX)  │  │ (localStorage)│  │ (localStorage)│ │
│  └──────────────┘  └──────────────┘  └──────────────┘ │
└──────────────────────────┬──────────────────────────────┘
                           │ Tauri IPC
┌──────────────────────────┴──────────────────────────────┐
│                  バックエンド (Rust)                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐ │
│  │ ファイル監視 │  │ RAG検索エンジン│  │ ベクトルストア│ │
│  │   (notify)   │  │  (embedding)  │  │ (永続化JSON) │ │
│  └──────────────┘  └──────────────┘  └──────────────┘ │
└──────────────────────────┬──────────────────────────────┘
                           │
┌──────────────────────────┴──────────────────────────────┐
│                  外部API                                │
│  ┌──────────────┐  ┌──────────────┐                    │
│  │ Gemini Chat  │  │ Gemini Embed │                    │
│  └──────────────┘  └──────────────┘                    │
└─────────────────────────────────────────────────────────┘
```

### データフロー

1. **ファイル監視**
   - ユーザーがフォルダを選択
   - Rustの`notify`クレートでファイル変更を監視
   - 変更検知時にプロジェクト状態を更新

2. **インデックス構築**
   - ソースコードを40行単位でチャンク分割（10行オーバーラップ）
   - Gemini Embedding APIで各チャンクをベクトル化
   - ベクトルストアをJSONファイルに永続化（アプリデータディレクトリ）

3. **RAG検索**
   - ユーザーの質問をベクトル化
   - コサイン類似度で関連チャンクをTop-K検索
   - 関連チャンクの内容をプロンプトに埋め込み

4. **AI回答生成**
   - プロジェクト概要、ファイルツリー、関連チャンクを含むプロンプトを構築
   - Gemini APIに送信して回答を生成

## 主要機能

### 1. ファイル監視
- **拡張子フィルタリング**: ユーザーがカスタマイズ可能（デフォルト: js, ts, py, rs, cppなど）
- **ディレクトリ除外**: node_modules, .git, targetなどのビルド成果物を自動除外
- **デバウンス**: 500ms以内の変更イベントを統合して過剰な更新を防止
- **自動状態更新**: ファイル変更時にプロジェクト状態キャッシュを更新

### 2. ベクトルインデックス
- **チャンク分割**: 40行/チャンク、10行オーバーラップ
- **差分更新**: 内容が同じチャンクはベクトルを再利用（APIコスト削減）
- **並列処理**: 10バッチ並列でembedding（EMBED_CONCURRENCY=10）
- **レート制限対策**: 429エラー時に指数バックオフで自動リトライ（最大5回）
- **永続化**: アプリデータディレクトリにvector_store.jsonとして保存

### 3. RAG検索
- **クエリベクトル化**: ユーザー質問をgemini-embedding-001でベクトル化
- **類似度計算**: コサイン類似度で並列計算（rayon使用）
- **Top-K検索**: デフォルト8件の関連チャンクを取得
- **フォールバック**: インデックス未構築時は全ソースコードを使用

### 4. AIチャット
- **モデル選択**: gemini-2.5-flash, gemini-3.5-flashなどを選択可能
- **プロンプトカスタマイズ**: ユーザーがAIの役割や指示をカスタマイズ可能
- **送信中制御**: AI回答中は送信ボタンを無効化
- **エラーハンドリング**: API制限、ネットワークエラーに対応

### 5. マジックコメント
- **自動コード生成**: ファイル保存時に「// Synapse:」で始まるコメントを検知
- **RAG統合**: 関連コードチャンクをコンテキストとして使用
- **インデント維持**: 生成コードに元のインデントを適用
- **指示文保持**: 「Synapse:」のみを削除し、指示文をコメントとして残す
- **コード挿入**: 生成コードをマジックコメント行の後に挿入
- **並行処理防止**: 同一ファイルの多重処理を防止（フロントエンド/バックエンド両方）
- **UIフィードバック**: トースト通知で処理状況を表示

### 6. トースト通知
- **画面右上表示**: 固定位置にポップアップ通知
- **3種類のタイプ**: info（青）、success（緑）、error（赤）
- **アニメーション**: CSS transitionで右から左にスライドイン/アウト
- **自動削除**: 指定時間（デフォルト3秒）後に自動消滅

### 7. 設定管理
- **localStorage**: APIキー、モデル選択、プロンプト、選択パスをブラウザに保存
- **Rust側設定**: 拡張子リストをextensions.jsonに保存
- **自動復元**: アプリ起動時に設定を自動読み込み

## データ構造

### Rust側主要構造体

```rust
// プロジェクト状態
struct ProjectState {
    overview: String,        // プロジェクト概要
    file_tree: String,       // ファイルツリー
    files_content: Vec<FileContent>,
}

// ファイル内容
struct FileContent {
    path: String,
    content: String,
}

// コードチャンク
struct CodeChunk {
    path: String,
    start_line: usize,
    end_line: usize,
    content: String,
}

// ベクトル化されたチャンク
struct EmbeddedChunk {
    chunk: CodeChunk,
    embedding: Vec<f32>,  // 768次元ベクトル
}

// マジックコメント処理リクエスト
struct MagicCommentRequest {
    api_key: String,
    file_path: String,
    model: String,
}
```

### TypeScript側主要変数

```typescript
// 設定キー
const API_KEY_STORAGE_KEY = "gemini_api_key";
const PROMPT_STORAGE_KEY = "gemini_prompt";
const MODEL_STORAGE_KEY = "gemini_model";
const PATH_STORAGE_KEY = "selected_path";

// UI状態
let isChatting = false;  // 送信中フラグ
let extensionsList: string[] = [];  // 拡張子リスト
let processingMagicFiles: Set<string> = new Set();  // マジックコメント処理中ファイル
let toastContainer: HTMLElement | null;  // トースト通知コンテナ
```

### Rust側主要定数

```rust
const MAGIC_MARKER: &str = "Synapse:";  // マジックコメントマーカー
```

## Tauriコマンド

### フロントエンド→バックエンド

1. **get_extensions()**: 拡張子リストを取得
2. **save_extensions(extensions)**: 拡張子リストを保存
3. **get_project_state()**: プロジェクト状態を取得
4. **start_watching(path)**: ファイル監視を開始
5. **stop_watching()**: ファイル監視を停止
6. **build_vector_index(api_key)**: ベクトルインデックスを構築
7. **reindex_changed_chunks(api_key)**: 変更チャンクのみ再ベクトル化
8. **has_vector_index()**: インデックス構築済みか確認
9. **search_similar_chunks(api_key, query, top_k)**: RAG検索
10. **ask_gemini(request)**: Gemini APIでチャット
11. **process_magic_comment(request)**: マジックコメントを処理してコード生成

### バックエンド→フロントエンド

- **project-updatedイベント**: ファイル変更時にフロントエンドに通知
- **magic-comment-detectedイベント**: マジックコメント検知時にフロントエンドに通知

## パフォーマンス最適化

### 並列処理
- ファイル読み込み: rayonで並列処理
- チャンク分割: rayonで並列処理
- 類似度計算: rayonで並列処理
- Embedding: 10バッチ並列実行

### メモリ管理
- プロジェクト状態キャッシュ: Mutexで保護
- ベクトルストア: Mutexで保護
- 拡張子キャッシュ: Lazyで初期化

### APIコスト削減
- 差分更新: 内容が同じチャンクはベクトルを再利用
- チャンク分割: 40行単位で細かく分割してTPM制限対策
- レート制限対策: 自動リトライと指数バックオフ

## セキュリティ

- **APIキー**: localStorageに保存（ローカル環境のみ）
- **CSP**: tauri.conf.jsonで無効化（デスクトップアプリのため）
- **ファイルアクセス**: Tauriの権限システムで制御

## 開発環境

### ビルドコマンド
```bash
npm run dev          # 開発サーバー起動
npm run build        # プロダクションビルド
npm run tauri dev    # Tauri開発モード
npm run tauri build  # Tauriビルド
```

### ポート
- Vite dev server: 1420
- HMR: 1421

## 既知の制限

1. **Embeddingモデル**: gemini-embedding-001を使用（gemini-embedding-2はレート制限が厳しすぎて実用的でない）
2. **チャットキャンセル**: Tauriのinvokeはキャンセル機能がないため、送信中は新しいリクエストを送信できない
3. **ファイルサイズ**: 大規模プロジェクトではTPM制限に達する可能性
4. **インデックス互換性**: 異なるembeddingモデル間でベクトルは互換性がない

## 今後の改善案

1. **ストリーミングレスポンス**: Gemini APIのストリーミング機能を活用
2. **チャット履歴**: 会話履歴の保存と読み込み
3. **複数プロジェクト**: 複数のプロジェクトを切り替え可能に
4. **インクリメンタル検索**: タイプ中にリアルタイムで検索
5. **コードハイライト**: AI回答内のコードブロックをシンタックスハイライト
