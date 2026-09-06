#!/usr/bin/env bash
# midi-clock-probe.sh — MIDI Clock 測定(Phase 13)。
#
#   実機 MIDI OUT → UM-ONE → PC(ALSA シーケンサ)で受けたクロックを記録し、
#   midi_loopback の E1 と同じ項目で集計する。
#
# 使い方:
#   ./scripts/midi-clock-probe.sh --task <タスク名> --label <ラベル> --duration <秒> [--bpm 120]
#       計測して captures/<タスク名>/<ラベル>.csv と .md を作る
#       --wait <秒> を付けると、接続先ポートが現れるまで待ってから計測を始める
#       (測定対象を後から起動する場合)
#   ./scripts/midi-clock-probe.sh --analyze-only --task <タスク名> --label <ラベル> \
#       [--bpm 120] [--from <秒>] [--to <秒>] [--segments auto]
#       既存 CSV を条件を変えて集計し直す(再測定は不要)
#   ./scripts/midi-clock-probe.sh --txlog captures/<タスク名>/monitor.log --label D
#       条件 D(送信側打刻)の σ を出す
#
# 出力先は captures/(.gitignore 対象)。生成物はツールのビルド成果物も含め
# コミットしない。
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO/tools/midi_clock_probe"
BUILD_DIR="$SRC_DIR/build"
BIN="$BUILD_DIR/midi_clock_probe"

TASK=""
LABEL="measurement"
DURATION=""
BPM="120"
PORT="UM-ONE"
FROM=""
TO=""
SEGMENTS=""
WAIT=""
SPAN=""
ANALYZE_ONLY=0
TXLOG=""

while [ $# -gt 0 ]; do
    case "$1" in
        --task)          TASK="$2"; shift 2 ;;
        --label)         LABEL="$2"; shift 2 ;;
        --duration)      DURATION="$2"; shift 2 ;;
        --bpm)           BPM="$2"; shift 2 ;;
        --port)          PORT="$2"; shift 2 ;;
        --from)          FROM="$2"; shift 2 ;;
        --to)            TO="$2"; shift 2 ;;
        --segments)      SEGMENTS="$2"; shift 2 ;;
        --wait)          WAIT="$2"; shift 2 ;;
        --span)          SPAN="$2"; shift 2 ;;
        --analyze-only)  ANALYZE_ONLY=1; shift ;;
        --txlog)         TXLOG="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [ -n "$TXLOG" ]; then
    exec python3 "$SRC_DIR/analyze.py" --txlog "$TXLOG" --label "$LABEL"
fi

[ -n "$TASK" ] || { echo "error: --task is required" >&2; exit 2; }
OUT_DIR="$REPO/captures/$TASK"
CSV="$OUT_DIR/$LABEL.csv"
MD="$OUT_DIR/$LABEL.md"
mkdir -p "$OUT_DIR"

if [ "$ANALYZE_ONLY" -eq 0 ]; then
    [ -n "$DURATION" ] || { echo "error: --duration is required" >&2; exit 2; }
    # ソースが新しいときだけビルドする(毎回のクリーンビルドはしない)
    if [ ! -x "$BIN" ] || [ "$SRC_DIR/midi_clock_probe.c" -nt "$BIN" ]; then
        mkdir -p "$BUILD_DIR"
        echo "building $BIN"
        cc -O2 -Wall -Wextra -o "$BIN" "$SRC_DIR/midi_clock_probe.c" \
            $(pkg-config --cflags --libs alsa)
    fi
    PROBE_ARGS=(--out "$CSV" --duration "$DURATION" --port "$PORT")
    [ -n "$WAIT" ] && PROBE_ARGS+=(--wait "$WAIT")
    "$BIN" "${PROBE_ARGS[@]}"
fi

ARGS=(--csv "$CSV" --bpm "$BPM" --label "$LABEL")
[ -n "$FROM" ] && ARGS+=(--from "$FROM")
[ -n "$TO" ] && ARGS+=(--to "$TO")
[ -n "$SEGMENTS" ] && ARGS+=(--segments "$SEGMENTS")
[ -n "$SPAN" ] && ARGS+=(--span "$SPAN")

python3 "$SRC_DIR/analyze.py" "${ARGS[@]}" | tee "$MD"
echo
echo "wrote: $CSV"
echo "wrote: $MD"
