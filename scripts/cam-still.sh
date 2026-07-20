#!/usr/bin/env bash
# scripts/cam-still.sh — Webカメラ(/dev/video0)静止画1枚取得。
#
# 使い方: scripts/cam-still.sh [出力先ディレクトリ]  (省略時 captures/check-workflow/)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR_ARG="${1:-captures/check-workflow}"
if [[ "$OUTDIR_ARG" == /* ]]; then OUTDIR="$OUTDIR_ARG"; else OUTDIR="$REPO_ROOT/$OUTDIR_ARG"; fi
mkdir -p "$OUTDIR"

DEV=/dev/video0
OUT="$OUTDIR/cam_still_$(date +%H%M%S).png"

v4l2-ctl -d "$DEV" -c auto_exposure=1
v4l2-ctl -d "$DEV" -c exposure_time_absolute=451
v4l2-ctl -d "$DEV" -c gain=167
v4l2-ctl -d "$DEV" -c focus_automatic_continuous=0
v4l2-ctl -d "$DEV" -c focus_absolute=73
v4l2-ctl -d "$DEV" -c exposure_dynamic_framerate=0

ffmpeg -y -loglevel error -f v4l2 -input_format mjpeg -video_size 1280x720 -i "$DEV" -frames:v 1 "$OUT"
echo "saved: $OUT"
