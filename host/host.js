import { WebSocket } from "ws";
import screenshot from "screenshot-desktop";
import { spawn } from "child_process";
import { mouse, keyboard, Button, Key } from "@nut-tree-fork/nut-js";
import { createSocket } from "dgram";
import {
  RTCPeerConnection,
  MediaStreamTrack,
  RTCRtpCodecParameters,
  RtpPacket,
} from "werift";

const SERVER_URL = process.env.SIGNALING_URL || "ws://localhost:8080";
const AUTH_TOKEN = process.env.SIGNALING_TOKEN || null;
const DEVICE_NAME = process.env.SIGNALING_DEVICE_NAME || "PC";
// プロセス起動時に一度だけ生成する固定ID。再接続時の重複登録を一意に識別するために使う。
const INSTANCE_ID = `inst-${Date.now()}-${Math.random().toString(36).slice(2, 10)}`;

// --- 画質・パフォーマンス設定 ---
const TARGET_FPS = Number(process.env.STREAM_FPS) || 30;       // 目標フレームレート（高更新頻度）
const STREAM_BITRATE = Number(process.env.STREAM_BITRATE) || 4; // 目標ビットレート(Mbps)
const STREAM_WIDTH = Number(process.env.STREAM_WIDTH) || 500;  // エンコード幅（画質向上）
const FFMPEG_PATH = process.env.FFMPEG_PATH || "ffmpeg";
// GOP（キーフレーム間隔・フレーム数）。0.5秒ごとにキーフレームを生成。
// 画面変化時のパケットロスに対応し、PLI即時キーフレームと合わせて自己回復を最大化。
// 帯域への影響はあるが、安定性優先。
const STREAM_GOP = Number(process.env.STREAM_GOP) || Math.max(1, Math.round(TARGET_FPS / 2));
// ddagrab(GPU DXGIキャプチャ)を使うか。デフォルトOFF。
// ハイブリッドGPU(Intel iGPU + NVIDIA dGPU)構成ではddagrabが全モニターを
// キャプチャできない（iGPU所有ディスプレイはdGPU経由で取得不可、output_idxも配列順と不一致）ため、
// 誤モニターキャプチャを避けデフォルト無効。単一GPUのデスクトップ等ではUSE_DDAGRAB=1で有効化可能。
// 有効時もddagrab失敗を検知すると自動でgdigrabにフォールバックする。
const USE_DDAGRAB = process.env.USE_DDAGRAB === "1";
// ddagrab用のGPUアダプタ指定（例: hw:0=1台目, hw:1=2台目）。nvencのあるGPUを指定する。
const DDAGRAB_DEVICE = process.env.DDAGRAB_DEVICE || "d3d11va=hw:0";

mouse.config.mouseSpeed = 0;
keyboard.config.autoDelayMs = 0;

let ws;
let isStreaming = false;
let displays = [];          // 利用可能なディスプレイ一覧
let currentDisplay = null;  // 現在キャプチャ中のディスプレイ
let trackedCursor = null;    // カーソル位置（論理座標）を自前で追跡。nut-jsのgetPosition()がセカンダリディスプレイで不正確なため。

// WebRTC関連
let pc = null;              // RTCPeerConnection
let videoTrack = null;      // MediaStreamTrack
let videoTransceiver = null; // RTCRtpTransceiver（RTPSenderのSSRC確認用）
let udpSocket = null;       // ffmpeg RTP受信用UDPソケット
let rtpPort = 0;            // RTP受信ポート
let inputChannel = null;    // 入力/カーソル用DataChannel（P2P直送）

