#!/usr/bin/env bash
# scripts/cam-rec.sh — Webカメラ(/dev/video0)録画。~/ビデオ/rec.sh の移植。
#
# 動画・音声ずれの調査・対処 (2026-07-20, check-workflow-routine 後の別タスク):
#   - v4l2/pulse とも thread_queue_size 既定(8)を超えて demuxer スレッドが
#     ブロックする警告が録画開始直後に出ていたため thread_queue_size を拡張。
#   - v4l2 は既定でカーネル(モノトニック)由来のタイムスタンプ、pulse は既定で
#     壁時計由来のタイムスタンプと、入力ごとに時刻系が異なっていたため v4l2 側を
#     -timestamps abs に統一。
#   - 上記修正後もメトロノームの点滅(映像)とクリック音(音声)を突き合わせて
#     実測したところ、音声が映像よりおよそ150〜210ms 遅れる一定オフセットが
#     修正前後で変わらず残った。低照度対策の固定露光(exposure_time_absolute=451
#     ≒45ms/フレーム)+ USB MJPEG の読み出し・デコード遅延に由来するカメラ
#     ハードウェア側の構造的遅延と推定し、-itsoffset -0.2 で音声を約200ms
#     前倒しする対症療法を追加、実測でオフセットを約210ms→約10msまで低減
#     できることを確認した。
#   - 200ms は本機(Logitech StreamCam)・この露光設定での経験値であり、
#     機種や exposure_time_absolute を変えた場合はズレ量も変わりうる点に注意
#     (詳細は docs/dev-log.md)。
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
       -f v4l2 -thread_queue_size 1024 -timestamps abs \
         -input_format mjpeg -video_size 1280x720 -framerate 30 -i "$DEV" \
       -itsoffset -0.2 -f pulse -thread_queue_size 1024 -i default \
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
