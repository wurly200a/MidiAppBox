#!/usr/bin/env bash
# scripts/cam-rec.sh — Webカメラ(/dev/video0)録画。~/ビデオ/rec.sh の移植。
#
# 動画・音声ずれの調査・対処 (2026-07-20, check-workflow-routine 後の別タスク):
#   - v4l2/pulse とも thread_queue_size 既定(8)を超えて demuxer スレッドが
#     ブロックする警告が録画開始直後に出ていたため thread_queue_size を拡張。
#   - v4l2 は既定でカーネル(モノトニック)由来のタイムスタンプ、pulse は既定で
#     壁時計(epoch)由来のタイムスタンプと、入力ごとに時刻系が異なっていた。
#     ffmpeg -debug_ts で両入力の生タイムスタンプを直接読むには両者が同一時計で
#     ある必要があるため、v4l2 側を -timestamps abs(epoch)に統一。
#   - 【根本原因】-debug_ts で確認したところ、両入力は同一 epoch 時計を共有して
#     いるにもかかわらず、ffmpeg は 2 つの別々の入力を**それぞれ自分の先頭
#     パケット時刻で 0 にリセット**する(demuxer+tsfixup)。このため「音声の
#     先頭が映像の先頭より遅れて始まる」という本来の相対差が破棄され、結果と
#     して音声が映像より**早く**再生される。実測(5 サンプル)でこの起動
#     オフセットは音声側が +134〜+191ms(平均 約164ms)と系統的だった。
#     -copyts / -start_at_zero では出力 mp4 が再度両ストリーム 0 起点に正規化
#     されてしまい保存できなかった。
#   - 【対処】pulse 入力に -itsoffset(音声を遅らせる方向)を前置して起動
#     オフセットを打ち消す。実測で出力に反映される(相殺されない)ことを確認。
#     既定 0.16s。CAM_AUDIO_DELAY 環境変数で微調整可能(機種・負荷でズレ量が
#     変わりうるため。ジッタは約±30ms=1フレーム相当)。
#   - 【過去の誤り】当初 -itsoffset -0.2(逆向き=音声をさらに早める)を入れて
#     「逆にずれた」。原因は効果測定にメトロノームの 0.5s 周期信号+最近傍
#     マッチングを使い、真のズレが半周期(250ms)超で符号を取り違えたこと。
#     周期信号を A/V 同期の基準に使ってはならない(詳細は docs/dev-log.md)。
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
# 音声を遅らせて起動オフセット(音声が約164ms早く始まる)を打ち消す(上記コメント参照)
AUDIO_DELAY="${CAM_AUDIO_DELAY:-0.16}"

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
       -itsoffset "$AUDIO_DELAY" -f pulse -thread_queue_size 1024 -i default \
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
