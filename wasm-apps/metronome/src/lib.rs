// メトロノーム本体。Phase 13 で**音楽時間軸 API(transport / tempomap / seq)だけ**で
// 書き直した(移行ステップ 3。docs/architecture.md §10、docs/hostapi.md §6 要件 1)。
//
// 旧版(Phase 7B/7C/7D/8b)からの機能は維持する:
//   BPM 40-240(±5 / ±1、長押し連打加速)、拍子 2/3/4/6、START/STOP、拍ランプ、
//   小節頭のアクセント音(slot 1)、音量 V-/V+。
//
// 旧版との違い(内部だけ。使い勝手は同じ):
//   - クリックは `hostapi_tone_schedule` の毎 tick 再予約ではなく、
//     `seq_write`(port=CLICK / OP_TONE)で **playback tick** に予約する。
//     供給はプレフィックス受理契約どおり(docs/hostapi.md §5 / §10)。
//   - MIDI Clock はホスト(L1)が 40 tick グリッドから生成する。**アプリは
//     Start/Stop も含めて MIDI を一切送らない**(`hostapi_midi_send` は使わない。
//     送るとクロックが二重に出る)。
//   - テンポ / 拍子の変更は「位置 0 へ locate してマップの at_tick=0 を上書きする」
//     方式。旧版の rearm(now)(変更した瞬間から小節をやり直す)と同じ意味論で、
//     テンポマップのエントリが増えない(SEQCORE_TEMPO_MAX = 32 の枯渇を避ける)。
//     詳細は docs/results/phase13.md のステップ 1。
//
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
    fn hostapi_tone_define(slot: i32, wave: i32, freq_hz: i32, dur_ms: i32, level: i32) -> i32;
    fn hostapi_audio_set_volume(v: i32);

    fn hostapi_transport_start() -> i32;
    fn hostapi_transport_stop() -> i32;
    fn hostapi_transport_locate(song_tick: i32) -> i32;
    fn hostapi_transport_get_position(buf: *mut u8, buf_len: u32) -> i32;
    fn hostapi_tempomap_set_tempo(at_tick: i32, us_per_quarter: i32) -> i32;
    fn hostapi_tempomap_set_meter(at_tick: i32, numer: i32, denom: i32) -> i32;
    fn hostapi_seq_write(buf: *const u8, buf_len: u32) -> i32;
    fn hostapi_seq_filled_until() -> i32;
}

const PPQN: u32 = 960;
const BEAT: u32 = PPQN; // 4 分音符 = 1 拍(denom は常に 4)

const PORT_CLICK: u8 = 3;
const OP_TONE: u8 = 1;

// アクセント音のスロット(1 拍目用)。slot 0 は既定クリック(通常拍)のまま
const ACCENT_SLOT: u32 = 1;

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
const EV_TOUCH_UP: u16 = 2;

/// hostapi_seq_event_t(16 バイト、ABI 凍結)
#[repr(C)]
#[derive(Clone, Copy)]
struct SeqEvent {
    tick: u32,
    port: u8,
    status: u8,
    data1: u8,
    data2: u8,
    param: u32,
    reserved: u32,
}

impl SeqEvent {
    const fn zero() -> SeqEvent {
        SeqEvent { tick: 0, port: 0, status: 0, data1: 0, data2: 0, param: 0, reserved: 0 }
    }
}

// ---- レイアウト(320x240)。旧版から変更なし ----
const LAMP_Y: i32 = 76;
const LAMP_H: i32 = 36;
const LAMP_W: i32 = 44;
const LAMP_GAP: i32 = 8;
const LAMP_X0: i32 = 12;
const MAX_BEATS: usize = 6;

const FINE_Y: i32 = 120;
const FINE_H: i32 = 44;
const FINE_LABELS: [&[u8]; 4] = [b"-1", b"+1", b"V-", b"V+"];

const BTN_Y: i32 = 176;
const BTN_H: i32 = 52;
const BTN_W: i32 = 70;
const BTN_XS: [i32; 4] = [12, 90, 168, 246];
const BTN_LABELS: [&[u8]; 4] = [b"BPM-", b"BPM+", b"BEAT", b"START"];