// WebRTC接続をセットアップ
async function setupWebRTC() {
  // 既存の接続があればクリーンアップ
  if (pc) {
    try { await pc.close(); } catch {}
    pc = null;
  }
  if (videoTrack) {
    try { videoTrack.stop(); } catch {}
    videoTrack = null;
  }
  if (udpSocket) {
    try { udpSocket.close(); } catch {}
    udpSocket = null;
  }
  inputChannel = null;

  // RTCPeerConnection作成
  pc = new RTCPeerConnection({
    codecs: {
      audio: [],
      video: [
        new RTCRtpCodecParameters({
          mimeType: "video/H264",
          clockRate: 90000,
          payloadType: 96,
          rtcpFeedback: [
            { type: "nack" },
            { type: "ccm", parameter: "fir" },
            { type: "goog-remb" },
          ],
        }),
      ],
    },
  });

  // MediaStreamTrack作成
  videoTrack = new MediaStreamTrack({ kind: "video" });
  videoTransceiver = pc.addTransceiver(videoTrack, { direction: "sendonly" });
  console.log("Video track created, Sender SSRC:", videoTransceiver.sender.ssrc);

  // 入力/カーソル用DataChannel（P2P直送）を作成。
  // ホストがオファー側なのでここで作成し、SDPに含めてネゴシエーションする。
  // maxRetransmits:0 + ordered:false で低遅延の非信頼転送（入力は最新値が重要）。
  // 失敗しても映像接続は継続できるよう例外を握りつぶす（入力はWebSocketにフォールバック）。
  try {
    inputChannel = pc.createDataChannel("input", {
      ordered: false,
      maxRetransmits: 0,
    });
    inputChannel.onMessage.subscribe(async (data) => {
      try {
        const text = typeof data === "string" ? data : data.toString();
        const msg = JSON.parse(text);
        await handleInput(msg);
      } catch (err) {
        console.error("[DataChannel] input parse error:", err);
      }
    });
    inputChannel.stateChanged.subscribe((state) => {
      console.log("[DataChannel] input channel state:", state);
    });
  } catch (err) {
    console.error("[DataChannel] setup failed (falling back to WebSocket input):", err);
    inputChannel = null;
  }

  // ICE candidateイベント
  pc.onIceCandidate.subscribe((candidate) => {
    if (candidate && ws && ws.readyState === ws.OPEN) {
      ws.send(JSON.stringify({
        type: "webrtc-ice",
        candidate: candidate.toJSON(),
      }));
    }
  });

  // Offer作成
  const offer = await pc.createOffer();
  await pc.setLocalDescription(offer);

  // Offerをクライアントへ送信
  if (ws && ws.readyState === ws.OPEN) {
    ws.send(JSON.stringify({
      type: "webrtc-offer",
      sdp: pc.localDescription.sdp,
    }));
  }

  // ランダムポートでUDPソケットを作成
  const { randomPort } = await import("werift");
  rtpPort = await randomPort();
  udpSocket = createSocket("udp4");
  udpSocket.bind(rtpPort);

  // RTPパケット受信 → track.writeRtp（weriftのRTPSenderが内部でSSRC/PT/SeqNoを自動書き換えする）
  let rtpCount = 0;
  let firstPacketLogged = false;
  udpSocket.on("message", (data) => {
    rtpCount++;
    if (rtpCount % 300 === 0) {
      console.log(`Received ${rtpCount} RTP packets, size: ${data.length}`);
    }
    if (videoTrack) {
      try {
        const rtp = RtpPacket.deSerialize(data);

        // デバッグ: 最初のパケットの情報をログ出力
        if (!firstPacketLogged) {
          firstPacketLogged = true;
          console.log(`[RTP初回パケット] ffmpeg SSRC: ${rtp.header.ssrc}, PT: ${rtp.header.payloadType}, SeqNo: ${rtp.header.sequenceNumber}, Timestamp: ${rtp.header.timestamp}`);
          console.log(`[RTP Sender] SSRC: ${videoTransceiver?.sender?.ssrc}, コーデックPT: ${videoTransceiver?.sender?.codec?.payloadType}`);
        }

        // SSRCとPayloadTypeはwerift内部のRTPSender.sendRtp()で自動書き換えされるため手動設定不要
        videoTrack.writeRtp(rtp);
      } catch (err) {
        console.error("RTP parse error:", err);
      }
    }
  });

  // PLI（Picture Loss Indication）ハンドリング:
  // ブラウザがパケットロスを検出してキーフレームを要求したとき、
  // ffmpegのstdinに'f'を送って即座にキーフレームを強制生成する。
  // プロセス再起動は行わず、映像ストリームを維持したまま回復する。
  // クールダウンなしで即座にキーフレーム生成（画面変化時の即時回復優先）
  let pliCount = 0;
  videoTransceiver.sender.onPictureLossIndication.subscribe(() => {
    pliCount++;
    console.log(`[PLI] キーフレーム要求受信 (${pliCount}回目) - ffmpegに即座にキーフレーム命令`);
    if (ffmpegProc && ffmpegProc.stdin && ffmpegProc.stdin.writable) {
      try {
        ffmpegProc.stdin.write("f");
      } catch (err) {
        console.warn("[PLI] stdin write failed:", err.message);
      }
    }
  });

  // DTLS接続完了を待ってからffmpegを起動する。
  // weriftのRTPSenderはDTLS未接続中のRTPパケットを全て破棄するため、
  // 接続完了前にffmpegを起動するとキーフレームが失われ映像がフリーズする。
  videoTransceiver.sender.onReady.subscribe(() => {
    console.log("[WebRTC] DTLS connected, starting ffmpeg...");
    startStreaming();
  });

  console.log(`WebRTC setup complete, RTP listening on port ${rtpPort}, waiting for DTLS connection...`);
}

