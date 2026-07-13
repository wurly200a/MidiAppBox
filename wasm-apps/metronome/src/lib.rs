// Phase 7B: メトロノーム本体。hostapi_click_schedule(7A)の上に
// 可変 BPM(40-240)・拍子(2/3/4/6)・START/STOP・拍ランプを実装する。
// - 発音はホストの予約発音(±µs 級)。アプリは毎 tick「次の拍」を再予約するだけ。
// - 拍時刻は anchor + n*60000/bpm を拍ごとに計算(累積丸め誤差なし。
//   BPM 132 = 454.545ms のような割り切れない周期でもドリフトしない)。
// - 拍ランプは tick(100ms 格子)で更新される視覚表示。1 拍目はアクセント色。
//   音のアクセント(音色変更)は API v2 の相談事項として 7C へ。
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

// ---- レイアウト(320x240) ----
const LAMP_Y: i32 = 76;
const LAMP_H: i32 = 36;
const LAMP_W: i32 = 44;
const LAMP_GAP: i32 = 8;
const LAMP_X0: i32 = 12;
const MAX_BEATS: usize = 6;

const BTN_Y: i32 = 176;
const BTN_H: i32 = 52;
const BTN_W: i32 = 70;
const BTN_XS: [i32; 4] = [12, 90, 168, 246];
const BTN_LABELS: [&[u8]; 4] = [b"BPM-", b"BPM+", b"BEAT", b"START"];

const BPM_MIN: u32 = 40;
const BPM_MAX: u32 = 240;
const SIGS: [u32; 4] = [2, 3, 4, 6]; // 1 小節の拍数

static mut BPM: u32 = 120;
static mut SIG_IDX: usize = 2; // 4 拍子
static mut RUNNING: bool = false;
static mut ANCHOR: u32 = 0;    // 拍 0 の時刻(now_ms 時基)
static mut LAST_BEAT: u64 = u64::MAX; // 表示済みの拍番号
static mut LAMP_LIT: usize = usize::MAX; // 点灯中ランプ(消灯管理)

struct Line {
    buf: [u8; 32],
    len: usize,
}

impl Line {
    fn new() -> Line {
        Line { buf: [b' '; 32], len: 0 }
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

fn draw_status() {
    unsafe {
        let mut l = Line::new();
        l.push(b"BPM: ").push_u32(BPM).push(b"   beats/bar: ").push_u32(SIGS[SIG_IDX]);
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

/// 再アンカー(START、BPM/拍子変更時)。次の拍が period 後に来るよう now を拍 0 に
fn rearm(now: u32) {
    unsafe {
        ANCHOR = now;
        LAST_BEAT = u64::MAX;
        if RUNNING {
            hostapi_click_schedule(beat_time(0).max(1) as i32); // 拍 0 = 今すぐ
        }
    }
}

fn handle_tap(x: i16, y: i16) {
    let (x, y) = (x as i32, y as i32);
    if y < BTN_Y || y >= BTN_Y + BTN_H {
        return;
    }
    unsafe {
        let now = hostapi_now_ms();
        for i in 0..4 {
            if x >= BTN_XS[i] && x < BTN_XS[i] + BTN_W {
                match i {
                    0 => {
                        if BPM > BPM_MIN {
                            BPM -= 5;
                            rearm(now);
                        }
                    }
                    1 => {
                        if BPM < BPM_MAX {
                            BPM += 5;
                            rearm(now);
                        }
                    }
                    2 => {
                        SIG_IDX = (SIG_IDX + 1) % SIGS.len();
                        rearm(now);
                        draw_lamps(usize::MAX);
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
                    }
                    _ => {}
                }
                draw_status();
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
    }
    draw_status();
    draw_lamps(usize::MAX);
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
        }
    }

    unsafe {
        if !RUNNING {
            return;
        }
        let now = hostapi_now_ms();
        let beat = beat_of(now);

        // 次の拍を毎 tick 再予約(last_fired ガードで二重発音しない)
        hostapi_click_schedule(beat_time(beat + 1) as i32);

        // 拍ランプの更新(視覚は tick 格子で十分)
        if beat != LAST_BEAT {
            LAST_BEAT = beat;
            let n_beats = SIGS[SIG_IDX] as u64;
            draw_lamps((beat % n_beats) as usize);
        }
    }
}
