#!/usr/bin/env bash
# device-regress.sh — 実機回帰の自動化(Phase 12 作業 3)
#
# 残したアプリを順に「run → 起動待ち → 一定時間保持 → stop → 停止待ち」で回し、
# free heap / largest free block / WARN・ERROR を集計して Markdown の表と合否を出す。
# ユーザーの物理操作は不要。音と画面の目視確認は自動化しない(docs/workflow.md §3.3 の
# カメラ+人間操作はそのまま残す)。
#
# 前提:
#   - ファームウェアが CONFIG_MIDIBOX_SERIAL_CMD=y でビルド・フラッシュ済み
#     (シリアルコマンド ping / ls / run / stop / heap。応答はタグ MBCMD のログ行)
#   - herdr が動いていて scripts/hpane.sh が使えること
#
# 待ち方について(docs/workflow.md §1-2 の趣旨):
#   完了待ちに herdr のペイン出力を使うと、スクロールバックに残る前回の実行の行に
#   誤マッチする(Phase 12 で実際に踏んだ)。本スクリプトは待ちを **tee が書く
#   ログファイルの「今回の待ちを始めた行より後ろ」** に対してのみ行うので、
#   古い行への誤マッチが原理的に起こらない。ペインは人間が見るライブ表示として残す。
#
# 使い方:
#   scripts/device-regress.sh [--task <name>] [--apps "a b c"] [--hold <sec>]
#                             [--conf <path>] [--keep-monitor]

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HPANE="$REPO/scripts/hpane.sh"
CONF="$REPO/scripts/device-regress.conf"
IMAGE="ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5:5.5.5"
PORT="${MIDIBOX_PORT:-/dev/ttyACM0}"

TASK="device-regress"
APPS_OVERRIDE=""
HOLD_OVERRIDE_ARG=""
KEEP_MONITOR=0

while [ $# -gt 0 ]; do
    case "$1" in
        --task)  TASK="$2"; shift 2 ;;
        --apps)  APPS_OVERRIDE="$2"; shift 2 ;;
        --hold)  HOLD_OVERRIDE_ARG="$2"; shift 2 ;;
        --conf)  CONF="$2"; shift 2 ;;
        --keep-monitor) KEEP_MONITOR=1; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# shellcheck source=device-regress.conf
. "$CONF" || { echo "cannot read conf: $CONF" >&2; exit 2; }
[ -n "$APPS_OVERRIDE" ] && APPS="$APPS_OVERRIDE"
[ -n "$HOLD_OVERRIDE_ARG" ] && HOLD_SEC="$HOLD_OVERRIDE_ARG"

OUT="$REPO/captures/$TASK"
LOG="$OUT/monitor.log"
REPORT="$OUT/report.md"
mkdir -p "$OUT"

DOCKER_RUN="docker run --rm -it -v $REPO:/workspaces/MidiAppBox -w /workspaces/MidiAppBox/src --device=$PORT --group-add $(stat -c '%g' "$PORT") $IMAGE"

say() { printf '%s\n' "$*" >&2; }

# --- ログ行数と、指定行より後ろだけを対象にした待ち -------------------------

line_count() { [ -f "$LOG" ] && wc -l < "$LOG" || echo 0; }

strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }

# wait_line <正規表現> <開始行(この行より後ろを見る)> <タイムアウト秒>
# 見つかったら 0 を返し、一致した最初の行を stdout に出す。
wait_line() {
    local pat="$1" from="$2" timeout="$3"
    local deadline=$((SECONDS + timeout)) hit
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$LOG" ]; then
            hit=$(tail -n "+$((from + 1))" "$LOG" 2>/dev/null | strip_ansi | grep -a -m1 -E "$pat")
            if [ -n "$hit" ]; then printf '%s\n' "$hit"; return 0; fi
        fi
        sleep 0.2
    done
    return 1
}

send_cmd() { "$HPANE" send esp32-monitor "$1" >/dev/null 2>&1; }

# --- モニタの起動 / 後始末 ---------------------------------------------------

kill_stale_containers() {
    local ids
    ids=$(docker ps -q --filter "ancestor=$IMAGE")
    if [ -n "$ids" ]; then
        say "note: killing leftover $IMAGE container(s) holding $PORT: $ids"
        # workflow.md §3.2 の既知の対処。hpane 経由で落とす。
        "$HPANE" run esp32-build "docker kill $ids" 60000 >/dev/null 2>&1
    fi
}

cleanup() {
    if [ "$KEEP_MONITOR" -eq 0 ]; then
        local ids
        ids=$(docker ps -q --filter "ancestor=$IMAGE")
        [ -n "$ids" ] && "$HPANE" run esp32-build "docker kill $ids" 60000 >/dev/null 2>&1
    fi
}
trap cleanup EXIT

kill_stale_containers
rm -f "$LOG"

