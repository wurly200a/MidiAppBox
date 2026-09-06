#!/usr/bin/env python3
"""midi_clock_probe の CSV を集計して Markdown を出す(Phase 13 の測定ツール)。

集計項目は midi_loopback の E1 統計と揃えてある:
  0xF8 総数 / 期待数 / clocks÷expected / 間隔の min・mean・max・σ /
  ヒストグラム / 外れ値(公称の 1.5 倍以上・0.5 倍以下)/ 見かけ BPM 分布 /
  0xFA・0xFB・0xFC の件数。

使い方:
  analyze.py --csv <csv> [--bpm 120] [--from <sec>] [--to <sec>]
             [--segments auto] [--label <名前>] [--txlog <monitor.log>]

時刻はすべて「プローブ起動を 0 とする秒」。--from/--to はこの時刻で窓を切る
(条件 B の「テンポ変更区間を除外する」用)。
"""
import argparse
import csv
import math
import sys

NOMINAL = lambda bpm: 60_000_000.0 / (bpm * 24.0)  # µs / clock


def read_csv(path):
    rows = []
    src = ""
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                src = line[1:].strip()
                continue
            break
        f.seek(0)
        rdr = csv.DictReader(l for l in f if not l.startswith("#"))
        for r in rdr:
            try:  # 計測中の CSV は最終行が途中で切れていることがある
                rows.append((int(r["seq"]), r["kind"],
                             int(r["t_queue_us"]), int(r["t_user_us"])))
            except (TypeError, ValueError):
                continue
    return src, rows


def stats(xs):
    n = len(xs)
    if n == 0:
        return dict(n=0, min=0, mean=0.0, max=0, sd=0.0)
    m = sum(xs) / n
    var = sum((x - m) ** 2 for x in xs) / n if n > 1 else 0.0
    return dict(n=n, min=min(xs), mean=m, max=max(xs), sd=math.sqrt(var))


