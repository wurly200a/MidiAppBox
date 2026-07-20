#!/usr/bin/env bash
# scripts/cam-rec.sh — Webカメラ(/dev/video0)録画。~/ビデオ/rec.sh の移植。
# 既知課題「録画の動画と音声のずれ」は本チェックでは修正せず現状のまま移植する。
#
# 使い方: scripts/cam-rec.sh [出力先ディレクトリ]  (省略時 captures/check-workflow/)
# 停止: 標準入力に空行(Enter)を送る。
set -uo pipefail  # ffmpeg は SIGINT 後に非0で終了しうるため -e は使わない

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR_ARG="${1:-captures/check-workflow}"
if [[ "$OUTDIR_ARG" == /* ]]; then OUTDIR="$OUTDIR_ARG"; else OUTDIR="$REPO_ROOT/$OUTDIR_ARG"; fi
mkdir -p "$OUTDIR"

DEV=/dev/video0
OUT="$OUTDIR/cam_rec_$(date +%H%M%S).mp4"

apply_settings() {
  v4l2-ctl -d "$DEV" -c auto_exposure=1
  v4l2-ctl -d "$DEV" -c exposure_time_absolute=451
  v4l2-ctl -d "$DEV" -c gain=167
  v4l2-ctl -d "$DEV" -c focus_automatic_continuous=0
  v4l2-ctl -d "$DEV" -c focus_absolute=73
  v4l2-ctl -d "$DEV" -c exposure_dynamic_framerate=0
}

# 1. 録画をバックグラウンドで開始
ffmpeg -nostdin -loglevel warning \
       -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i "$DEV" \
       -f pulse -i default \
       -c:v libx264 -preset fast -crf 20 -pix_fmt yuv420p -c:a aac \
       -movflags +faststart "$OUT" &
FFPID=$!

# 2. デバイスが開いた後に設定を適用
sleep 2
apply_settings

# 3. 検証(focus_automatic_continuous=0 が返ることを確認)
echo "--- 適用結果 ---"
v4l2-ctl -d "$DEV" -C auto_exposure,exposure_time_absolute,gain,focus_automatic_continuous,focus_absolute

# 4. 録画中も3秒ごとに再適用する見張り
( while kill -0 "$FFPID" 2>/dev/null; do sleep 3; apply_settings; done ) &
WATCHPID=$!

echo ""
echo "●REC: $OUT"
echo "Enterキーで停止"
read -r

# 5. 停止(SIGINTで正常終了 → MP4が正しく閉じられる)
kill -INT "$FFPID"
wait "$FFPID"
kill "$WATCHPID" 2>/dev/null

echo "--- 確認 ---"
ffprobe -hide_banner "$OUT" 2>&1 | grep -E "Input|Duration|Video|Audio"
