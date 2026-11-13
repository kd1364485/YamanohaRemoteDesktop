// 共通型定義・定数

export type ViewId = "login-view" | "register-view" | "home-view" | "host-view" | "connect-view" | "remote-view";

export type HostInfo = { hostId: string; deviceName: string };

export type DisplayInfo = { id: string; label: string; width: number; height: number; primary: boolean };

// クラウド（fly.io）にデプロイしたシグナリングサーバーのホスト名。
// 空文字 "" のときはローカル（同じWi-Fi）モード。ソースを書き換えずに切り替えられるよう、
// 起動するときの環境変数 VITE_CLOUD_HOST で上書きできる("local" でローカル、ほかの値はそのホスト名)。
//   例(PowerShell): $env:VITE_CLOUD_HOST = "local"; npm run tauri dev
const CLOUD_HOST_OVERRIDE = import.meta.env.VITE_CLOUD_HOST as string | undefined;
export const CLOUD_HOST =
  CLOUD_HOST_OVERRIDE === "local" ? "" : CLOUD_HOST_OVERRIDE || "yamanoha-signaling.fly.dev";

// 受信側ジッターバッファの目標(ミリ秒)。低遅延優先に設定。
export const JITTER_BUFFER_TARGET_MS = 120;

// サーバーURLを生成（WebSocket用）
export function buildDefaultServerUrl(): string {
  if (CLOUD_HOST) {
    return `wss://${CLOUD_HOST}`;
  }
  const hostname = window.location.hostname || "localhost";
  const protocol = window.location.protocol === "https:" ? "wss" : "ws";
  return `${protocol}://${hostname}:8080`;
}

export const DEFAULT_SERVER_URL = buildDefaultServerUrl();