// WebRTC Answer受信処理
async function handleWebRTCAnswer(sdp) {
  if (!pc) return;
  try {
    await pc.setRemoteDescription({ type: "answer", sdp });
    console.log("WebRTC remote description set");
  } catch (err) {
    console.error("WebRTC setRemoteDescription error:", err);
  }
}

// WebRTC ICE candidate受信処理
async function handleWebRTCIce(candidate) {
  if (!pc) return;
  try {
    await pc.addIceCandidate(candidate);
  } catch (err) {
    console.error("WebRTC addIceCandidate error:", err);
  }
}

// ディスプレイ一覧を取得し、現在のディスプレイを決定する
async function loadDisplays() {
  try {
    const list = await screenshot.listDisplays();
    // screenshot-desktop は物理ピクセル、nut-js は論理ピクセル(DPIスケール適用後)を使う。
    // マウス制御・カーソル位置は nut-js 座標系に揃えるため、論理座標(l*)を保持する。
    // プライマリディスプレイのDPIスケールを取得（仮想画面オフセットの変換に使う）
    const primary = list.find((d) => d.left === 0 && d.top === 0) || list[0];
    const primaryScale = (primary && primary.dpiScale) || 1;

    displays = list.map((d, i) => {
      const scale = d.dpiScale || 1;
      return {
        id: d.id,
        label: `ディスプレイ${i + 1} (${d.width}×${d.height})`,
        outputIdx: i,     // ddagrab用のDXGI出力インデックス（列挙順）
        width: d.width,   // 物理ピクセル（ラベル表示用・ffmpegキャプチャサイズ）
        height: d.height,
        left: d.left,     // 物理ピクセル座標（gdigrabのoffsetに使用）
        top: d.top,
        dpiScale: scale,  // DPIスケール（setPosition補正用）
        // left/top は仮想画面上の物理ピクセル座標 → プライマリのスケールで論理化
        // width/height は各ディスプレイ自身のスケールで論理化
        lLeft: d.left / primaryScale,
        lTop: d.top / primaryScale,
        lWidth: d.width / scale,
        lHeight: d.height / scale,
        primary: d.left === 0 && d.top === 0,
      };
    });
    // 未選択 or 選択中の画面がなくなった場合はプライマリ（なければ先頭）を選択
    if (!currentDisplay || !displays.find((d) => d.id === currentDisplay.id)) {
      currentDisplay = displays.find((d) => d.primary) || displays[0] || null;
    }
  } catch (err) {
    console.error("Display list error:", err.message);
    displays = [];
    currentDisplay = null;
  }
}