const BPM_MIN: u32 = 40;
const BPM_MAX: u32 = 240;
const SIGS: [u32; 4] = [2, 3, 4, 6]; // 1 小節の拍数

const VOLUME_MIN: i32 = 0;
const VOLUME_MAX: i32 = 100;
const VOLUME_STEP: i32 = 10;

// 長押し連打加速(Phase 7D)。押下直後に 1 ステップ、HOLD_INITIAL_DELAY_MS 後から
// 自動連打を開始し、保持時間に応じて 400ms→200ms→100ms へ縮める。
const HOLD_INITIAL_DELAY_MS: u32 = 500;
const HOLD_ACCEL_1_MS: u32 = 1500;
const HOLD_ACCEL_2_MS: u32 = 3000;
const HOLD_INTERVAL_1_MS: u32 = 400;
const HOLD_INTERVAL_2_MS: u32 = 200;
const HOLD_INTERVAL_3_MS: u32 = 100;

static mut BPM: u32 = 120;
static mut SIG_IDX: usize = 2; // 4 拍子
static mut RUNNING: bool = false;
static mut VOLUME: i32 = 98; // ホスト既定(hostapi_audio_reset)と同値

// L2 の供給状態。OFFSET は song tick 0 に対応する playback tick
// (locate / start の直後に取り直す。それ以外では不変)
static mut OFFSET: u32 = 0;
static mut NEXT_BEAT: u32 = 0;
static mut LAMP_LIT: usize = usize::MAX;
static mut LAST_BEAT_KEY: u64 = u64::MAX;

// プレフィックス受理契約(docs/hostapi.md §5)の未受理分。1 拍 = 1 イベント
const CHUNK_MAX: usize = 1;
static mut PENDING: [SeqEvent; CHUNK_MAX] = [SeqEvent::zero(); CHUNK_MAX];
static mut PENDING_LEN: usize = 0;
static mut PENDING_OFF: usize = 0;

// 長押し連打の状態。HELD_DELTA==0 は「保持中の BPM ボタンなし」
static mut HELD_DELTA: i32 = 0;
static mut HELD_SINCE: u32 = 0;
static mut NEXT_REPEAT_AT: u32 = 0;

struct Line {
    buf: [u8; 48],
    len: usize,
}

impl Line {
    fn new() -> Line {
        Line { buf: [b' '; 48], len: 0 }
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
        let start = i;
        let n = digits.len() - start;
        for k in 0..n {
            let b = digits[start + k];
            if self.len < self.buf.len() {
                self.buf[self.len] = b;
                self.len += 1;
            }
        }
        self
    }
    fn draw(&self, x: i32, y: i32) {
        unsafe { hostapi_draw_text(x, y, self.buf.as_ptr(), self.len as u32) };
    }
}

/// transport 位置(hostapi_position_t の必要フィールドだけ)
struct Pos {
    tick: u32,
    song_tick: u32,
    bar: u32,
    beat: u16,
}

fn get_position() -> Option<Pos> {
    let mut b = [0u8; 32];
    if unsafe { hostapi_transport_get_position(b.as_mut_ptr(), 32) } != 0 {
        return None;
    }
    Some(Pos {
        tick: u32::from_le_bytes([b[8], b[9], b[10], b[11]]),
        song_tick: u32::from_le_bytes([b[12], b[13], b[14], b[15]]),
        bar: u32::from_le_bytes([b[16], b[17], b[18], b[19]]),
        beat: u16::from_le_bytes([b[24], b[25]]),
    })
}

/// BPM → µs / 4 分音符(SMF の set tempo と同じ単位)。端数は四捨五入する。
/// 残差は L1 のアンカーからの絶対計算で吸収され、蓄積しない。
fn upq_of(bpm: u32) -> i32 {
    ((60_000_000u32 + bpm / 2) / bpm) as i32
}

fn beats_per_bar() -> u32 {
    unsafe { SIGS[SIG_IDX] }
}

