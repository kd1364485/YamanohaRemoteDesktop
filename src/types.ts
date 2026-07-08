// 共通型定義・定数

export type ViewId = "login-view" | "register-view" | "home-view" | "host-view" | "connect-view" | "remote-view";

export type HostInfo = { hostId: string; deviceName: string };

export type DisplayInfo = { id: string; label: string; width: number; height: number; primary: boolean };

// クラウド（fly.io）にデプロイしたシグナリングサーバーのホスト名。
// 空文字 "" にするとローカル（同じWi-Fi）モードに戻ります。
export const CLOUD_HOST = "yamanoha-signaling.fly.dev";

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