// クライアントへディスプレイ一覧を送信
function sendDisplayList() {
  if (!ws || ws.readyState !== ws.OPEN) return;
  ws.send(JSON.stringify({
    type: "display-list",
    displays: displays.map(({ id, label, width, height, primary }) => ({ id, label, width, height, primary })),
    currentId: currentDisplay?.id || null,
  }));
}

// 正規化座標(0..1)を選択ディスプレイ上のグローバル座標に変換
function toGlobalPosition(nx, ny) {
  if (typeof nx !== "number" || typeof ny !== "number" || !currentDisplay) return null;
  // nut-js は論理座標を使うので論理バウンドで換算
  return {
    x: Math.round(currentDisplay.lLeft + nx * currentDisplay.lWidth),
    y: Math.round(currentDisplay.lTop + ny * currentDisplay.lHeight),
  };
}

// nut-js の setPosition は論理座標を受け取るが、セカンダリディスプレイで
// DPIスケールが異なる場合、指定座標が実際の位置と一致しない（DPI差分だけ縮小される）。
// これを補正するため、ディスプレイ原点からの相対位置を secScale/primaryScale 倍して渡す。
// trackedCursor には補正前の論理座標を保持し、クライアント描画に使う。
const _primaryScale = () => displays.find((d) => d.primary)?.dpiScale || 1;

async function setMousePosition(pos) {
  if (currentDisplay && !currentDisplay.primary) {
    const ratio = currentDisplay.dpiScale / _primaryScale();
    await mouse.setPosition({
      x: Math.round(currentDisplay.lLeft + (pos.x - currentDisplay.lLeft) * ratio),
      y: Math.round(currentDisplay.lTop + (pos.y - currentDisplay.lTop) * ratio),
    });
  } else {
    await mouse.setPosition(pos);
  }
}

function connect() {
  ws = new WebSocket(SERVER_URL);

  ws.on("open", () => {
    console.log("Connected to signaling server");
    ws.send(JSON.stringify({ type: "auth", token: AUTH_TOKEN }));
  });

  ws.on("message", async (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      return;
    }

    switch (msg.type) {
      case "auth-ok":
        console.log(`Authenticated as ${msg.username}`);
        ws.send(JSON.stringify({ type: "host", deviceName: DEVICE_NAME, instanceId: INSTANCE_ID }));
        break;

      case "host-ready":
        console.log(`Host ready! hostId: ${msg.hostId}, device: ${msg.deviceName}`);
        break;

      case "client-connected":
        console.log("Client connected, setting up WebRTC...");
        await loadDisplays();
        sendDisplayList();
        await setupWebRTC();
        // ffmpegの起動はsetupWebRTC内でDTLS接続完了後に自動実行される
        break;

      case "select-display": {
        const target = displays.find((d) => d.id === msg.displayId);
        if (target) {
          currentDisplay = target;
          console.log(`Display switched to: ${target.label}`);
          // WebRTC接続とffmpegを再起動して新しいディスプレイをキャプチャ
          const wasStreaming = isStreaming;
          stopStreaming();
          await setupWebRTC();
          // カーソルを選択ディスプレイの中央へ移動し、追跡位置も更新
          const cx = Math.round(target.lLeft + target.lWidth / 2);
          const cy = Math.round(target.lTop + target.lHeight / 2);
          trackedCursor = { x: cx, y: cy };
          try {
            await setMousePosition(trackedCursor);
          } catch {}
          if (wasStreaming) {
            setTimeout(() => startStreaming(), 300);
          }
        }
        break;
      }

      case "input":
        await handleInput(msg);
        break;

      case "disconnected":
        console.log("Client disconnected:", msg.message);
        stopStreaming();
        break;

      case "webrtc-answer":
        await handleWebRTCAnswer(msg.sdp);
        break;

      case "webrtc-ice":
        await handleWebRTCIce(msg.candidate);
        break;
    }
  });

  ws.on("close", () => {
    console.log("Disconnected from server, reconnecting in 3s...");
    stopStreaming();
    setTimeout(connect, 3000);
  });

  ws.on("error", (err) => {
    console.error("WebSocket error:", err.message);
  });
}

