#!/usr/bin/env bash
# scripts/screen-rec.sh — Linux ホスト(SDL ウィンドウ)の画面録画。
#
# Phase 18 で方式を変更した: x11grab は画面全体を読むためこの環境では黒くなる。
# ウィンドウ ID を指定した `xwd -id` を一定間隔で連続取得し、停止後に ffmpeg で mp4 にまとめる
# (docs/results/phase18.md 0-0)。フレーム間隔は sleep による概算なので、UI の動作確認用であり
# タイミング測定には使わない。音声トラックは含めない。
#
# 使い方: scripts/screen-rec.sh [出力先ディレクトリ]  (省略時 captures/check-workflow/)
#         フレームレートは環境変数 SCREEN_FPS(既定 10)
# 停止: 標準入力に空行(Enter)を送る。
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR_ARG="${1:-captures/check-workflow}"
if [[ "$OUTDIR_ARG" == /* ]]; then OUTDIR="$OUTDIR_ARG"; else OUTDIR="$REPO_ROOT/$OUTDIR_ARG"; fi
mkdir -p "$OUTDIR"
FPS="${SCREEN_FPS:-10}"
export DISPLAY="${DISPLAY:-:0}"

# midibox_host の pid と一致するウィンドウを選ぶ(同名のフレーム窓は mutter の pid)
WIN_NAME="${SCREEN_WINDOW_NAME:-MidiAppBox WASM host}"
PID=$(pgrep -x midibox_host | head -1 || true)
WIN=""
for w in $(xdotool search --name "$WIN_NAME" 2>/dev/null || true); do
  if [ -n "$PID" ] && [ "$(xdotool getwindowpid "$w" 2>/dev/null || true)" = "$PID" ]; then
    WIN="$w"
  fi
done
if [ -z "$WIN" ]; then
  echo "screen-rec: window '$WIN_NAME' of midibox_host not found (SDL host が起動しているか確認してください)" >&2
  exit 1
fi

STAMP=$(date +%H%M%S)
OUT="$OUTDIR/screen_rec_$STAMP.mp4"
FRAMES="$OUTDIR/.frames_$STAMP"
mkdir -p "$FRAMES"

(
  n=0
  period_ns=$((1000000000 / FPS))
  while [ ! -f "$FRAMES/.stop" ]; do
    s=$(date +%s%N)
    xwd -silent -id "$WIN" > "$FRAMES/f$(printf %06d "$n").xwd" 2>/dev/null || break  # ウィンドウが閉じたら終わる
    n=$((n + 1))
    e=$(date +%s%N)
    rem=$((period_ns - (e - s)))
    [ "$rem" -gt 0 ] && sleep "0.$(printf %09d "$rem")"
  done
) &
CAP=$!

echo ""
echo "●REC: $OUT (window=$WIN, ${FPS}fps)"
echo "Enterキーで停止"
read -r

touch "$FRAMES/.stop"
wait "$CAP"

COUNT=$(find "$FRAMES" -name 'f*.xwd' -size +0 | wc -l)
ffmpeg -nostdin -loglevel error -framerate "$FPS" -i "$FRAMES/f%06d.xwd" \
       -c:v libx264 -preset fast -crf 20 -pix_fmt yuv420p -movflags +faststart "$OUT"
rm -rf "$FRAMES"

echo "--- 確認(${COUNT} frames)---"
ffprobe -hide_banner "$OUT" 2>&1 | grep -E "Input|Duration|Video"
