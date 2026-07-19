// Phase 7B: メトロノーム本体。hostapi_click_schedule(7A)の上に
// 可変 BPM(40-240)・拍子(2/3/4/6)・START/STOP・拍ランプを実装する。
// - 発音はホストの予約発音(±µs 級)。アプリは毎 tick「次の拍」を再予約するだけ。
// - 拍時刻は anchor + n*60000/bpm を拍ごとに計算(累積丸め誤差なし。
//   BPM 132 = 454.545ms のような割り切れない周期でもドリフトしない)。
// - 拍ランプは tick(100ms 格子)で更新される視覚表示。1 拍目はアクセント色。
//   音のアクセント(音色変更)は API v2 の相談事項として 7C へ。
// Phase 7D: テンポ 1 刻み(-1/+1 ボタン、既存 BPM±5 と長押し連打加速を追加)、
// ボリューム調整(V-/V+、hostapi_audio_set_volume によるマスター音量)を追加。
// いずれも既存 Host API のみで完結(API/ABI 変更なし)。
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
    fn hostapi_tone_define(slot: i32, wave: i32, freq_hz: i32, dur_ms: i32, level: i32) -> i32;
    fn hostapi_tone_schedule(slot: i32, time_ms: i32) -> i32;
    fn hostapi_audio_set_volume(v: i32);
}

// アクセント音のスロット(1 拍目用)。slot 0 は既定クリック(通常拍)のまま
const ACCENT_SLOT: i32 = 1;

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

// ---- レイアウト(320x240) ----
const LAMP_Y: i32 = 76;
const LAMP_H: i32 = 36;
const LAMP_W: i32 = 44;
const LAMP_GAP: i32 = 8;
const LAMP_X0: i32 = 12;
const MAX_BEATS: usize = 6;

// Phase 7D: ランプ行(下端112)とボタン行(上端176)の間の空き帯に
// -1/+1(テンポ微調整)・V-/V+(音量)の新規行を追加。x/幅は既存ボタン行と共用。
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
const VOLUME_STEP: i32 = 10; // mp3player(6B/6C)と同じ刻み

// 長押し連打加速(タスク1)。押下直後に1ステップ、HOLD_INITIAL_DELAY_MS 経過後
// から自動連打を開始し、保持時間に応じて間隔を 400ms→200ms→100ms(tick 格子の下限)
// へ縮める。Host API 変更なし、app_tick(100ms 周期)側の状態機械のみで完結。
const HOLD_INITIAL_DELAY_MS: u32 = 500;
const HOLD_ACCEL_1_MS: u32 = 1500; // これ未満は 400ms 間隔
const HOLD_ACCEL_2_MS: u32 = 3000; // これ未満は 200ms 間隔、以降は 100ms
const HOLD_INTERVAL_1_MS: u32 = 400;
const HOLD_INTERVAL_2_MS: u32 = 200;
const HOLD_INTERVAL_3_MS: u32 = 100;

static mut BPM: u32 = 120;
static mut SIG_IDX: usize = 2; // 4 拍子
static mut RUNNING: bool = false;
static mut ANCHOR: u32 = 0;    // 拍 0 の時刻(now_ms 時基)
static mut LAST_BEAT: u64 = u64::MAX; // 表示済みの拍番号
static mut LAMP_LIT: usize = usize::MAX; // 点灯中ランプ(消灯管理)
static mut VOLUME: i32 = 98; // ホスト既定(hostapi_audio_reset)と同値

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

/// 拍 n の時刻(ms)。拍ごとに除算するので累積丸め誤差が出ない
fn beat_time(n: u64) -> u32 {
    unsafe { ANCHOR.wrapping_add((n * 60000 / BPM as u64) as u32) }
}

/// 現在時刻が何拍目か(拍 0 起点)
fn beat_of(now: u32) -> u64 {
    unsafe {
        let elapsed = now.wrapping_sub(ANCHOR) as u64;
        elapsed * BPM as u64 / 60000
    }
}

