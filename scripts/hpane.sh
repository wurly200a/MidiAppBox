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
# VERIFY: 初回導入時に `herdr tab list --workspace 1` と `herdr tab get <id>` の
#         実際の JSON を目視確認すること。以下のパーサはキー名に依存しない
#         防御的な走査だが、構造が大きく違う場合はここを直す。
_py_find_pane_by_label() {
python3 - "$NAME" <<'PY'
import sys, json, re
label = sys.argv[1]
data = json.load(sys.stdin)

def walk(node):
    if isinstance(node, dict):
        yield node
        for v in node.values():
            yield from walk(v)
    elif isinstance(node, list):
        for v in node:
            yield from walk(v)

def pane_ids(node):
    """dict/list 内から pane ID 形式 (\d+-\d+) の文字列を全て集める"""
    out = []
    def rec(n):
        if isinstance(n, str) and re.fullmatch(r"\d+-\d+", n):
            out.append(n)
        elif isinstance(n, dict):
            for v in n.values(): rec(v)
        elif isinstance(n, list):
            for v in n: rec(v)
    rec(node)
    return out

for d in walk(data):
    vals = [v for v in d.values() if isinstance(v, str)]
    if label in vals:  # label キー名を仮定せず、値の一致で判定
        ids = pane_ids(d)
        if ids:
            print(ids[0]); sys.exit(0)
        # タブ dict 内にペイン情報がない場合は tab id だけ出して呼び元で tab get する
        for v in vals:
            if re.fullmatch(r"\d+:\d+", v):
                print("TAB:" + v); sys.exit(0)
sys.exit(1)
PY
}

_py_root_pane_from_create() {
python3 -c '
import sys, json, re
data = json.load(sys.stdin)
def rec(n):
    if isinstance(n, str) and re.fullmatch(r"\d+-\d+", n):
        print(n); sys.exit(0)
    elif isinstance(n, dict):
        for k in ("root_pane", "pane", "pane_id"):
            if k in n: rec(n[k])
        for v in n.values(): rec(v)
    elif isinstance(n, list):
        for v in n: rec(v)
rec(data)
sys.exit(1)'
}

resolve_pane() {
    local found
    if found=$(herdr tab list --workspace "$WORKSPACE" 2>/dev/null | _py_find_pane_by_label); then
        if [[ "$found" == TAB:* ]]; then
            herdr tab get "${found#TAB:}" | _py_find_pane_by_label && return 0
            return 1
        fi
        echo "$found"; return 0
    fi
    return 1
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
