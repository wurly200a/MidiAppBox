#!/usr/bin/env bash
# hpane.sh — herdr 名前付きペイン ヘルパー
#
# 全ラベルを、このスクリプトを呼び出しているセッション自身のタブ(プロンプト
# ペインが属するタブ)の中に分割ペインとして配置し、ペイン単位のラベル
# (`herdr pane rename`)で解決する。
# 「なければ作る / あればそのまま使う」「番兵トークンで確実に完了待ち」を提供する。
#
# 使い方:
#   ./scripts/hpane.sh ensure  <name>                     # ペインを解決(なければ作成)し pane_id を表示
#   ./scripts/hpane.sh run     <name> "<cmd>" [timeout_ms] # コマンド実行→完了待ち→exit code をそのまま返す
#   ./scripts/hpane.sh send    <name> "<cmd>"              # 実行するだけ(待たない: モニタ/カメラ等の常駐用)
#   ./scripts/hpane.sh waitfor <name> "<match>" [timeout_ms] # 指定文字列が出るまで待つ(常駐プロセス用)
#   ./scripts/hpane.sh read    <name> [lines]              # ペインの最近の出力を表示
#
# 前提: herdr 内で実行されていること (HERDR_ENV=1)。
# 注意: herdr の pane ID は永続ではない(閉じると詰められる)。
#       このスクリプトは ID を保存せず、毎回ペインのラベルから解決し直す。
#
# レイアウト(check-workflow-routine でプロンプトペイン同居に変更。
# 最上段にプロンプト(全幅)、その下を 2 列 x 3 行):
#   プロンプト(セッション開始時から存在するペイン。全幅、既定で高さ 35%)
#   ------------------------------------------------
#   esp32-build   | unix-build
#   esp32-monitor | zenn
#   camera        | screen
#   (esp32-build はプロンプトペインから down split で作る「ルート」。
#    unix-build は esp32-build から right split。esp32-monitor/zenn/camera/
#    screen はそれぞれ真上のペインから down split。)
#   ANCHOR_OF/DIRECTION_OF で定義。ensure_pane は未作成のアンカーを再帰的に
#   先に ensure するため、どのラベルから呼んでも同じ配置に組み上がる
#   (呼び出し順序に依存しない)。

set -euo pipefail

WORKSPACE="${HPANE_WORKSPACE:-1}"
DEFAULT_TIMEOUT_MS="${HPANE_TIMEOUT_MS:-1800000}"   # 既定 30 分
PROMPT_ROW_RATIO="${HPANE_PROMPT_ROW_RATIO:-0.35}"  # ルート作成時、プロンプト側が確保する高さ比率

# ラベル → (アンカーラベル, 分割方向)。アンカーが空 = プロンプトペインから作るルート。
declare -A ANCHOR_OF=(
    [esp32-build]=""
    [unix-build]="esp32-build"
    [esp32-monitor]="esp32-build"
    [zenn]="unix-build"
    [camera]="esp32-monitor"
    [screen]="zenn"
)
declare -A DIRECTION_OF=(
    [unix-build]="right"
    [esp32-monitor]="down"
    [zenn]="down"
    [camera]="down"
    [screen]="down"
)

usage() { grep '^#   ' "$0" | sed 's/^#   //'; exit 2; }