/// キューを捨てる操作(start / stop / locate)の後に呼ぶ。未受理分を破棄し、
/// song tick 0 = 拍 0 から供給し直す(docs/hostapi.md §5 の契約)。
fn resync() {
    unsafe {
        PENDING_LEN = 0;
        PENDING_OFF = 0;
        NEXT_BEAT = 0;
        OFFSET = match get_position() {
            Some(p) => p.tick.wrapping_sub(p.song_tick),
            None => 0,
        };
        LAST_BEAT_KEY = u64::MAX;
    }
}

/// 拍 i(song tick = i * BEAT)のクリックイベントを組み立てる。
/// 小節頭はアクセント用スロット、他は既定スロット。
fn build_beat(i: u32, out: &mut [SeqEvent; CHUNK_MAX]) -> usize {
    unsafe {
        let slot = if i % beats_per_bar() == 0 { ACCENT_SLOT } else { 0 };
        out[0] = SeqEvent {
            tick: OFFSET.wrapping_add(i.wrapping_mul(BEAT)),
            port: PORT_CLICK,
            status: OP_TONE,
            data1: 0,
            data2: 0,
            param: slot,
            reserved: 0,
        };
    }
    1
}

/// L2 の供給ループ(docs/hostapi.md §10)。受理されなかった残りは PENDING に
/// 持ち越して次回再送する(プレフィックス受理契約)。
fn supply(now_tick: u32) {
    unsafe {
        let horizon = beats_per_bar() * BEAT * 2; // 2 小節先まで
        loop {
            if PENDING_OFF == PENDING_LEN {
                if hostapi_seq_filled_until() >= now_tick.wrapping_add(horizon) as i32 {
                    return;
                }
                PENDING_LEN = build_beat(NEXT_BEAT, &mut PENDING);
                PENDING_OFF = 0;
                NEXT_BEAT += 1;
            }
            let remain = PENDING_LEN - PENDING_OFF;
            let ptr = PENDING.as_ptr().add(PENDING_OFF) as *const u8;
            let n = hostapi_seq_write(ptr, (remain * 16) as u32);
            if n <= 0 {
                return; // キュー満杯。次の tick で残りを再送する
            }
            PENDING_OFF += n as usize;
        }
    }
}

/// 演奏中に「今この瞬間から小節をやり直す」。song tick を 0 へ戻すだけで、
/// MIDI クロックのグリッド(playback tick 基準)には触らない。
fn restart_bar() {
    unsafe {
        if !RUNNING {
            return;
        }
        hostapi_transport_locate(0); // キューの未発火イベントは破棄される
        resync();
        if let Some(p) = get_position() {
            supply(p.tick);
        }
    }
}

/// テンポ変更。演奏中は「位置 0 へ戻して at_tick=0 のエントリを上書き」する
/// (旧版の rearm(now) と同じ意味論。テンポマップのエントリが増えない)。
///
/// locate と set_tempo の 2 呼び出しの間に song tick が 1 tick でも進むと
/// 「過去の at_tick は変更できない」規則で -1 になるため、数回だけ試す
/// (1 tick は 120bpm で 520µs あり、実際にはまず起きない)。
fn apply_tempo() {
    unsafe {
        if !RUNNING {
            hostapi_tempomap_set_tempo(0, upq_of(BPM));
            return;
        }
        for _ in 0..4 {
            hostapi_transport_locate(0);
            if hostapi_tempomap_set_tempo(0, upq_of(BPM)) == 0 {
                break;
            }
        }
        resync();
        if let Some(p) = get_position() {
            supply(p.tick);
        }
    }
}

/// 拍子変更。マップは at_tick=0 の 1 エントリを上書きする。演奏中は旧版と同じく
/// その場で小節をやり直す。
fn apply_meter() {
    unsafe {
        hostapi_tempomap_set_meter(0, beats_per_bar() as i32, 4);
    }
    restart_bar();
}

fn draw_status() {
    unsafe {
        let mut l = Line::new();
        l.push(b"BPM: ").push_u32(BPM).push(b"   beats/bar: ").push_u32(SIGS[SIG_IDX])
         .push(b"  Vol: ").push_u32(VOLUME as u32);
        l.draw(12, 50);
    }
}

