// UI共通関数

import { ViewId, DEFAULT_SERVER_URL, CLOUD_HOST } from "./types";

export function showView(viewId: ViewId): void {
  const views: ViewId[] = ["login-view", "register-view", "home-view", "host-view", "connect-view", "remote-view"];
  for (const id of views) {
    const el = document.getElementById(id);
    if (el) {
      el.classList.toggle("hidden", id !== viewId);
    }
  }
}

export function setConnectionStatus(text: string, color: string): void {
  const status = document.getElementById("connection-status");
  if (!status) return;
  const dot = status.querySelector("span:first-child");
  const label = status.querySelector("span:last-child");
  if (dot) dot.className = `w-2 h-2 rounded-full bg-${color}-500`;
  if (label) label.textContent = text;
}

export function getServerUrl(): string {
  const input = document.getElementById("server-url") as HTMLInputElement | null;
  return input?.value?.trim() || DEFAULT_SERVER_URL;
}

export function getHttpBaseUrl(): string {
  if (CLOUD_HOST) {
    return `https://${CLOUD_HOST}`;
  }
  const hostname = window.location.hostname || "localhost";
  const protocol = window.location.protocol === "https:" ? "https" : "http";
  return `${protocol}://${hostname}:8080`;
}
