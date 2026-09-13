#!/usr/bin/env bash
# scripts/screen-still.sh — Linux ホスト(SDL ウィンドウ)の静止画 1 枚。
#
# Phase 18 で方式を変更した: x11grab は画面全体(ルートウィンドウ)を読むため、この環境
# (Wayland + XWayland / GNOME)では黒くなる。ウィンドウ ID を指定して読む ImageMagick の
# `import -window` なら SDL ウィンドウの中身が取れる(docs/results/phase18.md 0-0)。
#
# 使い方: scripts/screen-still.sh [出力先ディレクトリ] [ファイル名(拡張子なし)]
#         (省略時 captures/check-workflow/、screen_still_HHMMSS)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR_ARG="${1:-captures/check-workflow}"
if [[ "$OUTDIR_ARG" == /* ]]; then OUTDIR="$OUTDIR_ARG"; else OUTDIR="$REPO_ROOT/$OUTDIR_ARG"; fi
mkdir -p "$OUTDIR"
NAME="${2:-screen_still_$(date +%H%M%S)}"
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
  echo "screen-still: window '$WIN_NAME' of midibox_host not found (SDL host が起動しているか確認してください)" >&2
  exit 1
fi

OUT="$OUTDIR/$NAME.png"
import -window "$WIN" "$OUT"
echo "saved: $OUT"
