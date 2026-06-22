// アプリ全体で共有する可変状態。
// オブジェクトは参照渡しのため、各モジュールから state.xxx で読み書きできる。
export const state = {
  // 現在選択中のフォルダパス
  currentPath: "",
  // チャット送信中フラグ（多重送信防止）
  isChatting: false,
  // インデックスが構築済みかどうか（自動増分更新の判定に使う）
  indexBuilt: false,
  // 増分更新の二重実行を防ぐフラグ
  reindexing: false,
  // 保存済みプロンプトのテキスト（変更検知用）
  savedPromptText: "",
  // 拡張子リスト
  extensionsList: [] as string[],
  // マジックコメント処理中のファイル（同一ファイルの多重処理を防ぐ）
  processingMagicFiles: new Set<string>(),
};
