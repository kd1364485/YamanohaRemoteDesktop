#!/bin/bash
# 撮影した動画から、読み取り用のコマを PNG で取り出す(Git Bash で実行。ffmpeg が必要)。
# 使い方: bash extract_frames.sh <動画> [開始秒=1] [枚数=30] [間隔秒=0.3]
#   例: bash extract_frames.sh ~/Videos/latency_01.mp4 2 30 0.3
# 出力: 動画と同じ場所の <動画名>_frames/ に 01.png, 02.png, ... と、確認用の一覧 sheet.png
# 間隔を 0.3 秒あけるのは、となりのコマどうしが同じ値になって、同じものを何度も数えないようにするため。
set -eu
VIDEO=${1:?動画のパスを指定してください}
START=${2:-1}
COUNT=${3:-30}
STEP=${4:-0.3}
OUT="${VIDEO%.*}_frames"
mkdir -p "$OUT"

# 撮影の細かさは、動画のコマの間隔で決まる。60 fps なら約 16.7 ms
FPS=$(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of csv=p=0 "$VIDEO")
echo "動画のコマの速さ: $FPS"
awk -v f="$FPS" 'BEGIN{split(f,a,"/"); r=a[1]/(a[2]?a[2]:1); printf "  → 1 コマ 約 %.1f ms(この細かさより細かい差は読めない)\n", 1000/r; if (r<50) print "  注意: 60 fps 未満です。撮影アプリの設定で 60 fps(またはスローモーション)にすると細かく読めます"}'

ffmpeg -v error -y -ss "$START" -i "$VIDEO" -vf "fps=1/$STEP" -frames:v "$COUNT" "$OUT/%02d.png"
ffmpeg -v error -y -i "$OUT/%02d.png" -vf "scale=480:-1,tile=6x5" -frames:v 1 "$OUT/sheet.png"
echo "取り出したコマ: $(ls "$OUT"/[0-9]*.png | wc -l) 枚 → $OUT"
echo "次: 各コマで PC の時計と Pixel 9 の時計を読み、results.csv に書いて python calc.py results.csv"
