"""読み取った時計の値から、遅延(PC の時計 - Pixel 9 に映った時計)を集計する。

使い方: python calc.py results.csv
results.csv の形(1 行目は見出し。# で始まる行と、どちらかが空の行は飛ばす):
    frame,pc,phone
    01,12.345,12.198
    02,12.648,12.512
値は時計の表示そのまま(秒.ミリ秒、60 秒で一周)。60 秒をまたいだ行も補正する。
"""
import csv
import statistics
import sys


def diff_ms(pc: float, phone: float) -> float:
    d = (pc - phone) * 1000
    # 時計は 60 秒で一周する。例: pc=00.050, phone=59.900 → 150 ms
    if d < -30000:
        d += 60000
    elif d > 30000:
        d -= 60000
    return d


def percentile(values: list[float], p: float) -> float:
    s = sorted(values)
    k = (len(s) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def main() -> None:
    # Git Bash などのパイプ経由では、Windows の既定(cp932)で書き出されて文字化けするので、UTF-8 にそろえる
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    rows = []
    with open(sys.argv[1], encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(line for line in f if not line.lstrip().startswith("#")):
            pc, phone = (row.get("pc") or "").strip(), (row.get("phone") or "").strip()
            if not pc or not phone:
                continue
            rows.append((row.get("frame", "").strip(), diff_ms(float(pc), float(phone))))

    if not rows:
        sys.exit("読み取った行がありません")
    for frame, d in rows:
        print(f"{frame:>4}  {d:7.0f} ms")
    values = [d for _, d in rows]
    negative = [f for f, d in rows if d < 0]
    print()
    print(f"コマ数: {len(values)}")
    print(f"p50(中央値): {statistics.median(values):.0f} ms")
    print(f"p90: {percentile(values, 0.9):.0f} ms")
    print(f"最小 / 最大: {min(values):.0f} / {max(values):.0f} ms")
    print(f"平均: {statistics.mean(values):.0f} ms")
    if negative:
        print(f"注意: マイナスの行があります(読み間違いの可能性): {', '.join(negative)}")


if __name__ == "__main__":
    main()
