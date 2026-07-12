// Phase 6B/6C: MP3 プレーヤー本体。
// - hostapi_fs_list でミュージックルートの .mp3 を列挙しリスト表示(6 行+スクロール)
// - 行タップで選択・再生。PLAY / PAUSE(トグル) / STOP / VOL± ボタン
// - 毎 tick get_state をポーリングし、FINISHED で次曲へ(末尾なら停止)
// - エラー表示: 曲なし(リスト空)、再生失敗(state: ERROR)
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
    fn hostapi_fs_list(idx: i32, buf: *mut u8, buf_len: u32) -> i32;
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

const ST_PLAYING: i32 = 1;
const ST_PAUSED: i32 = 2;
const ST_FINISHED: i32 = 3;

// ---- レイアウト(320x240) ----
const LIST_Y0: i32 = 68;
const LIST_ROW_H: i32 = 22;
const LIST_ROWS: usize = 6;
const LIST_X: i32 = 12;
const LIST_W: i32 = 270;
const SCROLL_X: i32 = 290;
const SCROLL_W: i32 = 22;
const BTN_Y: i32 = 204;
const BTN_H: i32 = 32;
const BTN_W: i32 = 56;
const BTN_XS: [i32; 5] = [12, 74, 136, 198, 260];
const BTN_LABELS: [&[u8]; 5] = [b"PLAY", b"PAUS", b"STOP", b"V-", b"V+"];

const MAX_TRACKS: usize = 16;
const NAME_MAX: usize = 64;
const NAME_SHOWN: usize = 26; // 行に収まる目安で切り詰めて表示

static mut TRACKS: [[u8; NAME_MAX]; MAX_TRACKS] = [[0; NAME_MAX]; MAX_TRACKS];
static mut TRACK_LENS: [usize; MAX_TRACKS] = [0; MAX_TRACKS];
static mut TRACK_COUNT: usize = 0;
static mut SELECTED: usize = 0;
static mut OFFSET: usize = 0; // リスト先頭に表示する曲 index
static mut VOLUME: i32 = 98;
static mut LAST_STATE: i32 = -1;
static mut LAST_RC: i32 = 0;

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

fn state_name(st: i32) -> &'static [u8] {
    match st {
        0 => b"STOPPED ",
        ST_PLAYING => b"PLAYING ",
        ST_PAUSED => b"PAUSED  ",
        ST_FINISHED => b"FINISHED",
        _ => b"ERROR   ",
    }
}

fn scan_tracks() {
    unsafe {
        TRACK_COUNT = 0;
        for i in 0..MAX_TRACKS {
            let n = hostapi_fs_list(i as i32, TRACKS[i].as_mut_ptr(), NAME_MAX as u32);
            if n < 0 {
                break;
            }
            TRACK_LENS[i] = n as usize;
            TRACK_COUNT += 1;
        }
    }
}

fn draw_status() {
    let st = unsafe { hostapi_audio_get_state() };
    let mut line = Line::new();
    line.push(b"state: ").push(state_name(st));
    unsafe {
        line.push(b" vol: ").push_i32(VOLUME);
        if LAST_RC != 0 {
            line.push(b" rc: ").push_i32(LAST_RC);
        }
    }
    line.draw(12, 46);
}

fn draw_list() {
    unsafe {
        for row in 0..LIST_ROWS {
            let y = LIST_Y0 + (row as i32) * LIST_ROW_H;
            let idx = OFFSET + row;
            let (bg, is_track) = if idx < TRACK_COUNT {
                (if idx == SELECTED { 0x20_60_a0 } else { 0x2a_33_40 }, true)
            } else {
                (0x10_18_28, false)
            };
            hostapi_fill_rect(LIST_X, y, LIST_W, LIST_ROW_H - 2, bg);

            let mut line = Line::new();
            if is_track {
                let shown = if TRACK_LENS[idx] > NAME_SHOWN { NAME_SHOWN } else { TRACK_LENS[idx] };
                line.push(&TRACKS[idx][..shown]);
            } else if TRACK_COUNT == 0 && row == 0 {
                line.push(b"no mp3 files in music dir");
            } else {
                line.push(b" ");
            }
            line.draw(LIST_X + 6, y + 2);
        }
        // スクロールボタン(曲が収まる場合も描くが、押しても何も起きないだけ)
        hostapi_fill_rect(SCROLL_X, LIST_Y0, SCROLL_W, 62, 0x2a_33_40);
        hostapi_fill_rect(SCROLL_X, LIST_Y0 + 68, SCROLL_W, 62, 0x2a_33_40);
        hostapi_draw_text(SCROLL_X + 7, LIST_Y0 + 24, b"^".as_ptr(), 1);
        hostapi_draw_text(SCROLL_X + 7, LIST_Y0 + 90, b"v".as_ptr(), 1);
    }
}