// --- ffmpeg H.264 (h264_nvenc) による画面キャプチャ → RTP ストリーミング ---
// gdigrab で画面をキャプチャし、h264_nvenc でハードウェアエンコード、
// RTPパケットとしてUDPでlocalhost:rtpPortに出力する。
// RTPパケットはUDPソケットで受信し、weriftのMediaStreamTrack.writeRtp()でWebRTCへ転送。

let ffmpegProc = null;
let cursorTimer = null;
// 実際に使用中のキャプチャ方式（ddagrab失敗時にfalseへ落としてgdigrabにフォールバック）
let useDdagrab = USE_DDAGRAB;
// ddagrabで起動したffmpegがキャプチャ/エンコード初期化エラーを出したか
let ddagrabErrorDetected = false;

// 共通のエンコード幅に合わせたスケールフィルタ式（縦横どちらが長くてもSTREAM_WIDTHに収める）
const SCALE_EXPR = `scale='if(gte(iw,ih),${STREAM_WIDTH},-2):if(gte(iw,ih),-2,${STREAM_WIDTH})'`;

// h264_nvenc共通エンコードオプション（入力フィルタ後に付与）
function nvencArgs(gop) {
  return [
    "-c:v", "h264_nvenc",
    "-preset", "p1",
    "-tune", "ull",
    "-profile:v", "baseline",
    "-level", "4.0",
    "-pix_fmt", "yuv420p",
    "-g", String(gop),
    "-keyint_min", String(gop),
    "-bf", "0",
    "-forced-idr", "1",
    "-b:v", `${STREAM_BITRATE}M`,
    "-maxrate", `${STREAM_BITRATE}M`,
    "-bufsize", `${STREAM_BITRATE * 2}M`,
    "-rc", "cbr",
    "-zerolatency", "1",
    "-an",
    "-f", "rtp",
    "-payload_type", "96",
    `rtp://127.0.0.1:${rtpPort}`,
  ];
}

// ddagrab(DXGI Desktop Duplication)でGPU上でキャプチャし、nvencでエンコードする引数。
// GDIのBitBltを使わずGPUで取得するため、CPU負荷とキャプチャ遅延が大幅に下がる。
function buildDdagrabArgs(display) {
  const gop = STREAM_GOP;
  return [
    // nvencのあるGPUアダプタでDXGIキャプチャ用のd3d11デバイスを初期化
    "-init_hw_device", DDAGRAB_DEVICE,
    "-filter_complex",
    // ddagrabでoutput_idxのモニターをGPUキャプチャ→hwdownloadでシステムメモリへ→スケール
    `ddagrab=output_idx=${display.outputIdx}:framerate=${TARGET_FPS}:draw_mouse=0,hwdownload,format=bgra,${SCALE_EXPR}`,
    ...nvencArgs(gop),
  ];
}

function buildGdigrabArgs(display) {
  const gop = STREAM_GOP;
  return [
    "-f", "gdigrab",
    "-framerate", String(TARGET_FPS),
    "-draw_mouse", "0",
    "-offset_x", String(display.left),
    "-offset_y", String(display.top),
    "-video_size", `${display.width}x${display.height}`,
    "-i", "desktop",
    "-vf", SCALE_EXPR,
    ...nvencArgs(gop),
  ];
}

// 現在の設定に応じてffmpeg引数を生成する。
function buildFfmpegArgs(display) {
  return useDdagrab ? buildDdagrabArgs(display) : buildGdigrabArgs(display);
}