say "starting monitor (log: $LOG)"
"$HPANE" send esp32-monitor \
    "$DOCKER_RUN bash -c 'source /opt/esp-idf/export.sh && PYTHONUNBUFFERED=1 idf.py -p $PORT monitor | tee /workspaces/MidiAppBox/captures/$TASK/monitor.log'" \
    >/dev/null 2>&1

# モニタ起動はボードをリセットするので、新しい起動の完了を待つ
if ! wait_line 'MBCMD: ready' 0 90 >/dev/null; then
    say "ERROR: serial command console did not come up within 90s."
    say "       CONFIG_MIDIBOX_SERIAL_CMD=y でビルド・フラッシュされているか確認すること。"
    say "       last lines of $LOG:"; tail -5 "$LOG" >&2
    exit 1
fi

# 疎通確認
mark=$(line_count); send_cmd "ping"
if ! wait_line 'MBCMD: pong' "$mark" 10 >/dev/null; then
    say "ERROR: no response to ping."; exit 1
fi
say "console ready"

# --- 各アプリを回す ----------------------------------------------------------

declare -a ROWS=()
overall=0

for app in $APPS; do
    hold="${HOLD_OVERRIDE[$app]:-$HOLD_SEC}"
    say "--- $app (hold ${hold}s)"

    mark=$(line_count)
    send_cmd "run $app"
    if ! runline=$(wait_line 'MBCMD: run (ok|err)' "$mark" 20); then
        ROWS+=("| $app | - | - | - | - | FAIL(run 応答なし) |"); overall=1; continue
    fi
    if [[ "$runline" == *"run err"* ]]; then
        ROWS+=("| $app | - | - | - | - | FAIL(${runline#*MBCMD: }) |"); overall=1; continue
    fi
    if ! wait_line 'app: app_init\(\)' "$mark" 20 >/dev/null; then
        ROWS+=("| $app | - | - | - | - | FAIL(app_init に到達せず) |"); overall=1; continue
    fi

    sleep "$hold"

    mark=$(line_count)
    send_cmd "stop"
    if ! stopline=$(wait_line 'app: stopped \(' "$mark" 30); then
        ROWS+=("| $app | - | - | - | - | FAIL(停止しない) |"); overall=1; continue
    fi

    # I (12345) WASM: app: stopped (ok), free heap 54008 (at start 54052), largest block 31744
    state=$(sed -n 's/.*app: stopped (\([^)]*\)).*/\1/p'            <<<"$stopline")
    fin=$(  sed -n 's/.*free heap \([0-9]*\).*/\1/p'                <<<"$stopline")
    start=$(sed -n 's/.*(at start \([0-9]*\)).*/\1/p'               <<<"$stopline")
    largest=$(sed -n 's/.*largest block \([0-9]*\).*/\1/p'          <<<"$stopline")
    delta=$((fin - start))
    want="${EXPECT_DELTA[$app]:-0}"

    verdict="PASS"
    [ "$state" != "ok" ]            && { verdict="FAIL(state=$state)"; overall=1; }
    [ "$delta" != "$want" ]         && { verdict="FAIL(delta=$delta 期待 $want)"; overall=1; }
    [ "$largest" != "$EXPECT_LARGEST" ] && { verdict="FAIL(largest=$largest 期待 $EXPECT_LARGEST)"; overall=1; }

    ROWS+=("| $app | $start | $fin | $(printf '%+d' "$delta") | $largest | $verdict |")
done

# --- WARN / ERROR の集計 -----------------------------------------------------

allow_re=$(IFS='|'; printf '%s' "${ALLOW_PATTERNS[*]}")
we_all=$(strip_ansi < "$LOG" | grep -aE '^[WE] \(' || true)
if [ -n "$allow_re" ]; then
    we_bad=$(printf '%s\n' "$we_all" | grep -av -E "$allow_re" | grep -a . || true)
else
    we_bad=$we_all
fi
we_bad_n=$(printf '%s' "$we_bad" | grep -ac . || true)
[ "$we_bad_n" -gt 0 ] && overall=1

# --- 出力 --------------------------------------------------------------------

{
    echo "# 実機自動回帰: $TASK"
    echo
    echo "- 生成: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "- ログ: \`captures/$TASK/monitor.log\`"
    echo "- 設定: \`$(realpath --relative-to="$REPO" "$CONF")\`"
    echo
    echo "| アプリ | 開始 free heap | 終了 free heap | 差分 | largest block | 判定 |"
    echo "|---|---|---|---|---|---|"
    printf '%s\n' "${ROWS[@]}"
    echo
    echo "許容外の WARN/ERROR: **${we_bad_n} 件**"
    if [ "$we_bad_n" -gt 0 ]; then
        echo
        echo '```'
        printf '%s\n' "$we_bad"
        echo '```'
    fi
    echo
    if [ "$overall" -eq 0 ]; then echo "**結果: PASS**"; else echo "**結果: FAIL**"; fi
} | tee "$REPORT"

exit "$overall"
