// Phase 9b Stage 1/2/3: MIDI ループバック診断アプリ
// (配線チェック・テンポ表示・診断統計)。
// 自機の MIDI OUT(hostapi_midi_send による Start/Stop、host 内部の
// 24ppqn クロック生成)を自機の MIDI IN(hostapi_midi_recv)で受信し、
// 受信生バイトを 16進表示する。パースは一切アプリ側で行う
// (shared/hostapi_defs.h の "midi" セクション参照、Host API 側はタイム
// スタンプ付き生バイトを渡すのみ)。
//
// クロック生成は既存メトロノーム(Phase 7B/8b)と同じ方式: BPM 120 固定で
// hostapi_click_schedule を毎 tick 再予約し、host がその予約間隔から
// MIDI Clock の周期を自動導出する(テンポをアプリから明示的に伝えない)。
// ホスト API (module "env") のみ使用。no_std / アロケータ不要。
#![no_std]

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

extern "C" {
    fn hostapi_draw_text(x: i32, y: i32, ptr: *const u8, len: u32);
    fn hostapi_fill_rect(x: i32, y: i32, w: i32, h: i32, rgb888: u32);
    fn hostapi_poll_event(buf: *mut u8, buf_len: u32) -> i32;
    fn hostapi_now_ms() -> u32;
    fn hostapi_click_schedule(time_ms: i32) -> i32;
    fn hostapi_midi_send(bytes: *const u8, len: u32) -> i32;
    fn hostapi_midi_recv(buf: *mut u8, buf_len: u32) -> i32;
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Event {
    ev_type: u16,
    param: u16,
    x: i16,
    y: i16,
    time_ms: u32,
}

const EV_TOUCH_DOWN: u16 = 1;

// shared/hostapi_defs.h の hostapi_midi_recv_t と同一レイアウト
// (16 bytes, LE, ABI 凍結)。
#[repr(C)]
#[derive(Clone, Copy)]
struct MidiRecv {
    timestamp_us: u64,
    byte: u8,
    _reserved: [u8; 7],
}

const MIDI_START: [u8; 1] = [0xFA];
const MIDI_STOP: [u8; 1] = [0xFC];
const PERIOD_MS: u32 = 500; // BPM 120 固定(診断用の測定条件)

const RECV_BUF_LEN: usize = 64; // 1 tick(100ms)分のバーストを吸収するのに十分な余裕
const RECENT_LEN: usize = 16; // Stage 1 の 16進表示(8バイト x 2行)
const MIDI_CLOCK: u8 = 0xF8;
const CLOCK_PPQN: u32 = 24; // MIDI Clock は 1/4 音符あたり 24 クロック
const AVG_WINDOW: usize = 24; // Stage 2: 直近 24 クロック(=1拍)の移動平均

// Stage 3: 120bpm(24ppqn)の公称クロック間隔。60,000,000 / (120*24) =
// 20833.33...µs だが整数表示のため 20833 に丸める(バイアス ~0.33µs は
// 実測ジッタ(数百µs オーダー)に対して無視できる)。
const NOMINAL_INTERVAL_US: i64 = 20833;

const BTN_X: i32 = 110;
const BTN_Y: i32 = 190;
const BTN_W: i32 = 100;
const BTN_H: i32 = 40;

static mut RUNNING: bool = false;
static mut ANCHOR: u32 = 0;
static mut LAST_BEAT: u32 = u32::MAX;

static mut RECENT: [u8; RECENT_LEN] = [0; RECENT_LEN];
static mut RECENT_COUNT: usize = 0; // 0..=RECENT_LEN(埋まっている件数)
static mut TOTAL_BYTES: u32 = 0;

// Stage 2: 実測 BPM(直近 AVG_WINDOW 件のクロック間隔の移動平均から算出)。
// タイムスタンプは hostapi_midi_recv のレコード値(host 打刻の µs)のみを
// 使う(app_tick の呼び出しタイミングは時刻として使わない)。
static mut LAST_CLOCK_TS_US: u64 = 0; // 0 = 前回クロック未受信
static mut CLOCK_RING: [u32; AVG_WINDOW] = [0; AVG_WINDOW];
static mut CLOCK_RING_IDX: usize = 0;
static mut CLOCK_RING_FILLED: usize = 0; // 0..=AVG_WINDOW
static mut CLOCK_RING_SUM_US: u64 = 0; // CLOCK_RING の現在有効分の合計(O(1) 平均用)

// Stage 3: セッション全体(START からの)診断統計。整数 Welford 法で
// 平均・分散をオーバーフローなく逐次計算する(sum/sum^2 の直接保持は
// 長時間実行で桁あふれしうるため避ける)。
static mut STAT_COUNT: u32 = 0; // 区間(interval)のサンプル数
static mut STAT_MEAN_US: i64 = 0; // 逐次平均(整数丸め、簡易実装)
static mut STAT_M2: i64 = 0; // 偏差二乗和(分散 = M2 / count)
static mut STAT_MIN_US: u32 = u32::MAX;
static mut STAT_MAX_US: u32 = 0;
static mut CLOCK_COUNT: u32 = 0; // 受信した 0xF8 の総数(区間数 = CLOCK_COUNT-1)
static mut FIRST_CLOCK_TS_US: u64 = 0; // 0 = 未受信(期待クロック数の起点)

// ---- Phase 9c E1: 分布指標(ヒストグラム・ロバスト統計・外れ値・BPM分布) ----
// バケット境界は「有力仮説」節の予測値(公称 20833us / 欠落時 約41674us)を
// 分離できるよう設計: <20000, 20000-20499, 20500-20999, 21000-21499,
// 21500-29999, 30000-39999, 40000-44999, 45000+
//
// 実装上の重要な変更(2026-08-23、実機検証で判明): 当初は生サンプルを
// [u16; 6000](約12KB)で保持して真の中央値を算出する設計だったが、実機で
// "WASM module instantiate failed: allocate linear memory failed" となり
// 起動不能になった(Linux ホストでは同一 WAMR プール 48KB でも成功しており、
// 実機側のプール断片化が Linux より厳しいためと推定)。そのため生サンプルの
// 保持をやめ、静的メモリの増分を数百バイトに抑える方式に変更した:
// - 「中央値」はヒストグラムの累積カウントから該当バケットの代表値を返す
//   近似値(厳密な中央値ではない。ログには "med~=" と表記して区別する)。
// - 外れ値除外統計(ロバスト平均・σ・min/max)は「中央値の1.5倍」ではなく
//   公称間隔(NOMINAL_INTERVAL_US)の1.5倍を固定閾値とした逐次 Welford 計算。
//   公称値と欠落時の予測値(約41674us、公称の約2倍)は 2 倍以上離れており、
//   閾値をどちらのベースにしても分離結果はほぼ変わらない。
const HIST_BUCKETS: usize = 8;
const HIST_REPR_US: [u32; HIST_BUCKETS] = [19750, 20250, 20750, 21250, 25750, 35000, 42500, 50000]; // 各バケットの代表値(概ね中点)
const OUTLIER_LIVE_CAP: usize = 8;
const BPM_DIST_BUCKETS: usize = 5; // 114-116 / 116-118 / 118-119.5 / 119.5-120.5 / その他

static mut HIST_COUNTS: [u32; HIST_BUCKETS] = [0; HIST_BUCKETS];

// ロバスト統計: 公称値(NOMINAL_INTERVAL_US)の1.5倍を閾値とし、それ以下の
// 区間だけを対象に整数 Welford 法で逐次計算する(STAT_* と同様の方式)。
static mut EX_COUNT: u32 = 0;
static mut EX_MEAN_US: i64 = 0;
static mut EX_M2: i64 = 0;
static mut EX_MIN_US: u32 = u32::MAX;
static mut EX_MAX_US: u32 = 0;
static mut OUT_COUNT: u32 = 0; // 閾値超(外れ値)の総数

// 外れ値の実測値そのもの(リングバッファ、直近 OUTLIER_LIVE_CAP 件)。
static mut OUTLIER_LIVE: [u32; OUTLIER_LIVE_CAP] = [0; OUTLIER_LIVE_CAP];

static mut BPM_DIST: [u32; BPM_DIST_BUCKETS] = [0; BPM_DIST_BUCKETS];

// STOP 時のログ出力専用センチネル座標(Phase 9c 検証用)。x を画面幅(320)
// より十分大きい値にすることで、hostapi.cpp 側の検証専用ガード
// (PHASE9C_STATLOG_TEST)がこの draw_text 呼び出しだけを判別してシリアル
// ログへ転送できる(Host API のシグネチャ・contractは一切変更しない)。
// 画面上は非表示(retained モデルのオフスクリーン描画、実害なし)。
const LOG_X: i32 = 9000;
const LOG_Y_BASE: i32 = 0;

fn hist_bucket(x: u32) -> usize {
    if x < 20000 {
        0
    } else if x < 20500 {
        1
    } else if x < 21000 {
        2
    } else if x < 21500 {
        3
    } else if x < 30000 {
        4
    } else if x < 40000 {
        5
    } else if x < 45000 {
        6
    } else {
        7
    }
}

/// ヒストグラムの累積カウントから中央値が属するバケットを求め、その代表値
/// (HIST_REPR_US)を近似中央値として返す(厳密な中央値ではない)。
fn approx_median_us() -> i64 {
    unsafe {
        let mut total: u32 = 0;
        for i in 0..HIST_BUCKETS {
            total += HIST_COUNTS[i];
        }
        if total == 0 {
            return 0;
        }
        let half = total / 2;
        let mut cum: u32 = 0;
        for i in 0..HIST_BUCKETS {
            cum += HIST_COUNTS[i];
            if cum > half {
                return HIST_REPR_US[i] as i64;
            }
        }
        HIST_REPR_US[HIST_BUCKETS - 1] as i64
    }
}

/// v_x100 は BPM の x100 固定小数点
fn bpm_dist_bucket(v_x100: i64) -> usize {
    if v_x100 >= 11400 && v_x100 < 11600 {
        0
    } else if v_x100 >= 11600 && v_x100 < 11800 {
        1
    } else if v_x100 >= 11800 && v_x100 < 11950 {
        2
    } else if v_x100 >= 11950 && v_x100 < 12050 {
        3
    } else {
        4
    }
}

struct Line {
    buf: [u8; 64],
    len: usize,
}

impl Line {
    fn new() -> Line {
        Line { buf: [b' '; 64], len: 0 }
    }
    fn push(&mut self, s: &[u8]) -> &mut Line {
        for &b in s {
            if self.len < self.buf.len() {
                self.buf[self.len] = b;
                self.len += 1;
            }
        }
        self
    }
    fn push_u32(&mut self, mut v: u32) -> &mut Line {
        let mut digits = [0u8; 10];
        let mut i = digits.len();
        loop {
            i -= 1;
            digits[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        self.push(&digits[i..])
    }
    fn push_i32(&mut self, v: i32) -> &mut Line {
        if v < 0 {
            self.push(b"-").push_u32(v.unsigned_abs())
        } else {
            self.push_u32(v as u32)
        }
    }
    fn push_hex_u8(&mut self, v: u8) -> &mut Line {
        const HEX: &[u8; 16] = b"0123456789ABCDEF";
        self.push(&[HEX[(v >> 4) as usize], HEX[(v & 0x0f) as usize]])
    }
    /// v は実値を 100 倍した固定小数点(例: 119.42 -> 11942)。小数 2 桁で表示
    fn push_fixed100(&mut self, v: i64) -> &mut Line {
        if v < 0 {
            self.push(b"-");
        }
        let a = v.unsigned_abs();
        self.push_u32((a / 100) as u32).push(b".");
        let frac = (a % 100) as u32;
        if frac < 10 {
            self.push(b"0");
        }
        self.push_u32(frac)
    }
    fn draw(&self, x: i32, y: i32) {
        unsafe { hostapi_draw_text(x, y, self.buf.as_ptr(), self.len as u32) };
    }
}

fn push_recent(byte: u8) {
    unsafe {
        if RECENT_COUNT < RECENT_LEN {
            RECENT[RECENT_COUNT] = byte;
            RECENT_COUNT += 1;
        } else {
            for i in 0..RECENT_LEN - 1 {
                RECENT[i] = RECENT[i + 1];
            }
            RECENT[RECENT_LEN - 1] = byte;
        }
        TOTAL_BYTES = TOTAL_BYTES.saturating_add(1);
    }
}

/// u64 の整数平方根(Newton法)。Stage 3 の σ 計算用
fn isqrt(v: u64) -> u64 {
    if v == 0 {
        return 0;
    }
    let mut x = v;
    let mut y = (x + 1) / 2;
    while y < x {
        x = y;
        y = (x + v / x) / 2;
    }
    x
}

/// Stage 3: 整数 Welford 法でセッション全体の平均・分散を逐次更新する。
/// 平均は整数丸め(簡易実装、指示書の許容範囲)。sum/sum^2 を直接持たない
/// ため長時間実行でもオーバーフローしない
fn welford_update(x_us: u32) {
    unsafe {
        STAT_COUNT += 1;
        let x = x_us as i64;
        let delta = x - STAT_MEAN_US;
        STAT_MEAN_US += delta / STAT_COUNT as i64;
        let delta2 = x - STAT_MEAN_US;
        STAT_M2 += delta * delta2;
        if x_us < STAT_MIN_US {
            STAT_MIN_US = x_us;
        }
        if x_us > STAT_MAX_US {
            STAT_MAX_US = x_us;
        }
    }
}

/// MIDI Clock(0xF8)受信を記録する。record_ts_us は hostapi_midi_recv の
/// タイムスタンプ(µs、host が受信直後に打刻)。直前クロックとの間隔を
/// リングバッファ(Stage 2)とセッション統計(Stage 3)へ積む(初回受信時は
/// 間隔なしなのでスキップ)。
fn note_clock(record_ts_us: u64) {
    unsafe {
        CLOCK_COUNT = CLOCK_COUNT.saturating_add(1);
        if FIRST_CLOCK_TS_US == 0 {
            FIRST_CLOCK_TS_US = record_ts_us;
        }
        if LAST_CLOCK_TS_US != 0 && record_ts_us > LAST_CLOCK_TS_US {
            let interval_us = (record_ts_us - LAST_CLOCK_TS_US) as u32;
            if CLOCK_RING_FILLED < AVG_WINDOW {
                CLOCK_RING[CLOCK_RING_FILLED] = interval_us;
                CLOCK_RING_SUM_US += interval_us as u64;
                CLOCK_RING_FILLED += 1;
                CLOCK_RING_IDX = CLOCK_RING_FILLED % AVG_WINDOW;
            } else {
                CLOCK_RING_SUM_US -= CLOCK_RING[CLOCK_RING_IDX] as u64;
                CLOCK_RING[CLOCK_RING_IDX] = interval_us;
                CLOCK_RING_SUM_US += interval_us as u64;
                CLOCK_RING_IDX = (CLOCK_RING_IDX + 1) % AVG_WINDOW;
            }
            welford_update(interval_us);

            // ---- Phase 9c E1: ヒストグラム / ロバスト統計(逐次) / 外れ値 / BPM分布 ----
            HIST_COUNTS[hist_bucket(interval_us)] += 1;
            // 外れ値判定: 公称値(NOMINAL_INTERVAL_US)の1.5倍を固定閾値とする
            // (中央値ベースではない簡易版。上のコメント参照)。
            if (interval_us as i64) <= NOMINAL_INTERVAL_US * 3 / 2 {
                EX_COUNT += 1;
                let x = interval_us as i64;
                let delta = x - EX_MEAN_US;
                EX_MEAN_US += delta / EX_COUNT as i64;
                let delta2 = x - EX_MEAN_US;
                EX_M2 += delta * delta2;
                if interval_us < EX_MIN_US {
                    EX_MIN_US = interval_us;
                }
                if interval_us > EX_MAX_US {
                    EX_MAX_US = interval_us;
                }
            } else {
                let pos = (OUT_COUNT as usize) % OUTLIER_LIVE_CAP;
                OUTLIER_LIVE[pos] = interval_us;
                OUT_COUNT = OUT_COUNT.saturating_add(1);
            }
            if CLOCK_RING_FILLED == AVG_WINDOW {
                if let Some(bpm) = estimate_bpm_x100() {
                    BPM_DIST[bpm_dist_bucket(bpm)] += 1;
                }
            }
        }
        LAST_CLOCK_TS_US = record_ts_us;
    }
}

fn reset_clock_stats() {
    unsafe {
        LAST_CLOCK_TS_US = 0;
        CLOCK_RING_IDX = 0;
        CLOCK_RING_FILLED = 0;
        CLOCK_RING_SUM_US = 0;
        STAT_COUNT = 0;
        STAT_MEAN_US = 0;
        STAT_M2 = 0;
        STAT_MIN_US = u32::MAX;
        STAT_MAX_US = 0;
        CLOCK_COUNT = 0;
        FIRST_CLOCK_TS_US = 0;
        HIST_COUNTS = [0; HIST_BUCKETS];
        EX_COUNT = 0;
        EX_MEAN_US = 0;
        EX_M2 = 0;
        EX_MIN_US = u32::MAX;
        EX_MAX_US = 0;
        OUT_COUNT = 0;
        BPM_DIST = [0; BPM_DIST_BUCKETS];
    }
}

/// Phase 9c E1: STOP 時に全統計値を 1 行ずつ、画面外センチネル座標
/// (LOG_X, LOG_Y_BASE)へ draw_text する。hostapi.cpp 側の検証専用ガード
/// (PHASE9C_STATLOG_TEST)がこれを検知してシリアルログへ転送する
/// (docs/prompts/phase09c.md の要求: 「測定値の読み取りはカメラ静止画に
/// 頼らないこと」への対応)。画面上は非表示のため通常表示には影響しない。
fn dump_stop_stats() {
    unsafe {
        // 1行目: ヒストグラム(8バケット)
        let mut l = Line::new();
        l.push(b"H");
        for i in 0..HIST_BUCKETS {
            l.push(b" ").push_u32(HIST_COUNTS[i]);
        }
        l.draw(LOG_X, LOG_Y_BASE);

        let median = approx_median_us(); // ヒストグラム近似(厳密な中央値ではない)
        let ex_n = EX_COUNT;
        let ex_mean = EX_MEAN_US;
        let variance = if EX_COUNT > 0 {
            (EX_M2 / EX_COUNT as i64).max(0) as u64
        } else {
            0
        };
        let ex_sigma = isqrt(variance) as u32;
        let ex_min = if EX_COUNT > 0 { EX_MIN_US } else { 0 };
        let ex_max = EX_MAX_US;
        let out_n = OUT_COUNT;

        // 2行目: 近似中央値・ロバスト統計のサンプル数・ロバスト平均
        let mut l2 = Line::new();
        l2.push(b"R med~=").push_i32(median as i32);
        l2.push(b" exN=").push_u32(ex_n);
        l2.push(b" exMean=").push_i32(ex_mean as i32);
        l2.draw(LOG_X, LOG_Y_BASE + 16);

        // 3行目: ロバスト σ・min/max・外れ値件数
        let mut l3 = Line::new();
        l3.push(b"R exSig=").push_u32(ex_sigma);
        l3.push(b" exMin=").push_u32(ex_min);
        l3.push(b" exMax=").push_u32(ex_max);
        l3.push(b" outN=").push_u32(out_n);
        l3.draw(LOG_X, LOG_Y_BASE + 32);

        // 4行目: 外れ値(公称値x1.5閾値)の実測値そのもの(最大8件)
        let mut l4 = Line::new();
        l4.push(b"O cnt=").push_u32(OUT_COUNT);
        let cnt = OUT_COUNT.min(OUTLIER_LIVE_CAP as u32) as usize;
        for i in 0..cnt {
            l4.push(b" ").push_u32(OUTLIER_LIVE[i]);
        }
        l4.draw(LOG_X, LOG_Y_BASE + 48);

        // 5行目: 見かけ BPM 分布(24クロック移動平均、5バケット)
        let mut l5 = Line::new();
        l5.push(b"B");
        for i in 0..BPM_DIST_BUCKETS {
            l5.push(b" ").push_u32(BPM_DIST[i]);
        }
        l5.draw(LOG_X, LOG_Y_BASE + 64);
    }
}

/// 直近クロック間隔の移動平均から BPM を算出(x100 固定小数点)。
/// クロックが 1 件も溜まっていなければ None
fn estimate_bpm_x100() -> Option<i64> {
    unsafe {
        if CLOCK_RING_FILLED == 0 {
            return None;
        }
        let avg_interval_us = CLOCK_RING_SUM_US / CLOCK_RING_FILLED as u64;
        if avg_interval_us == 0 {
            return None;
        }
        let quarter_period_us = avg_interval_us * CLOCK_PPQN as u64;
        // bpm = 60,000,000 / quarter_period_us、x100 固定小数点
        Some((6_000_000_000i64) / quarter_period_us as i64)
    }
}

fn draw_bpm() {
    let mut l = Line::new();
    l.push(b"BPM: ");
    match estimate_bpm_x100() {
        Some(v) => {
            l.push_fixed100(v);
        }
        None => {
            l.push(b"--");
        }
    }
    l.draw(12, 98);
}

/// Stage 3: 平均間隔の公称値(NOMINAL_INTERVAL_US)からの偏差(µs)と
/// サンプル数
fn draw_deviation() {
    let mut l = Line::new();
    unsafe {
        if STAT_COUNT == 0 {
            l.push(b"avg dev: --");
        } else {
            let dev = STAT_MEAN_US - NOMINAL_INTERVAL_US;
            l.push(b"avg dev: ").push_i32(dev as i32).push(b"us  n=").push_u32(STAT_COUNT);
        }
    }
    l.draw(12, 114);
}

/// Stage 3: クロック間隔の min/max(µs)
fn draw_minmax() {
    let mut l = Line::new();
    unsafe {
        if STAT_COUNT == 0 {
            l.push(b"min/max: --");
        } else {
            l.push(b"min/max: ").push_u32(STAT_MIN_US).push(b"/").push_u32(STAT_MAX_US).push(b"us");
        }
    }
    l.draw(12, 130);
}

/// Stage 3: クロック間隔の標準偏差 σ(µs)
fn draw_sigma() {
    let mut l = Line::new();
    unsafe {
        if STAT_COUNT == 0 {
            l.push(b"sigma: --");
        } else {
            let variance = (STAT_M2 / STAT_COUNT as i64).max(0) as u64;
            let sigma = isqrt(variance);
            l.push(b"sigma: ").push_u32(sigma as u32).push(b"us");
        }
    }
    l.draw(12, 146);
}

/// Stage 3: 受信クロック数 vs 経過時間から期待されるクロック数。
/// 期待値も受信タイムスタンプ(RX 記録の時間軸)のみから算出し、
/// app_tick 呼び出しタイミングは使わない
fn draw_clock_count() {
    let mut l = Line::new();
    unsafe {
        l.push(b"clocks: ").push_u32(CLOCK_COUNT).push(b" / exp ");
        if FIRST_CLOCK_TS_US == 0 || LAST_CLOCK_TS_US <= FIRST_CLOCK_TS_US {
            l.push(b"--");
        } else {
            let elapsed_us = (LAST_CLOCK_TS_US - FIRST_CLOCK_TS_US) as i64;
            let expected = 1 + (elapsed_us / NOMINAL_INTERVAL_US) as u32;
            l.push_u32(expected);
        }
    }
    l.draw(12, 162);
}

fn draw_counts() {
    let mut l = Line::new();
    l.push(b"RX bytes: ").push_u32(unsafe { TOTAL_BYTES });
    l.draw(12, 50);
}

fn draw_hex() {
    unsafe {
        for row in 0..2 {
            let mut l = Line::new();
            for col in 0..8 {
                let idx = row * 8 + col;
                if idx < RECENT_COUNT {
                    l.push_hex_u8(RECENT[idx]);
                } else {
                    l.push(b"--");
                }
                l.push(b" ");
            }
            l.draw(12, 66 + row as i32 * 16);
        }
    }
}

fn draw_button() {
    let (label, color): (&[u8], u32) = if unsafe { RUNNING } {
        (b"STOP", 0xa0_30_30)
    } else {
        (b"START", 0x20_80_40)
    };
    unsafe { hostapi_fill_rect(BTN_X, BTN_Y, BTN_W, BTN_H, color) };
    unsafe { hostapi_draw_text(BTN_X + 24, BTN_Y + 14, label.as_ptr(), label.len() as u32) };
}

fn in_button(x: i16, y: i16) -> bool {
    let (x, y) = (x as i32, y as i32);
    x >= BTN_X && x < BTN_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H
}

fn reset_stats() {
    unsafe {
        RECENT_COUNT = 0;
        TOTAL_BYTES = 0;
    }
    reset_clock_stats();
}

fn toggle_running(now: u32) {
    unsafe {
        RUNNING = !RUNNING;
        if RUNNING {
            ANCHOR = now;
            LAST_BEAT = u32::MAX;
            reset_stats();
            hostapi_midi_send(MIDI_START.as_ptr(), 1);
        } else {
            dump_stop_stats(); // Phase 9c E1: 全統計値をログ用センチネルへ出力
            hostapi_click_schedule(0); // 予約キャンセル
            hostapi_midi_send(MIDI_STOP.as_ptr(), 1);
        }
    }
    draw_button();
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        hostapi_fill_rect(0, 0, 320, 40, 0x30_50_90); // タイトルバー
        hostapi_fill_rect(0, 40, 320, 200, 0x10_18_28); // 背景
        let title = b"midi_loopback (wasm)";
        hostapi_draw_text(12, 12, title.as_ptr(), title.len() as u32);

        RUNNING = false;
        ANCHOR = 0;
        LAST_BEAT = u32::MAX;
    }
    reset_stats();
    draw_counts();
    draw_hex();
    draw_bpm();
    draw_deviation();
    draw_minmax();
    draw_sigma();
    draw_clock_count();
    draw_button();
    0
}

#[no_mangle]
pub extern "C" fn app_tick() {
    let mut evs = [Event { ev_type: 0, param: 0, x: 0, y: 0, time_ms: 0 }; 8];
    let n = unsafe {
        hostapi_poll_event(evs.as_mut_ptr() as *mut u8,
                           (8 * core::mem::size_of::<Event>()) as u32)
    };
    let now = unsafe { hostapi_now_ms() };
    for ev in &evs[..n.max(0) as usize] {
        if ev.ev_type == EV_TOUCH_DOWN && in_button(ev.x, ev.y) {
            toggle_running(now);
        }
    }

    unsafe {
        if RUNNING {
            let elapsed = now.wrapping_sub(ANCHOR);
            let beat = elapsed / PERIOD_MS;
            // 次の拍を毎 tick 再予約(host 側の last_fired ガードで二重発音しない)。
            let next = ANCHOR.wrapping_add((beat + 1) * PERIOD_MS);
            hostapi_click_schedule(next as i32);
            LAST_BEAT = beat;
        }
    }

    // MIDI IN の受信ドレイン。タイムスタンプは hostapi_midi_recv のレコード値
    // (host が受信直後に打刻した µs 値)のみを使う(app_tick 呼び出しの
    // タイミングは時刻として使わない、約 5ms のジッタがあるため)。
    let mut recs = [MidiRecv { timestamp_us: 0, byte: 0, _reserved: [0; 7] }; RECV_BUF_LEN];
    let n = unsafe {
        hostapi_midi_recv(recs.as_mut_ptr() as *mut u8,
                          (RECV_BUF_LEN * core::mem::size_of::<MidiRecv>()) as u32)
    };
    for rec in &recs[..n.max(0) as usize] {
        push_recent(rec.byte);
        if rec.byte == MIDI_CLOCK {
            note_clock(rec.timestamp_us);
        }
    }

    draw_counts();
    draw_hex();
    draw_bpm();
    draw_deviation();
    draw_minmax();
    draw_sigma();
    draw_clock_count();
}