function startStreaming() {
  if (isStreaming) return;
  if (!currentDisplay) return;
  isStreaming = true;

  const usingDdagrabNow = useDdagrab;
  ddagrabErrorDetected = false;
  const startedAt = Date.now();

  const args = buildFfmpegArgs(currentDisplay);
  console.log(`Starting ffmpeg (${usingDdagrabNow ? "ddagrab" : "gdigrab"}):`, FFMPEG_PATH, args.join(" "));
  ffmpegProc = spawn(FFMPEG_PATH, args, { stdio: ["pipe", "pipe", "pipe"] });

  ffmpegProc.stderr.on("data", (chunk) => {
    const text = chunk.toString().trim();
    if (text && !text.startsWith("frame=")) console.error("ffmpeg:", text);
    // ddagrab固有の初期化失敗を検知（DXGI未対応・出力なし等）
    if (usingDdagrabNow && /ddagrab|Desktop Duplication|DXGI|No such filter|Invalid argument|Error initializing/i.test(text)) {
      ddagrabErrorDetected = true;
    }
  });

  ffmpegProc.on("close", (code) => {
    console.log(`ffmpeg exited with code ${code}`);
    ffmpegProc = null;
    // ddagrabで起動直後(5秒以内)に異常終了し、エラーを検知した場合はgdigrabにフォールバック
    if (usingDdagrabNow && isStreaming && (ddagrabErrorDetected || (code !== 0 && Date.now() - startedAt < 5000))) {
      console.warn("[capture] ddagrab failed, falling back to gdigrab");
      useDdagrab = false;
      isStreaming = false;
      setTimeout(() => startStreaming(), 200);
      return;
    }
    // PLIによる意図的な再起動の場合はisStreamingをfalseにしない
    // （restartFfmpeg内で管理するため）
  });

  // カーソル位置を定期的に送信（映像とは別のJSONメッセージ）
  if (!cursorTimer) {
    cursorTimer = setInterval(() => {
      if (!isStreaming) { clearInterval(cursorTimer); cursorTimer = null; return; }
      sendCursorUpdate();
    }, 33);
  }
}

function sendCursorUpdate() {
  if (!currentDisplay || !trackedCursor) return;
  const cx = (trackedCursor.x - currentDisplay.lLeft) / currentDisplay.lWidth;
  const cy = (trackedCursor.y - currentDisplay.lTop) / currentDisplay.lHeight;
  if (cx < 0 || cx > 1 || cy < 0 || cy > 1) return;
  const payload = JSON.stringify({ type: "cursor", x: cx, y: cy });
  // DataChannel(P2P直送)優先、未接続時はWebSocketリレーにフォールバック
  if (inputChannel && inputChannel.readyState === "open") {
    try { inputChannel.send(payload); return; } catch {}
  }
  if (ws && ws.readyState === ws.OPEN) ws.send(payload);
}