fn draw_lamps(lit: usize) {
    unsafe {
        let n = SIGS[SIG_IDX] as usize;
        for i in 0..MAX_BEATS {
            let x = LAMP_X0 + (i as i32) * (LAMP_W + LAMP_GAP);
            let color = if i >= n {
                0x10_18_28 // 拍子の外は背景色で消す
            } else if i == lit {
                if i == 0 { 0xf0_80_20 } else { 0x30_c0_e0 } // 1 拍目はアクセント色
            } else {
                0x2a_33_40
            };
            hostapi_fill_rect(x, LAMP_Y, LAMP_W, LAMP_H, color);
        }
        LAMP_LIT = lit;
    }
}

fn draw_run_button() {
    unsafe {
        let (label, color): (&[u8], u32) = if RUNNING {
            (b"STOP ", 0xa0_30_30)
        } else {
            (b"START", 0x20_80_40)
        };
        hostapi_fill_rect(BTN_XS[3], BTN_Y, BTN_W, BTN_H, color);
        hostapi_draw_text(BTN_XS[3] + 10, BTN_Y + 16, label.as_ptr(), label.len() as u32);
    }
}

fn draw_buttons() {
    for i in 0..3 {
        unsafe {
            hostapi_fill_rect(BTN_XS[i], BTN_Y, BTN_W, BTN_H, 0x20_40_a0);
            hostapi_draw_text(BTN_XS[i] + 10, BTN_Y + 16, BTN_LABELS[i].as_ptr(),
                              BTN_LABELS[i].len() as u32);
        }
    }
    draw_run_button();
}

fn draw_fine_buttons() {
    for i in 0..4 {
        unsafe {
            hostapi_fill_rect(BTN_XS[i], FINE_Y, BTN_W, FINE_H, 0x18_50_70);
            hostapi_draw_text(BTN_XS[i] + 24, FINE_Y + 18, FINE_LABELS[i].as_ptr(),
                              FINE_LABELS[i].len() as u32);
        }
    }
}

/// now_ms は wraparound しうるので、差分を符号付きで見て到達判定する
fn time_reached(now: u32, target: u32) -> bool {
    (now.wrapping_sub(target) as i32) >= 0
}

fn apply_bpm_delta(delta: i32) {
    unsafe {
        let new_bpm = (BPM as i32 + delta).clamp(BPM_MIN as i32, BPM_MAX as i32) as u32;
        if new_bpm != BPM {
            BPM = new_bpm;
            apply_tempo();
        }
    }
    draw_status();
}

fn apply_volume_delta(delta: i32) {
    unsafe {
        let new_vol = (VOLUME + delta).clamp(VOLUME_MIN, VOLUME_MAX);
        if new_vol != VOLUME {
            VOLUME = new_vol;
            hostapi_audio_set_volume(VOLUME);
        }
    }
    draw_status();
}

fn start_repeat(delta: i32, now: u32) {
    apply_bpm_delta(delta);
    unsafe {
        HELD_DELTA = delta;
        HELD_SINCE = now;
        NEXT_REPEAT_AT = now.wrapping_add(HOLD_INITIAL_DELAY_MS);
    }
}

fn process_repeat(now: u32) {
    unsafe {
        if HELD_DELTA == 0 || !time_reached(now, NEXT_REPEAT_AT) {
            return;
        }
        let delta = HELD_DELTA;
        apply_bpm_delta(delta);
        let elapsed = now.wrapping_sub(HELD_SINCE);
        let interval: u32 = if elapsed < HOLD_ACCEL_1_MS {
            HOLD_INTERVAL_1_MS
        } else if elapsed < HOLD_ACCEL_2_MS {
            HOLD_INTERVAL_2_MS
        } else {
            HOLD_INTERVAL_3_MS
        };
        NEXT_REPEAT_AT = now.wrapping_add(interval);
    }
}

fn toggle_run() {
    unsafe {
        if RUNNING {
            hostapi_transport_stop(); // 0xFC を送出、キュー破棄、クロック停止
            RUNNING = false;
            PENDING_LEN = 0;
            PENDING_OFF = 0;
            NEXT_BEAT = 0;
            draw_lamps(usize::MAX);
        } else {
            // マップは常に at_tick=0 の 1 エントリ。start は song tick 0 から始まる
            hostapi_tempomap_set_meter(0, beats_per_bar() as i32, 4);
            hostapi_tempomap_set_tempo(0, upq_of(BPM));
            hostapi_transport_start(); // 0xFA を送出、クロック生成を開始
            RUNNING = true;
            resync();
            if let Some(p) = get_position() {
                supply(p.tick);
            }
        }
        draw_run_button();
        draw_status();
    }
}

