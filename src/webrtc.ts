// WebRTC接続管理・フリーズ検出

import { JITTER_BUFFER_TARGET_MS } from "./types";
import { getClientWs, setInputChannelGetter } from "./connection";

export let pc: RTCPeerConnection | null = null;
export let remoteStream: MediaStream | null = null;
export let inputChannel: RTCDataChannel | null = null;

// remoteDescriptionセット前に到着したICE candidateを一時保持するバッファ
let pendingIceCandidates: RTCIceCandidateInit[] = [];

export function getPc(): RTCPeerConnection | null {
  return pc;
}

export function getInputChannel(): RTCDataChannel | null {
  return inputChannel;
}

// 循環依存回避: connection.tsからinputChannelを参照できるようにgetterを登録
setInputChannelGetter(getInputChannel);

// WebRTC接続をセットアップ
export async function setupWebRTC(): Promise<void> {
  console.log("[client] setupWebRTC called");
  // 既存の接続があればクリーンアップ
  if (pc) {
    try { pc.close(); } catch {}
    pc = null;
  }
  remoteStream = null;
  inputChannel = null;
  // ICE candidateバッファをリセット
  pendingIceCandidates = [];

  // RTCPeerConnection作成
  const iceServers: RTCIceServer[] = [
    { urls: "stun:stun.l.google.com:19302" },
    { urls: "stun:stun1.l.google.com:19302" },
  ];

  // TURN server (URLパラメータで指定可能)
  const turnParam = new URLSearchParams(window.location.search).get("turn");
  if (turnParam) {
    const parts = turnParam.split(":");
    if (parts.length >= 5) {
      const proto = parts[0];
      const host = parts[1];
      const port = parts[2];
      const username = parts[3];
      const credential = parts.slice(4).join(":");
      iceServers.push({ urls: `${proto}:${host}:${port}`, username, credential });
      console.log(`[client] TURN server configured: ${proto}:${host}:${port}`);
    }
  }

  pc = new RTCPeerConnection({ iceServers });
  console.log("[client] RTCPeerConnection created");

  // ホストが作成した入力DataChannelを受信
  pc.ondatachannel = (event) => {
    if (event.channel.label !== "input" && event.channel.label !== "control") return;
    const channel = event.channel;
    console.log(`[client] input DataChannel received: ${channel.label}`);
    inputChannel = channel;
    channel.onopen = () => console.log(`[client] input DataChannel open: ${channel.label}`);
    channel.onclose = () => {
      console.log(`[client] input DataChannel closed: ${channel.label}`);
      if (inputChannel === channel) inputChannel = null;
    };
    channel.onerror = (error) => console.error("[client] input DataChannel error:", error);
  };

  // Trackイベント: リモートストリームを受信
  pc.ontrack = (event) => {
    console.log("Received remote track:", event.track.kind);
    remoteStream = event.streams[0];

    event.track.onmute = () => console.warn("[client] Video track muted");
    event.track.onunmute = () => console.log("[client] Video track unmuted");
    event.track.onended = () => console.warn("[client] Video track ended");

    // 受信側ジッターバッファを軽めに設定
    try {
      const receiver = event.receiver as RTCRtpReceiver & {
        jitterBufferTarget?: number | null;
        playoutDelayHint?: number | null;
      };
      if ("jitterBufferTarget" in receiver) {
        receiver.jitterBufferTarget = JITTER_BUFFER_TARGET_MS;
      }
      if ("playoutDelayHint" in receiver) {
        receiver.playoutDelayHint = JITTER_BUFFER_TARGET_MS / 1000;
      }
    } catch (err) {
      console.warn("jitterBufferTarget setting failed:", err);
    }

    const video = document.getElementById("remote-video") as HTMLVideoElement | null;
    if (video && remoteStream) {
      video.srcObject = remoteStream;
      video.play().catch((err) => console.error("Video play error:", err));

      video.onpause = () => {
        if (remoteStream && remoteStream.active) {
          video.play().catch(() => {});
        }
      };

      startFreezeDetection(video);
    }
  };

  // ICE candidateイベント
  pc.onicecandidate = (event) => {
    if (event.candidate && getClientWs() && getClientWs()!.readyState === WebSocket.OPEN) {
      getClientWs()!.send(JSON.stringify({
        type: "webrtc-ice",
        candidate: event.candidate.toJSON(),
      }));
    }
  };

  console.log("WebRTC setup complete");
}