function stopStreaming() {
  isStreaming = false;
  if (cursorTimer) { clearInterval(cursorTimer); cursorTimer = null; }
  if (ffmpegProc) {
    try { ffmpegProc.kill("SIGTERM"); } catch {}
    ffmpegProc = null;
  }
  if (pc) {
    try { pc.close(); } catch {}
    pc = null;
  }
  if (videoTrack) {
    try { videoTrack.stop(); } catch {}
    videoTrack = null;
  }
  videoTransceiver = null;
  if (udpSocket) {
    try { udpSocket.close(); } catch {}
    udpSocket = null;
  }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function handleInput(msg) {
  try {
    switch (msg.action) {
      case "mouse-move": {
        const p = toGlobalPosition(msg.nx, msg.ny);
        if (p) {
          trackedCursor = p;
          await setMousePosition(p);
        }
        break;
      }

      case "mouse-move-rel": {
        // 現在位置からの相対移動（スマホのトラックパッド操作用）
        // getPosition()がセカンダリディスプレイで不正確なため、自前追跡値を使う
        if (!trackedCursor) {
          try { trackedCursor = await mouse.getPosition(); } catch { break; }
        }
        let nx = trackedCursor.x + msg.dx;
        let ny = trackedCursor.y + msg.dy;
        // カーソルが現在のディスプレイ範囲外に出ないようクランプ
        if (currentDisplay) {
          nx = Math.max(currentDisplay.lLeft, Math.min(nx, currentDisplay.lLeft + currentDisplay.lWidth - 1));
          ny = Math.max(currentDisplay.lTop, Math.min(ny, currentDisplay.lTop + currentDisplay.lHeight - 1));
        }
        trackedCursor = { x: nx, y: ny };
        await setMousePosition(trackedCursor);
        break;
      }

      case "mouse-click": {
        // 正規化座標が指定されていれば移動してからクリック、なければ現在位置でクリック
        const p = toGlobalPosition(msg.nx, msg.ny);
        if (p) {
          trackedCursor = p;
          await setMousePosition(p);
        }
        if (msg.button === "right") {
          await mouse.click(Button.RIGHT);
        } else {
          await mouse.click(Button.LEFT);
        }
        break;
      }

      case "mouse-double-click": {
        const p = toGlobalPosition(msg.nx, msg.ny);
        if (p) {
          trackedCursor = p;
          await setMousePosition(p);
        }
        await mouse.doubleClick(msg.button === "right" ? Button.RIGHT : Button.LEFT);
        break;
      }

      case "mouse-down": {
        const p = toGlobalPosition(msg.nx, msg.ny);
        if (p) {
          trackedCursor = p;
          await setMousePosition(p);
        }
        if (msg.button === "left") await mouse.pressButton(Button.LEFT);
        else if (msg.button === "right") await mouse.pressButton(Button.RIGHT);
        break;
      }

      case "mouse-up":
        if (msg.button === "left") await mouse.releaseButton(Button.LEFT);
        else if (msg.button === "right") await mouse.releaseButton(Button.RIGHT);
        break;

      case "mouse-scroll": {
        // deltaY > 0 で下方向、< 0 で上方向。符号で向きを切り替える。
        const amount = Math.max(1, Math.round(Math.abs(msg.deltaY)));
        if (msg.deltaY > 0) {
          await mouse.scrollDown(amount);
        } else if (msg.deltaY < 0) {
          await mouse.scrollUp(amount);
        }
        break;
      }

      case "key": {
        const key = mapKey(msg.key);
        if (key !== null) {
          if (msg.isDown) {
            await keyboard.pressKey(key);
          } else {
            await keyboard.releaseKey(key);
          }
        }
        break;
      }

      case "type-char": {
        // 印字可能な文字（記号・大文字含む）をそのまま入力
        if (typeof msg.char === "string" && msg.char.length > 0) {
          await keyboard.type(msg.char);
        }
        break;
      }
    }
  } catch (err) {
    console.error("Input error:", err.message);
  }
}

function mapKey(key) {
  if (typeof key !== "string") return null;

  // 1文字の英字は大文字・小文字どちらも同じ物理キーへ（Ctrl+C など併用時用）
  if (key.length === 1) {
    const lower = key.toLowerCase();
    if (lower >= "a" && lower <= "z") return Key[lower.toUpperCase()];
    const digit = { "0": Key.Num0, "1": Key.Num1, "2": Key.Num2, "3": Key.Num3,
      "4": Key.Num4, "5": Key.Num5, "6": Key.Num6, "7": Key.Num7,
      "8": Key.Num8, "9": Key.Num9, " ": Key.Space };
    if (digit[key] !== undefined) return digit[key];
  }

  const keyMap = {
    "Enter": Key.Enter, "Escape": Key.Escape, "Backspace": Key.Backspace,
    "Tab": Key.Tab, "Space": Key.Space, "CapsLock": Key.CapsLock,
    "Shift": Key.LeftShift, "Control": Key.LeftControl,
    "Alt": Key.LeftAlt, "Meta": Key.LeftSuper, "OS": Key.LeftSuper,
    "ArrowUp": Key.Up, "ArrowDown": Key.Down,
    "ArrowLeft": Key.Left, "ArrowRight": Key.Right,
    "Delete": Key.Delete, "Home": Key.Home, "End": Key.End,
    "PageUp": Key.PageUp, "PageDown": Key.PageDown, "Insert": Key.Insert,
    "F1": Key.F1, "F2": Key.F2, "F3": Key.F3, "F4": Key.F4,
    "F5": Key.F5, "F6": Key.F6, "F7": Key.F7, "F8": Key.F8,
    "F9": Key.F9, "F10": Key.F10, "F11": Key.F11, "F12": Key.F12,
  };
  return keyMap[key] !== undefined ? keyMap[key] : null;
}

connect();
