"""操作の往復遅延を、スマホの画面録画 1 本から数える。

使い方:
    python tools/latency/tap_latency.py <録画.mp4>

前提(tools/latency/README.md の「撮影用の端末なしで測る」):
- PC で tap.html を全画面にしておく(クリックで画面の白黒が反転する)。
- クライアントを ?tapmark 付き(または起動時に VITE_TAPMARK=1)で開く。
  クリックを送った瞬間に、スマホの画面の左上へ赤い四角が出る。
- スマホを横向きにして、画面録画しながら、1 秒以上あけて 1 回ずつタップする。

各タップについて「赤い四角が出たコマ」から「中央の明るさが反転したコマ」までの時間を、
録画の時刻情報(可変フレームレートでも正しい)から求め、p50 / p90 / 最小 / 最大を出す。
ffmpeg / ffprobe と numpy が必要。
"""
import json
import subprocess
import sys

import numpy as np

W, H = 404, 180  # 解析用に縮小する大きさ(横向きの録画を想定)


def main() -> None:
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    video = sys.argv[1]
    pts = [float(x) for x in subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "frame=best_effort_timestamp_time", "-of", "csv=p=0", video],
        capture_output=True, text=True, check=True).stdout.split() if x.strip()]
    proc = subprocess.Popen(
        ["ffmpeg", "-v", "quiet", "-i", video, "-fps_mode", "passthrough",
         "-vf", f"scale={W}:{H}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE)
    marks, light = [], []
    while True:
        buf = proc.stdout.read(W * H * 3)
        if len(buf) < W * H * 3:
            break
        f = np.frombuffer(buf, np.uint8).reshape(H, W, 3).astype(int)
        tl = f[: int(H * 0.4), : int(W * 0.25)]  # 左上(赤い四角が出るあたり)
        red = (tl[..., 0] > 170) & (tl[..., 1] < 90) & (tl[..., 2] < 90)
        marks.append(red.sum())
        light.append(f[int(H * 0.35): int(H * 0.6), int(W * 0.3): int(W * 0.7)].mean())
    n = min(len(pts), len(marks))
    t = np.array(pts[:n])
    mark = np.array(marks[:n]) > 200
    light = np.array(light[:n])
    white = light > (light.min() + light.max()) / 2
    onsets = [i for i in range(1, n) if mark[i] and not mark[i - 1]]
    flips = [i for i in range(1, n) if white[i] != white[i - 1]]
    results = []
    for k, m in enumerate(onsets):
        limit = onsets[k + 1] if k + 1 < len(onsets) else n
        nxt = [f for f in flips if m <= f < limit]
        if nxt:
            results.append((t[m], (t[nxt[0]] - t[m]) * 1000))
    if not results:
        print("赤い四角と白黒の反転の組が見つからなかった")
        sys.exit(1)
    d = np.array([r[1] for r in results])
    for ts, ms in results:
        print(f"{ts:8.3f} s  {ms:6.1f} ms")
    iv = np.median(np.diff(t)) * 1000
    print(json.dumps({
        "taps": len(onsets), "paired": len(d),
        "p50_ms": round(float(np.median(d)), 1), "p90_ms": round(float(np.percentile(d, 90)), 1),
        "min_ms": round(float(d.min()), 1), "max_ms": round(float(d.max()), 1),
        "frame_interval_ms": round(float(iv), 1),
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