// --- フリーズ検出と自動回復 ---
let freezeCheckTimer: ReturnType<typeof setTimeout> | null = null;
let lastFrameTime = 0;
let freezeRecoveryAttempts = 0;
const FREEZE_THRESHOLD_MS = 2000;
const FREEZE_MAX_RECOVERY_ATTEMPTS = 3;

function startFreezeDetection(video: HTMLVideoElement): void {
  stopFreezeDetection();
  lastFrameTime = Date.now();
  freezeRecoveryAttempts = 0;

  if ("requestVideoFrameCallback" in video) {
    const onFrame = () => {
      lastFrameTime = Date.now();
      if (freezeRecoveryAttempts > 0) {
        console.log("[client] Video recovered from freeze");
        freezeRecoveryAttempts = 0;
      }
      (video as any).requestVideoFrameCallback(onFrame);
    };
    (video as any).requestVideoFrameCallback(onFrame);
  }

  freezeCheckTimer = setInterval(() => {
    const elapsed = Date.now() - lastFrameTime;
    if (elapsed < FREEZE_THRESHOLD_MS) return;

    console.warn(`[client] Video freeze detected (${elapsed}ms without frames)`);
    if (freezeRecoveryAttempts >= FREEZE_MAX_RECOVERY_ATTEMPTS) {
      console.warn("[client] Max recovery attempts reached, giving up");
      return;
    }
    freezeRecoveryAttempts++;
    recoverFromFreeze(video);
  }, 1000);
}

export function stopFreezeDetection(): void {
  if (freezeCheckTimer) {
    clearInterval(freezeCheckTimer);
    freezeCheckTimer = null;
  }
}

function recoverFromFreeze(video: HTMLVideoElement): void {
  if (video.paused) {
    console.log("[client] Recovery: video was paused, calling play()");
    video.play().catch(() => {});
    return;
  }

  if (remoteStream) {
    console.log("[client] Recovery: resetting srcObject");
    video.srcObject = null;
    setTimeout(() => {
      if (remoteStream) {
        video.srcObject = remoteStream;
        video.play().catch(() => {});
      }
    }, 100);
  }
}

// WebRTC Offer受信処理
export async function handleWebRTCOffer(sdp: string): Promise<void> {
  if (pc) {
    try { pc.close(); } catch {}
    pc = null;
    remoteStream = null;
  }

  await setupWebRTC();
  if (!pc) return;

  try {
    await (pc as RTCPeerConnection).setRemoteDescription({ type: "offer", sdp });
    console.log("[client] remoteDescription set");

    if (pendingIceCandidates.length > 0) {
      console.log(`[client] Flushing ${pendingIceCandidates.length} buffered ICE candidates`);
      for (const c of pendingIceCandidates) {
        try {
          await (pc as RTCPeerConnection).addIceCandidate(c);
        } catch (err) {
          console.error("WebRTC addIceCandidate (buffered) error:", err);
        }
      }
      pendingIceCandidates = [];
    }

    const answer = await (pc as RTCPeerConnection).createAnswer();
    await (pc as RTCPeerConnection).setLocalDescription(answer);
    if (getClientWs() && getClientWs()!.readyState === WebSocket.OPEN) {
      getClientWs()!.send(JSON.stringify({
        type: "webrtc-answer",
        sdp: (pc as RTCPeerConnection).localDescription?.sdp,
      }));
    }
    console.log("WebRTC answer sent");
  } catch (err) {
    console.error("WebRTC offer handling error:", err);
  }
}

// WebRTC ICE candidate受信処理
export async function handleWebRTCIce(candidate: RTCIceCandidateInit): Promise<void> {
  if (!pc) return;
  if (!pc.remoteDescription) {
    pendingIceCandidates.push(candidate);
    console.log(`[client] ICE candidate buffered (total: ${pendingIceCandidates.length})`);
    return;
  }
  try {
    await pc.addIceCandidate(candidate);
  } catch (err) {
    console.error("WebRTC addIceCandidate error:", err);
  }
}
