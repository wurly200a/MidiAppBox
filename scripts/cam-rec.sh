#!/usr/bin/env bash
# scripts/cam-rec.sh — Webカメラ(/dev/video0)録画。~/ビデオ/rec.sh の移植。
#
# 動画・音声ずれの調査・対処 (2026-07-20, check-workflow-routine 後の別タスク):
#   - v4l2/pulse とも thread_queue_size 既定(8)を超えて demuxer スレッドが
#     ブロックする警告が録画開始直後に出ていたため thread_queue_size を拡張。
#   - v4l2 は既定でカーネル(モノトニック)由来のタイムスタンプ、pulse は既定で
#     壁時計由来のタイムスタンプと、入力ごとに時刻系が異なっていたため v4l2 側を
#     -timestamps abs に統一。
#   - 上記修正後、メトロノームの点滅(映像)とクリック音(音声)という「0.5秒
#     周期の信号」を最近傍マッチングで突き合わせて -itsoffset による補正量を
#     見積もったが、これは誤りだった。周期信号同士の最近傍マッチングは真の
#     ズレが半周期(250ms)を超えると符号を取り違える(300ms遅れ ≡ -200ms ≡
#     +200ms mod 500ms)ため、実際に -itsoffset -0.2 を組み込んで ffplay で
#     再生確認したところ「むしろ逆にずれた」というユーザー報告により誤りが
#     判明した。この対症療法は撤回済み。
#   - 周期性のない単発イベント(拍手・画面ボタンタップ)での再検証も試みたが、
#     この録画アングル(ESP32 本体に固定)では手が画角に入らず、ボタン側にも
#     押下時の視覚変化がないため、映像側の基準時刻を特定できなかった。
#     このため固定オフセットの符号・量は**未確定のまま**とし、根拠のない
#     -itsoffset は追加しないことで確定(ユーザー承認済み、2026-07-20)。
#     再検証するなら、手や既知の視覚マーカーが確実に画角に入る構図が必要
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
       -f pulse -thread_queue_size 1024 -i default \
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
