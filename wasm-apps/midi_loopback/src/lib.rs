// Phase 9b Stage 1: MIDI ループバック診断アプリ(配線チェック)。
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
    fn push_hex_u8(&mut self, v: u8) -> &mut Line {
        const HEX: &[u8; 16] = b"0123456789ABCDEF";
        self.push(&[HEX[(v >> 4) as usize], HEX[(v & 0x0f) as usize]])
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
    // (host が受信直後に打刻した µs 値)のみを使う想定だが、Stage 1 では
    // 生バイト表示のみで時刻は未使用(Stage 2/3 で使用)。
    let mut recs = [MidiRecv { timestamp_us: 0, byte: 0, _reserved: [0; 7] }; RECV_BUF_LEN];
    let n = unsafe {
        hostapi_midi_recv(recs.as_mut_ptr() as *mut u8,
                          (RECV_BUF_LEN * core::mem::size_of::<MidiRecv>()) as u32)
    };
    for rec in &recs[..n.max(0) as usize] {
        push_recent(rec.byte);
    }

    draw_counts();
    draw_hex();
}
