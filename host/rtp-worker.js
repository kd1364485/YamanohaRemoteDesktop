const { parentPort, workerData } = require("worker_threads");
const { RtpPacket } = require("werift");

// RTPパケット解析をWorkerスレッドで並列処理
parentPort.on("message", (data) => {
  try {
    const rtp = RtpPacket.deSerialize(data);
    parentPort.postMessage({ type: "rtp", rtp });
  } catch (err) {
    parentPort.postMessage({ type: "error", error: err.message });
  }
});
