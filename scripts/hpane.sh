#!/usr/bin/env bash
# hpane.sh — herdr 名前付きペイン ヘルパー
#
# 役割ごとにラベル付きタブ(1タブ=1ペイン)を管理し、
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
#       このスクリプトは ID を保存せず、毎回タブのラベルから解決し直す。

set -euo pipefail

WORKSPACE="${HPANE_WORKSPACE:-1}"
DEFAULT_TIMEOUT_MS="${HPANE_TIMEOUT_MS:-1800000}"   # 既定 30 分

usage() { grep '^#   ' "$0" | sed 's/^#   //'; exit 2; }

[ $# -ge 2 ] || usage
CMD="$1"; NAME="$2"; shift 2

# --- JSON からラベル一致タブの pane_id を抜く -------------------------------
# VERIFY 済み(実 JSON, hpane.sh 初回導入時に確認): `tab list`/`tab get` は
#   result.tabs[].{label, tab_id, ...} のみを返し pane_id を含まない。
#   pane_id は `pane list` の result.panes[].{pane_id, tab_id, ...} から
#   tab_id で突き合わせて取る必要がある。ID 形式は "<workspace>:p<N>" /
#   "<workspace>:t<N>"(例: "wB:p3", "wB:t3")で "\d+-\d+" ではない。
# 注意: `python3 - <<PY ... PY` は script 自体を stdin から読むため、
#       パイプで渡した JSON を読む stdin が残らない(実機で踏んだバグ)。
#       stdin をパイプ入力のために空けておくには `-c` で script を渡す。
_py_tab_id_for_label() {
python3 -c '
import sys, json
label = sys.argv[1]
data = json.load(sys.stdin)
tabs = data.get("result", {}).get("tabs", [])
matches = [t["tab_id"] for t in tabs if t.get("label") == label and "tab_id" in t]
if not matches:
    sys.exit(1)
print(matches[0])
' "$NAME"
}

_py_pane_id_for_tab() {
python3 -c '
import sys, json
tab_id = sys.argv[1]
data = json.load(sys.stdin)
panes = data.get("result", {}).get("panes", [])
for p in panes:
    if p.get("tab_id") == tab_id and "pane_id" in p:
        print(p["pane_id"]); sys.exit(0)
sys.exit(1)
' "$1"
}

_py_root_pane_from_create() {
python3 -c '
import sys, json
data = json.load(sys.stdin)
print(data["result"]["root_pane"]["pane_id"])
'
}

resolve_pane() {
    local tab_id pane_id
    tab_id=$(herdr tab list --workspace "$WORKSPACE" 2>/dev/null | _py_tab_id_for_label) || return 1
    pane_id=$(herdr pane list --workspace "$WORKSPACE" 2>/dev/null | _py_pane_id_for_tab "$tab_id") || return 1
    echo "$pane_id"
}

ensure_pane() {
    local pane
    if pane=$(resolve_pane); then
        echo "$pane"; return 0
    fi
    # なければラベル付きタブを新規作成(フォーカスは奪わない)
    pane=$(herdr tab create --workspace "$WORKSPACE" --label "$NAME" --no-focus | _py_root_pane_from_create)
    [ -n "$pane" ] || { echo "hpane: failed to create tab '$NAME'" >&2; exit 1; }
    echo "$pane"
}

case "$CMD" in
  ensure)
    ensure_pane
    ;;

  run)
    [ $# -ge 1 ] || usage
    USER_CMD="$1"; TIMEOUT_MS="${2:-$DEFAULT_TIMEOUT_MS}"
    PANE=$(ensure_pane)
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
    PANE=$(ensure_pane)
    herdr pane run "$PANE" "$1"
    ;;

  waitfor)
    [ $# -ge 1 ] || usage
    MATCH="$1"; TIMEOUT_MS="${2:-60000}"
    PANE=$(ensure_pane)
    herdr wait output "$PANE" --match "$MATCH" --timeout "$TIMEOUT_MS"
    ;;

  read)
    LINES="${1:-50}"
    PANE=$(ensure_pane)
    herdr pane read "$PANE" --source recent-unwrapped --lines "$LINES"
    ;;

  *)
    usage
    ;;
esac
