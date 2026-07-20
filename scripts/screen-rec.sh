#!/usr/bin/env bash
# scripts/screen-rec.sh — Linux ホスト(SDL ウィンドウ)の画面録画。
# ウィンドウは xdotool search --name で特定し x11grab でその領域のみ録画する。
# 音声トラックは含めない(SDL 側の PulseAudio モニタ取得は本チェックのスコープ外)。
#
# 使い方: scripts/screen-rec.sh [出力先ディレクトリ]  (省略時 captures/check-workflow/)
# 停止: 標準入力に空行(Enter)を送る。
set -uo pipefail  # ffmpeg は SIGINT 後に非0で終了しうるため -e は使わない

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR_ARG="${1:-captures/check-workflow}"
if [[ "$OUTDIR_ARG" == /* ]]; then OUTDIR="$OUTDIR_ARG"; else OUTDIR="$REPO_ROOT/$OUTDIR_ARG"; fi
mkdir -p "$OUTDIR"

WIN_NAME="${SCREEN_WINDOW_NAME:-MidiAppBox WASM host}"
WINS=$(xdotool search --name "$WIN_NAME")
if [ -z "$WINS" ]; then
  echo "screen-rec: window '$WIN_NAME' not found (SDL host が起動しているか確認してください)" >&2
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

OUT="$OUTDIR/screen_rec_$(date +%H%M%S).mp4"

ffmpeg -nostdin -loglevel warning \
       -f x11grab -video_size "${WIDTH}x${HEIGHT}" -framerate 30 -i "${DISPLAY:-:0}+${X},${Y}" \
       -c:v libx264 -preset fast -crf 20 -pix_fmt yuv420p \
       -movflags +faststart "$OUT" &
FFPID=$!

echo ""
echo "●REC: $OUT (window=$WIN ${WIDTH}x${HEIGHT}+${X}+${Y})"
echo "Enterキーで停止"
read -r

kill -INT "$FFPID"
wait "$FFPID"

echo "--- 確認 ---"
ffprobe -hide_banner "$OUT" 2>&1 | grep -E "Input|Duration|Video"