/// now_ms は wraparound しうるので、差分を符号付きで見て到達判定する
fn time_reached(now: u32, target: u32) -> bool {
    (now.wrapping_sub(target) as i32) >= 0
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

/// Phase 7D: -1/+1(テンポ微調整)・V-/V+(音量)の新規行。ラベルは固定なので
/// app_init で一度描画すれば良い(押下による再描画は不要)
fn draw_fine_buttons() {
    for i in 0..4 {
        unsafe {
            hostapi_fill_rect(BTN_XS[i], FINE_Y, BTN_W, FINE_H, 0x18_50_70);
            hostapi_draw_text(BTN_XS[i] + 24, FINE_Y + 18, FINE_LABELS[i].as_ptr(),
                              FINE_LABELS[i].len() as u32);
        }
    }
}

/// 拍番号 n に応じたトーンで予約する(小節頭 = アクセント)
fn schedule_beat(n: u64) {
    unsafe {
        let slot = if n % (SIGS[SIG_IDX] as u64) == 0 { ACCENT_SLOT } else { 0 };
        hostapi_tone_schedule(slot, beat_time(n).max(1) as i32);
    }
}

/// 再アンカー(START、BPM/拍子変更時)。次の拍が period 後に来るよう now を拍 0 に
fn rearm(now: u32) {
    unsafe {
        ANCHOR = now;
        LAST_BEAT = u64::MAX;
        if RUNNING {
            schedule_beat(0); // 拍 0(小節頭)= 今すぐ
        }
    }
}

/// BPM を delta だけ変更(40-240 にクランプ)。変化の有無によらずステータス行を
/// 再描画する(連打加速中は毎回変わるとは限らないが、表示を最新に保つ)
fn apply_bpm_delta(delta: i32, now: u32) {
    unsafe {
        let new_bpm = (BPM as i32 + delta).clamp(BPM_MIN as i32, BPM_MAX as i32) as u32;
        if new_bpm != BPM {
            BPM = new_bpm;
            rearm(now);
        }
    }
    draw_status();
}

/// 音量を delta だけ変更(0-100 にクランプ)。マスター音量 API のみで実現
/// (7A でクリック/トーン出力にも適用済み。Host API 変更なし)
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

/// BPM ボタン押下開始: 即座に 1 ステップ適用し、長押し連打の状態を仕込む
fn start_repeat(delta: i32, now: u32) {
    apply_bpm_delta(delta, now);
    unsafe {
        HELD_DELTA = delta;
        HELD_SINCE = now;
        NEXT_REPEAT_AT = now.wrapping_add(HOLD_INITIAL_DELAY_MS);
    }
}

/// 保持中の BPM ボタンがあれば、保持時間に応じた間隔で自動連打する
fn process_repeat(now: u32) {
    unsafe {
        if HELD_DELTA == 0 || !time_reached(now, NEXT_REPEAT_AT) {
            return;
        }
        let delta = HELD_DELTA;
        apply_bpm_delta(delta, now);
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

fn handle_tap(x: i16, y: i16) {
    let (x, y) = (x as i32, y as i32);
    let now = unsafe { hostapi_now_ms() };

    // Phase 7D 新規行: -1 / +1 / V- / V+
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
                        rearm(now);
                        draw_lamps(usize::MAX);
                        draw_status();
                    }
                    3 => {
                        RUNNING = !RUNNING;
                        if RUNNING {
                            rearm(now);
                        } else {
                            hostapi_click_schedule(0); // キャンセル
                            draw_lamps(usize::MAX);
                        }
                        draw_run_button();
                        draw_status();
                    }
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
        LAST_BEAT = u64::MAX;
        LAMP_LIT = usize::MAX;
        VOLUME = 98;
        HELD_DELTA = 0;

        // 1 拍目のアクセント音(高いピッチ)。通常拍は slot 0 の既定クリック
        hostapi_tone_define(ACCENT_SLOT, 0 /*SINE*/, 1568, 30, 100);
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
        let beat = beat_of(now);

        // 次の拍を毎 tick 再予約(last_fired ガードで二重発音しない)。
        // 小節頭ならアクセント音のスロットで予約する
        schedule_beat(beat + 1);

        // 拍ランプの更新(視覚は tick 格子で十分)
        if beat != LAST_BEAT {
            LAST_BEAT = beat;
            let n_beats = SIGS[SIG_IDX] as u64;
            draw_lamps((beat % n_beats) as usize);
        }
    }
}
