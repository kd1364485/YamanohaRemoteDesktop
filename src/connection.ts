// WebSocket接続・入力送信の管理

export let clientWs: WebSocket | null = null;
export let listWs: WebSocket | null = null;

// DataChannelの参照をwebrtc.tsから注入される（循環依存回避）
let inputChannelGetter: (() => RTCDataChannel | null) | null = null;

export function setInputChannelGetter(getter: () => RTCDataChannel | null): void {
  inputChannelGetter = getter;
}

export function getClientWs(): WebSocket | null {
  return clientWs;
}

export function setClientWs(ws: WebSocket | null): void {
  clientWs = ws;
}

export function closeClientWs(): void {
  if (clientWs) {
    clientWs.close();
    clientWs = null;
  }
}

export function closeListWs(): void {
  if (listWs) {
    listWs.close();
    listWs = null;
  }
}

export function setListWs(ws: WebSocket | null): void {
  listWs = ws;
}

// 入力をホストへ送信する。DataChannel(P2P直送)優先、未接続時はWebSocketリレーにフォールバック。
export function sendInput(obj: object): void {
  const payload = JSON.stringify(obj);
  const channel = inputChannelGetter ? inputChannelGetter() : null;
  if (channel && channel.readyState === "open") {
    try { channel.send(payload); return; } catch {}
  }
  if (clientWs && clientWs.readyState === WebSocket.OPEN) {
    clientWs.send(payload);
  }
}