def histogram(intervals, nominal, width=50, span=500):
    """公称 ±span µs を width µs 刻みで。範囲外は下側/上側にまとめる。"""
    lo = nominal - span
    bins = {}
    under = over = 0
    nb = int(2 * span / width)
    for v in intervals:
        if v < lo:
            under += 1
        elif v >= lo + nb * width:
            over += 1
        else:
            k = int((v - lo) // width)
            bins[k] = bins.get(k, 0) + 1
    return lo, width, nb, bins, under, over


def bar(count, total, cols=40):
    if total == 0:
        return ""
    return "#" * max(1, int(round(cols * count / total))) if count else ""


def apparent_bpm(intervals, window=24):
    """直近 window クロックの移動平均から見かけ BPM を出す。"""
    out = []
    if len(intervals) < window:
        return out
    acc = sum(intervals[:window])
    out.append(60_000_000.0 / (acc / window * 24.0))
    for i in range(window, len(intervals)):
        acc += intervals[i] - intervals[i - window]
        out.append(60_000_000.0 / (acc / window * 24.0))
    return out


def bpm_distribution(bpms, binw=0.25):
    if not bpms:
        return [], "n/a"
    lo = math.floor(min(bpms) / binw) * binw
    hi = math.ceil(max(bpms) / binw) * binw
    nb = max(1, int(round((hi - lo) / binw)))
    counts = [0] * (nb + 1)
    for b in bpms:
        counts[min(nb, int((b - lo) / binw))] += 1
    rows = [(lo + i * binw, c) for i, c in enumerate(counts) if c]
    # 単峰 / 多峰の判定: 3 点平滑した上で、最大ピークの 10% 以上の高さを持ち
    # 1bpm 以上離れた局所最大が 2 つ以上あれば多峰とする
    sm = []
    for i in range(len(counts)):
        w = counts[max(0, i - 1):i + 2]
        sm.append(sum(w) / len(w))
    peaks = []
    for i in range(len(sm)):
        l = sm[i - 1] if i > 0 else 0
        r = sm[i + 1] if i + 1 < len(sm) else 0
        if sm[i] > 0 and sm[i] >= l and sm[i] >= r:
            peaks.append((i, sm[i]))
    if not peaks:
        return rows, "n/a"
    top = max(p[1] for p in peaks)
    sig = [p for p in peaks if p[1] >= 0.10 * top]
    merged = []
    for i, h in sorted(sig):
        if merged and (i - merged[-1][0]) * binw < 1.0:
            if h > merged[-1][1]:
                merged[-1] = (i, h)
            continue
        merged.append((i, h))
    verdict = "単峰" if len(merged) <= 1 else f"{len(merged)}峰"
    if len(merged) > 1:
        verdict += " (" + ", ".join(f"{lo + i * binw:.2f}" for i, _ in merged) + ")"
    return rows, verdict


def segment_split(clocks, intervals, nominal, window=24, tol=0.025, hold_s=5.0):
    """テンポ区間へ分割する(条件 C)。

    受信側の 1 発ごとの間隔は USB のフレーム化で ±1ms 級にぼやけるので、
    判定は 24 クロックの移動平均(σ が 1/√24 に落ちる)に対して行う。
    移動平均が tol 以上ずれた状態が hold_s 以上続いたら切り替えとみなし、
    区間の境目は「その移動平均窓が新テンポで埋まり始めた位置」に置く。
    """
    n = len(intervals)
    if n < window * 3:
        return [(0, n)]
    tm = [0.0] * n  # trailing mean
    acc = 0.0
    for i, v in enumerate(intervals):
        acc += v
        if i >= window:
            acc -= intervals[i - window]
        tm[i] = acc / min(i + 1, window)

    segs = []
    start = 0
    ref = tm[window - 1]
    i = window
    while i < n:
        if ref > 0 and abs(tm[i] - ref) / ref > tol:
            j = i
            while j < n and abs(tm[j] - ref) / ref > tol:
                j += 1
            if sum(intervals[i:j]) >= hold_s * 1e6 or j >= n:
                # 実際の切替点を探す: 新テンポの代表値を取り、i から遡って
                # 「旧テンポ側に近い」最後の間隔の次を境目にする
                lo_n = min(n, i + window)
                hi_n = min(n, i + 2 * window)
                new_mean = (sum(intervals[lo_n:hi_n]) / (hi_n - lo_n)) if hi_n > lo_n else tm[i]
                k = i
                while k > start and abs(intervals[k - 1] - ref) > abs(intervals[k - 1] - new_mean):
                    k -= 1
                end = max(start + 1, k)
                segs.append((start, end))
                start = end
                probe = min(n - 1, start + 2 * window)
                ref = tm[probe]
                i = min(n, start + 2 * window)
                continue
            i = j
        else:
            i += 1
    segs.append((start, n))
    return [(a, b) for a, b in segs if b - a >= window]


def playing_spans(rows, t_from, t_to):
    """0xFA/0xFB で開き 0xFC で閉じる「再生中」区間の一覧を返す。

    STOPPED をまたぐ間隔はクロックの欠落ではないので、統計から外すために使う。
    start/stop が観測されていない場合は全体を 1 区間とみなす。
    """
    lo = -float("inf") if t_from is None else t_from * 1e6
    hi = float("inf") if t_to is None else t_to * 1e6
    spans = []
    open_t = None
    last_clock = None
    for (_, k, t, _) in rows:
        if k in ("start", "continue"):
            if open_t is None:
                open_t = t
        elif k == "stop":
            if open_t is not None:
                spans.append((open_t, t))
                open_t = None
        elif k == "clock":
            last_clock = t
            if open_t is None and not spans:
                open_t = t  # 開始を取り逃がした場合は最初のクロックから
    if open_t is not None and last_clock is not None and last_clock > open_t:
        spans.append((open_t, last_clock))
    clipped = []
    for a, b in spans:
        a2, b2 = max(a, lo), min(b, hi)
        if b2 > a2:
            clipped.append((a2, b2))
    return clipped


def analyze(rows, bpm, t_from, t_to, label, segments, span_no=None):
    nominal = NOMINAL(bpm)
    out = []
    ev = {}
    for k in ("clock", "start", "continue", "stop", "other", "sensing", "tick"):
        ev[k] = [t for (_, kk, t, _) in rows if kk == k]

    def in_window(t):
        s = t / 1e6
        return (t_from is None or s >= t_from) and (t_to is None or s <= t_to)

    spans = playing_spans(rows, t_from, t_to)
    if span_no is not None:
        if span_no < 1 or span_no > len(spans):
            return f"### {label}\n\n(再生区間 {span_no} は存在しない: 全 {len(spans)} 区間)"
        spans = [spans[span_no - 1]]

    def span_of(t):
        for i, (a, b) in enumerate(spans):
            if a <= t <= b:
                return i
        return None

    clocks = [t for t in ev["clock"] if in_window(t) and span_of(t) is not None]
    # 間隔は「同じ再生区間内で連続するクロック」だけで作る
    # (STOPPED をまたぐ間隔は欠落ではないので除外する)
    intervals = []
    ivt = []  # 各間隔の終端時刻
    for i in range(1, len(clocks)):
        if span_of(clocks[i]) == span_of(clocks[i - 1]):
            intervals.append(clocks[i] - clocks[i - 1])
            ivt.append(clocks[i])

    span_us = sum(b - a for a, b in spans)
    # 各区間は先頭でも 1 発出るので区間数ぶん +1
    expected_n = span_us / nominal + len(spans)

    st = stats(intervals)
    outliers = [(i, v) for i, v in enumerate(intervals)
                if v >= 1.5 * nominal or v <= 0.5 * nominal]
    bpms = apparent_bpm(intervals)
    bpm_rows, verdict = bpm_distribution(bpms)

    lat = [u - t for (_, k, t, u) in rows if k == "clock" and in_window(t)]
    lat_st = stats(lat)

    out.append(f"### {label}\n")
    out.append("| 項目 | 値 |")
    out.append("|---|---|")
    out.append(f"| 公称 BPM / クロック間隔 | {bpm:g} / {nominal:.2f} µs |")
    out.append(f"| 再生区間(0xFA/0xFB〜0xFC) | {len(spans)} 区間 / 計 {span_us/1e6:.1f} s |")
    if t_from is not None or t_to is not None:
        out.append(f"| 窓 | {t_from if t_from is not None else 0:.1f} 〜 "
                   f"{t_to if t_to is not None else (clocks[-1]/1e6 if clocks else 0):.1f} s |")
    out.append(f"| 0xF8 総数 | **{len(clocks)}** |")
    out.append(f"| 期待数 | {expected_n:.1f} |")
    ratio = 100.0 * len(clocks) / expected_n if expected_n > 0 else 0.0
    out.append(f"| clocks / expected | **{ratio:.2f} %** |")
    out.append(f"| 0xFA / 0xFB / 0xFC | {len(ev['start'])} / {len(ev['continue'])} / {len(ev['stop'])} |")
    out.append(f"| 間隔 min / mean / max | {st['min']} / **{st['mean']:.1f}** / {st['max']} µs |")
    out.append(f"| 間隔 σ(受信側・参考) | {st['sd']:.1f} µs |")
    out.append(f"| 外れ値(≥1.5x / ≤0.5x) | **{len(outliers)} 件** |")
    out.append(f"| 見かけ BPM(24 クロック移動平均) | {verdict}"
               + (f", min {min(bpms):.2f} / max {max(bpms):.2f}" if bpms else "") + " |")
    if bpms:
        low = sum(1 for b in bpms if b < bpm - 1.0)
        out.append(f"| 見かけ BPM < {bpm - 1.0:.0f} のサンプル | {low} "
                   f"({100.0 * low / len(bpms):.2f} %) |")
    out.append(f"| その他の受信イベント | other={len(ev['other'])} sensing={len(ev['sensing'])} |")
    out.append(f"| カーネル打刻とユーザ打刻の差 | mean {lat_st['mean']:.0f} µs / max {lat_st['max']} µs |")
    out.append("")

    if outliers:
        out.append("**外れ値の一覧**\n")
        out.append("| # | 発生時刻 (s) | 間隔 (µs) | 公称比 |")
        out.append("|---|---|---|---|")
        for i, v in outliers[:40]:
            out.append(f"| {i} | {ivt[i]/1e6:.3f} | {v} | {v/nominal:.2f}x |")
        if len(outliers) > 40:
            out.append(f"| … | 他 {len(outliers)-40} 件 | | |")
        out.append("")

    lo, width, nb, bins, under, over = histogram(intervals, nominal)
    out.append("**クロック間隔のヒストグラム**(50µs 刻み)\n")
    out.append("```")
    total = len(intervals)
    if under:
        out.append(f"{'':8s}  < {lo:8.0f} : {under:7d} {bar(under, total)}")
    for k in sorted(bins):
        a = lo + k * width
        out.append(f"{a:8.0f}..{a+width:8.0f} : {bins[k]:7d} {bar(bins[k], total)}")
    if over:
        out.append(f"{'':8s} >= {lo+nb*width:8.0f} : {over:7d} {bar(over, total)}")
    out.append("```")
    out.append("")

    if bpm_rows:
        out.append("**見かけ BPM の分布**(0.25 bpm 刻み)\n")
        out.append("```")
        tb = sum(c for _, c in bpm_rows)
        for b, c in bpm_rows:
            out.append(f"{b:8.2f} : {c:7d} {bar(c, tb)}")
        out.append("```")
        out.append("")

    if segments == "auto":
        segs = segment_split(ivt, intervals, nominal)
        if len(segs) > 1:
            out.append("**区間分割(移動平均が 2.5% 以上変化して 5 秒以上持続した点で分割)**\n")
            out.append("| 区間 | 時刻 (s) | クロック数 | 平均間隔 (µs) | 見かけ BPM | 外れ値 |")
            out.append("|---|---|---|---|---|---|")
            for si, (a, b) in enumerate(segs):
                sub = intervals[a:b]
                ss = stats(sub)
                sb = 60_000_000.0 / (ss['mean'] * 24.0) if ss['mean'] else 0
                so = sum(1 for v in sub if v >= 1.5 * nominal or v <= 0.5 * nominal)
                out.append(f"| {si} | {ivt[a]/1e6:.1f}〜{ivt[b-1]/1e6:.1f} | {b-a} | "
                           f"{ss['mean']:.1f} | {sb:.2f} | {so} |")
            out.append("")
            out.append("境界の間隔(切替点):\n")
            out.append("```")
            for si, (a, b) in enumerate(segs[:-1]):
                lo_i = max(0, b - 2)
                hi_i = min(len(intervals), b + 3)
                out.append(f"seg{si}->seg{si+1} @ {ivt[b]/1e6:.3f}s : " +
                           " ".join(str(intervals[i]) for i in range(lo_i, hi_i)))
            out.append("```")
            out.append("")
    return "\n".join(out)


def analyze_txlog(path, label):
    """条件 D: PHASE13_TXLOG_TEST が出す送信側の打刻ログから σ を出す。
    形式は `PHASE13 TX <us>`(1 行 1 クロック)を想定する。"""
    ts = []
    import re as _re
    pat = _re.compile(r"PHASE13\s+TX\s+(\d+)")
    with open(path, errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                ts.append(int(m.group(1)))
    if len(ts) < 2:
        return f"### {label}\n\nPHASE13 TX の行が見つからない({path})\n"
    iv = [ts[i] - ts[i - 1] for i in range(1, len(ts))]
    st = stats(iv)
    dev = [abs(v - st['mean']) for v in iv]
    out = [f"### {label}(送信側打刻)\n", "| 項目 | 値 |", "|---|---|",
           f"| クロック数 | {len(ts)} |",
           f"| 間隔 min / mean / max | {st['min']} / {st['mean']:.1f} / {st['max']} µs |",
           f"| **送信側 σ** | **{st['sd']:.1f} µs** |",
           f"| 平均からの最大偏差 | {max(dev):.0f} µs |", ""]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv")
    ap.add_argument("--bpm", type=float, default=120.0)
    ap.add_argument("--from", dest="t_from", type=float, default=None)
    ap.add_argument("--to", dest="t_to", type=float, default=None)
    ap.add_argument("--label", default="measurement")
    ap.add_argument("--segments", default="none", choices=["none", "auto"])
    ap.add_argument("--txlog")
    ap.add_argument("--span", type=int, default=None,
                    help="この再生区間(1 始まり)だけを対象にする")
    a = ap.parse_args()

    if a.txlog:
        print(analyze_txlog(a.txlog, a.label))
        return
    if not a.csv:
        ap.error("--csv or --txlog is required")
    src, rows = read_csv(a.csv)
    if not rows:
        print(f"### {a.label}\n\n(受信イベントなし: {a.csv})")
        sys.exit(1)
    print(analyze(rows, a.bpm, a.t_from, a.t_to, a.label, a.segments, a.span))
    print(f"生データ: `{a.csv}`  /  受信元: {src}")


if __name__ == "__main__":
    main()
