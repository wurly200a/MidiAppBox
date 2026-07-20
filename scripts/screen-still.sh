#!/usr/bin/env bash
# scripts/screen-still.sh — Linux ホスト(SDL ウィンドウ)の静止画1枚取得。
# ウィンドウは xdotool search --name で特定し x11grab でその領域を1フレーム保存する。
#
# 使い方: scripts/screen-still.sh [出力先ディレクトリ]  (省略時 captures/check-workflow/)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR_ARG="${1:-captures/check-workflow}"
if [[ "$OUTDIR_ARG" == /* ]]; then OUTDIR="$OUTDIR_ARG"; else OUTDIR="$REPO_ROOT/$OUTDIR_ARG"; fi
mkdir -p "$OUTDIR"

WIN_NAME="${SCREEN_WINDOW_NAME:-MidiAppBox WASM host}"
WINS=$(xdotool search --name "$WIN_NAME")
if [ -z "$WINS" ]; then
  echo "screen-still: window '$WIN_NAME' not found (SDL host が起動しているか確認してください)" >&2
  exit 1
fi
# WM が同名のフレーム窓を重ねて返すことがあるため、面積最小(= 実 SDL 描画面)を選ぶ
WIN=""; BEST_AREA=-1
for w in $WINS; do
  eval "$(xdotool getwindowgeometry --shell "$w")"
  area=$((WIDTH * HEIGHT))
  if [ "$BEST_AREA" -lt 0 ] || [ "$area" -lt "$BEST_AREA" ]; then
    BEST_AREA=$area; WIN="$w"
  fi
done
eval "$(xdotool getwindowgeometry --shell "$WIN")"  # X, Y, WIDTH, HEIGHT を展開(最終選択分)

OUT="$OUTDIR/screen_still_$(date +%H%M%S).png"
ffmpeg -y -loglevel error -f x11grab -video_size "${WIDTH}x${HEIGHT}" -i "${DISPLAY:-:0}+${X},${Y}" -frames:v 1 "$OUT"
echo "saved: $OUT"