fn handle_tap(x: i16, y: i16) {
    let (x, y) = (x as i32, y as i32);
    let now = unsafe { hostapi_now_ms() };

    // -1 / +1 / V- / V+(Phase 7D の行)
    if y >= FINE_Y && y < FINE_Y + FINE_H {
        for i in 0..4 {
            if x >= BTN_XS[i] && x < BTN_XS[i] + BTN_W {
                match i {
                    0 => start_repeat(-1, now),
                    1 => start_repeat(1, now),
                    2 => apply_volume_delta(-VOLUME_STEP),
                    3 => apply_volume_delta(VOLUME_STEP),
                    _ => {}
                }
                break;
            }
        }
        return;
    }

    if y < BTN_Y || y >= BTN_Y + BTN_H {
        return;
    }
    unsafe {
        for i in 0..4 {
            if x >= BTN_XS[i] && x < BTN_XS[i] + BTN_W {
                match i {
                    0 => start_repeat(-5, now),
                    1 => start_repeat(5, now),
                    2 => {
                        SIG_IDX = (SIG_IDX + 1) % SIGS.len();
                        apply_meter();
                        draw_lamps(usize::MAX);
                        draw_status();
                    }
                    3 => toggle_run(),
                    _ => {}
                }
                break;
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        hostapi_fill_rect(0, 0, 320, 40, 0x90_30_50); // タイトルバー
        hostapi_fill_rect(0, 40, 320, 200, 0x10_18_28); // 背景
        let title = b"metronome (wasm)";
        hostapi_draw_text(12, 12, title.as_ptr(), title.len() as u32);

        BPM = 120;
        SIG_IDX = 2;
        RUNNING = false;
        VOLUME = 98;
        HELD_DELTA = 0;
        OFFSET = 0;
        NEXT_BEAT = 0;
        PENDING_LEN = 0;
        PENDING_OFF = 0;
        LAMP_LIT = usize::MAX;
        LAST_BEAT_KEY = u64::MAX;

        // 1 拍目のアクセント音(高いピッチ)。通常拍は slot 0 の既定クリック
        hostapi_tone_define(ACCENT_SLOT as i32, 0 /*SINE*/, 1568, 30, 100);

        // テンポ / 拍子マップは常に at_tick=0 の 1 エントリだけを上書きして使う
        hostapi_tempomap_set_meter(0, beats_per_bar() as i32, 4);
        hostapi_tempomap_set_tempo(0, upq_of(BPM));
    }
    draw_status();
    draw_lamps(usize::MAX);
    draw_fine_buttons();
    draw_buttons();
    0
}

#[no_mangle]
pub extern "C" fn app_tick() {
    let mut evs = [Event { ev_type: 0, param: 0, x: 0, y: 0, time_ms: 0 }; 8];
    let n = unsafe {
        hostapi_poll_event(evs.as_mut_ptr() as *mut u8,
                           (8 * core::mem::size_of::<Event>()) as u32)
    };
    for ev in &evs[..n.max(0) as usize] {
        if ev.ev_type == EV_TOUCH_DOWN {
            handle_tap(ev.x, ev.y);
        } else if ev.ev_type == EV_TOUCH_UP {
            unsafe { HELD_DELTA = 0; }
        }
    }

    let now = unsafe { hostapi_now_ms() };
    process_repeat(now);

    unsafe {
        if !RUNNING {
            return;
        }
        let p = match get_position() {
            Some(p) => p,
            None => return,
        };

        // 2 小節先まで先読み供給する(実時間はアプリでは一切扱わない)
        supply(p.tick);

        // 拍ランプ(視覚は tick 格子で十分。最大 100ms 遅れる)
        let key = (p.bar as u64) * (beats_per_bar() as u64) + p.beat as u64;
        if key != LAST_BEAT_KEY {
            LAST_BEAT_KEY = key;
            draw_lamps(p.beat as usize);
        }
    }
}