fn draw_buttons() {
    for i in 0..5 {
        unsafe {
            hostapi_fill_rect(BTN_XS[i], BTN_Y, BTN_W, BTN_H, 0x20_40_a0);
            hostapi_draw_text(BTN_XS[i] + 8, BTN_Y + 8, BTN_LABELS[i].as_ptr(),
                              BTN_LABELS[i].len() as u32);
        }
    }
}

fn play_selected() {
    unsafe {
        if SELECTED < TRACK_COUNT {
            LAST_RC = hostapi_audio_play(TRACKS[SELECTED].as_ptr(),
                                         TRACK_LENS[SELECTED] as u32);
        }
    }
}

fn handle_tap(x: i16, y: i16) {
    let (x, y) = (x as i32, y as i32);
    unsafe {
        // リスト行
        if x >= LIST_X && x < LIST_X + LIST_W && y >= LIST_Y0
            && y < LIST_Y0 + (LIST_ROWS as i32) * LIST_ROW_H
        {
            let row = ((y - LIST_Y0) / LIST_ROW_H) as usize;
            let idx = OFFSET + row;
            if idx < TRACK_COUNT {
                SELECTED = idx;
                play_selected();
                draw_list();
            }
            return;
        }
        // スクロール
        if x >= SCROLL_X && x < SCROLL_X + SCROLL_W && y >= LIST_Y0 && y < LIST_Y0 + 130 {
            if y < LIST_Y0 + 65 {
                if OFFSET > 0 {
                    OFFSET -= 1;
                    draw_list();
                }
            } else if OFFSET + LIST_ROWS < TRACK_COUNT {
                OFFSET += 1;
                draw_list();
            }
            return;
        }
        // ボタン行
        if y >= BTN_Y && y < BTN_Y + BTN_H {
            let st = hostapi_audio_get_state();
            for i in 0..5 {
                if x >= BTN_XS[i] && x < BTN_XS[i] + BTN_W {
                    match i {
                        0 => play_selected(),
                        1 => {
                            LAST_RC = if st == ST_PLAYING {
                                hostapi_audio_ctrl(CMD_PAUSE)
                            } else if st == ST_PAUSED {
                                hostapi_audio_ctrl(CMD_RESUME)
                            } else {
                                -1
                            };
                        }
                        2 => LAST_RC = hostapi_audio_ctrl(CMD_STOP),
                        3 | 4 => {
                            VOLUME += if i == 3 { -10 } else { 10 };
                            if VOLUME < 0 {
                                VOLUME = 0;
                            }
                            if VOLUME > 100 {
                                VOLUME = 100;
                            }
                            hostapi_audio_set_volume(VOLUME);
                        }
                        _ => {}
                    }
                    break;
                }
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        hostapi_fill_rect(0, 0, 320, 40, 0x20_60_50); // タイトルバー
        hostapi_fill_rect(0, 40, 320, 200, 0x10_18_28); // 背景
        let title = b"MP3 player (wasm)";
        hostapi_draw_text(12, 12, title.as_ptr(), title.len() as u32);

        VOLUME = 98;
        SELECTED = 0;
        OFFSET = 0;
        LAST_STATE = -1;
        LAST_RC = 0;
    }
    scan_tracks();
    draw_list();
    draw_buttons();
    draw_status();
    0
}

#[no_mangle]
pub extern "C" fn app_tick() {
    let mut evs = [Event { ev_type: 0, param: 0, x: 0, y: 0, time_ms: 0 }; EV_BUF_LEN];
    let n = unsafe {
        hostapi_poll_event(evs.as_mut_ptr() as *mut u8,
                           (EV_BUF_LEN * core::mem::size_of::<Event>()) as u32)
    };
    let mut dirty = false;
    for ev in &evs[..n.max(0) as usize] {
        if ev.ev_type == EV_TOUCH_DOWN {
            handle_tap(ev.x, ev.y);
            dirty = true;
        }
    }

    // 自然終了 → 次曲へ(末尾なら停止)
    let st = unsafe { hostapi_audio_get_state() };
    unsafe {
        if st == ST_FINISHED {
            if SELECTED + 1 < TRACK_COUNT {
                SELECTED += 1;
                if SELECTED >= OFFSET + LIST_ROWS {
                    OFFSET = SELECTED - (LIST_ROWS - 1);
                }
                play_selected();
                draw_list();
            } else {
                LAST_RC = hostapi_audio_ctrl(CMD_STOP);
            }
            dirty = true;
        }
        if dirty || st != LAST_STATE {
            draw_status();
            LAST_STATE = hostapi_audio_get_state();
        }
    }
}