[ $# -ge 2 ] || usage
CMD="$1"; NAME="$2"; shift 2

# --- JSON パーサ群 -----------------------------------------------------------
# VERIFY 済み(check-workflow, 使い捨てタブでの実地確認):
#   `herdr pane list` の各要素は tab_id に加え label(pane rename で設定した
#   ペイン単位のラベル。タブラベルとは別軸、未設定なら null)を持つ。
#   `herdr pane split` のレスポンスは result.pane.pane_id。
# 注意: `python3 - <<PY ... PY`(ヒアドキュメント)は stdin を script 本体で
#       使い切り、パイプ入力を読めない(7D で踏んだバグ)。パイプ入力を読む
#       ワンライナーは `python3 -c` で渡すこと。

_py_pane_id_for_label() {
python3 -c '
import sys, json
label = sys.argv[1]
data = json.load(sys.stdin)
panes = data.get("result", {}).get("panes", [])
matches = [p["pane_id"] for p in panes if p.get("label") == label and "pane_id" in p]
if not matches:
    sys.exit(1)
print(matches[0])
' "$NAME_ARG"
}

_py_current_pane_id() {
python3 -c '
import sys, json
data = json.load(sys.stdin)
print(data["result"]["pane"]["pane_id"])
'
}

_py_pane_id_from_split() {
python3 -c '
import sys, json
data = json.load(sys.stdin)
print(data["result"]["pane"]["pane_id"])
'
}

# --- ペイン解決/作成 ---------------------------------------------------------

resolve_pane_by_label() {
    local label="$1" NAME_ARG
    NAME_ARG="$label"
    herdr pane list --workspace "$WORKSPACE" 2>/dev/null | _py_pane_id_for_label
}

ensure_pane() {
    local label="$1" pane anchor_label direction anchor_pane split_out
    if pane=$(resolve_pane_by_label "$label"); then
        echo "$pane"; return 0
    fi

    anchor_label="${ANCHOR_OF[$label]:-}"
    if [ -z "$anchor_label" ]; then
        # ルートラベル: このスクリプトを呼んでいるセッションのプロンプトペインの
        # 下に down split してワークエリアを作る(同一タブ内に同居させる)。
        local prompt_pane
        prompt_pane=$(herdr pane current --current 2>/dev/null | _py_current_pane_id)
        [ -n "$prompt_pane" ] || { echo "hpane: failed to resolve current (prompt) pane" >&2; exit 1; }
        split_out=$(herdr pane split "$prompt_pane" --direction down --ratio "$PROMPT_ROW_RATIO" --no-focus)
        pane=$(echo "$split_out" | _py_pane_id_from_split)
    else
        anchor_pane=$(ensure_pane "$anchor_label")
        direction="${DIRECTION_OF[$label]}"
        split_out=$(herdr pane split "$anchor_pane" --direction "$direction" --no-focus)
        pane=$(echo "$split_out" | _py_pane_id_from_split)
    fi

    [ -n "$pane" ] || { echo "hpane: failed to create pane '$label'" >&2; exit 1; }
    herdr pane rename "$pane" "$label" >/dev/null
    echo "$pane"
}

case "$CMD" in
  ensure)
    ensure_pane "$NAME"
    ;;

  run)
    [ $# -ge 1 ] || usage
    USER_CMD="$1"; TIMEOUT_MS="${2:-$DEFAULT_TIMEOUT_MS}"
    PANE=$(ensure_pane "$NAME")
    TOKEN="HPANE_$(date +%s)_$$"        # 毎回一意 → 過去ログへの誤マッチを防ぐ
    # 送信文字列側は '' で分断し、コマンドエコー行に完全形が現れないようにする
    herdr pane run "$PANE" "$USER_CMD; echo ${TOKEN}''_EXIT=\$?"
    if ! herdr wait output "$PANE" --match "${TOKEN}_EXIT=" --timeout "$TIMEOUT_MS"; then
        echo "hpane: TIMEOUT (${TIMEOUT_MS}ms) waiting for '$NAME'. Last output:" >&2
        herdr pane read "$PANE" --source recent-unwrapped --lines 40 >&2
        exit 124
    fi
    OUT=$(herdr pane read "$PANE" --source recent-unwrapped --lines 200)
    CODE=$(printf '%s\n' "$OUT" | grep -o "${TOKEN}_EXIT=[0-9]*" | tail -1 | cut -d= -f2)
    if [ -z "${CODE:-}" ]; then
        echo "hpane: sentinel matched but exit code not found in scrollback" >&2
        exit 1
    fi
    if [ "$CODE" != "0" ]; then
        echo "hpane: command in '$NAME' failed (exit $CODE). Last output:" >&2
        printf '%s\n' "$OUT" | tail -60 >&2
    fi
    exit "$CODE"
    ;;

  send)
    [ $# -ge 1 ] || usage
    PANE=$(ensure_pane "$NAME")
    herdr pane run "$PANE" "$1"
    ;;

  waitfor)
    [ $# -ge 1 ] || usage
    MATCH="$1"; TIMEOUT_MS="${2:-60000}"
    PANE=$(ensure_pane "$NAME")
    herdr wait output "$PANE" --match "$MATCH" --timeout "$TIMEOUT_MS"
    ;;

  read)
    LINES="${1:-50}"
    PANE=$(ensure_pane "$NAME")
    herdr pane read "$PANE" --source recent-unwrapped --lines "$LINES"
    ;;

  *)
    usage
    ;;
esac
