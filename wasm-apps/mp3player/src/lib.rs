// Phase 6B 検証: hostapi_audio_* で固定パスの MP3 を制御する。
// (6C でファイル列挙+プレイリストに育てる予定の土台)
// - PLAY / PAUSE(トグル) / STOP / VOL- / VOL+ の 5 ボタン
// - 毎 tick get_state をポーリングして状態表示(FINISHED の検知を含む)
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
    fn hostapi_audio_play(path: *const u8, path_len: u32) -> i32;
    fn hostapi_audio_ctrl(cmd: i32) -> i32;
    fn hostapi_audio_set_volume(v: i32);
    fn hostapi_audio_get_state() -> i32;
}

/// shared/hostapi_defs.h と同一レイアウト(12 bytes, LE)
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
const EV_BUF_LEN: usize = 16;

const CMD_PAUSE: i32 = 1;
const CMD_RESUME: i32 = 2;
const CMD_STOP: i32 = 3;

const ST_STOPPED: i32 = 0;
const ST_PLAYING: i32 = 1;
const ST_PAUSED: i32 = 2;
const ST_FINISHED: i32 = 3;

const TRACK: &[u8] = b"test.mp3";

// ボタン行(320x240、y=150 に高さ 56 の 5 ボタン)
const BTN_Y: i32 = 150;
const BTN_H: i32 = 56;
const BTN_W: i32 = 56;
const BTN_XS: [i32; 5] = [12, 74, 136, 198, 260];
const BTN_LABELS: [&[u8]; 5] = [b"PLAY", b"PAUSE", b"STOP", b"V-", b"V+"];

static mut VOLUME: i32 = 98; // ホストの既定音量と一致させておく
static mut LAST_STATE: i32 = -1;
static mut LAST_RC: i32 = 0; // 直近の API 戻り値(エラー表示用)

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
    fn push_i32(&mut self, v: i32) -> &mut Line {
        if v < 0 {
            self.push(b"-");
        }
        let mut v = v.unsigned_abs();
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
        let n = digits.len() - i;
        let start = i;
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

fn state_name(st: i32) -> &'static [u8] {
    match st {
        ST_STOPPED => b"STOPPED ",
        ST_PLAYING => b"PLAYING ",
        ST_PAUSED => b"PAUSED  ",
        ST_FINISHED => b"FINISHED",
        _ => b"ERROR   ",
    }
}

fn draw_status(st: i32) {
    let mut line = Line::new();
    line.push(b"state: ").push(state_name(st));
    unsafe {
        line.push(b"  vol: ").push_i32(VOLUME);
        if LAST_RC != 0 {
            line.push(b"  rc: ").push_i32(LAST_RC);
        }
    }
    line.draw(12, 88);
}

fn draw_buttons() {
    for i in 0..5 {
        unsafe {
            hostapi_fill_rect(BTN_XS[i], BTN_Y, BTN_W, BTN_H, 0x20_40_a0);
            hostapi_draw_text(BTN_XS[i] + 10, BTN_Y + 22, BTN_LABELS[i].as_ptr(),
                              BTN_LABELS[i].len() as u32);
        }
    }
}

fn hit_button(x: i16, y: i16) -> i32 {
    let (x, y) = (x as i32, y as i32);
    if y < BTN_Y || y >= BTN_Y + BTN_H {
        return -1;
    }
    for i in 0..5 {
        if x >= BTN_XS[i] && x < BTN_XS[i] + BTN_W {
            return i as i32;
        }
    }
    -1
}

fn set_volume_clamped(delta: i32) {
    unsafe {
        VOLUME += delta;
        if VOLUME < 0 {
            VOLUME = 0;
        }
        if VOLUME > 100 {
            VOLUME = 100;
        }
        hostapi_audio_set_volume(VOLUME);
    }
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        hostapi_fill_rect(0, 0, 320, 40, 0x20_60_50); // タイトルバー
        hostapi_fill_rect(0, 40, 320, 200, 0x10_18_28); // 背景
        let title = b"MP3 player (wasm)";
        hostapi_draw_text(12, 12, title.as_ptr(), title.len() as u32);

        let mut track = Line::new();
        track.push(b"track: ").push(TRACK);
        track.draw(12, 60);

        VOLUME = 98;
        LAST_STATE = -1;
        LAST_RC = 0;
    }
    draw_buttons();
    draw_status(unsafe { hostapi_audio_get_state() });
    0
}

#[no_mangle]
pub extern "C" fn app_tick() {
    let mut evs = [Event { ev_type: 0, param: 0, x: 0, y: 0, time_ms: 0 }; EV_BUF_LEN];
    let n = unsafe {
        hostapi_poll_event(evs.as_mut_ptr() as *mut u8,
                           (EV_BUF_LEN * core::mem::size_of::<Event>()) as u32)
    };

    for ev in &evs[..n.max(0) as usize] {
        if ev.ev_type != EV_TOUCH_DOWN {
            continue;
        }
        let st = unsafe { hostapi_audio_get_state() };
        match hit_button(ev.x, ev.y) {
            0 => unsafe {
                LAST_RC = hostapi_audio_play(TRACK.as_ptr(), TRACK.len() as u32);
            },
            1 => unsafe {
                // PAUSE はトグル: PLAYING なら PAUSE、PAUSED なら RESUME
                LAST_RC = if st == ST_PLAYING {
                    hostapi_audio_ctrl(CMD_PAUSE)
                } else if st == ST_PAUSED {
                    hostapi_audio_ctrl(CMD_RESUME)
                } else {
                    -1
                };
            },
            2 => unsafe {
                LAST_RC = hostapi_audio_ctrl(CMD_STOP);
            },
            3 => set_volume_clamped(-10),
            4 => set_volume_clamped(10),
            _ => {}
        }
        unsafe { LAST_STATE = -1 }; // ボタン操作後は必ず表示を更新
    }

    // 状態表示(変化時のみ再描画)
    let st = unsafe { hostapi_audio_get_state() };
    unsafe {
        if st != LAST_STATE {
            draw_status(st);
            LAST_STATE = st;
        }
    }
}
