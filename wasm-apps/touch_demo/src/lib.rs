// Phase 6A 検証: hostapi_poll_event のタッチイベントを可視化する。
// - 直近イベントの種別・座標・時刻を表示
// - DOWN/UP の累計カウント表示(素早いタップで両方増えることの確認用)
// - ボタン領域のタップでクリック音+ボタン色変化
// ホスト API (module "env") のみ使用。no_std / アロケータ不要。
#![no_std]

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

extern "C" {
    fn hostapi_draw_text(x: i32, y: i32, ptr: *const u8, len: u32);
    fn hostapi_fill_rect(x: i32, y: i32, w: i32, h: i32, rgb888: u32);
    fn hostapi_play_click();
    fn hostapi_poll_event(buf: *mut u8, buf_len: u32) -> i32;
}

/// shared/hostapi_defs.h の hostapi_event_t と同一レイアウト(12 bytes, LE)
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
const EV_BUF_LEN: usize = 16;

// ボタン領域(論理画面 320x240)
const BTN_X: i32 = 200;
const BTN_Y: i32 = 160;
const BTN_W: i32 = 104;
const BTN_H: i32 = 60;

// app_init/app_tick は同一スレッドから呼ばれる(ホスト側の契約)
static mut DOWN_COUNT: u32 = 0;
static mut UP_COUNT: u32 = 0;

/// 固定長バッファへの逐次書き込み(no_std での簡易フォーマッタ)
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
        self.push(&digits[i..])
    }
    fn push_i32(&mut self, v: i32) -> &mut Line {
        if v < 0 {
            self.push(b"-").push_u32(v.unsigned_abs())
        } else {
            self.push_u32(v as u32)
        }
    }
    fn draw(&self, x: i32, y: i32) {
        unsafe { hostapi_draw_text(x, y, self.buf.as_ptr(), self.len as u32) };
    }
}

fn draw_button(pressed: bool) {
    unsafe {
        hostapi_fill_rect(BTN_X, BTN_Y, BTN_W, BTN_H,
                          if pressed { 0x30_a0_50 } else { 0x20_40_a0 });
        let label = b"CLICK";
        hostapi_draw_text(BTN_X + 32, BTN_Y + 22, label.as_ptr(), label.len() as u32);
    }
}

fn in_button(x: i16, y: i16) -> bool {
    let (x, y) = (x as i32, y as i32);
    x >= BTN_X && x < BTN_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H
}

fn draw_counters() {
    unsafe {
        let mut line = Line::new();
        line.push(b"down: ")
            .push_u32(DOWN_COUNT)
            .push(b"  up: ")
            .push_u32(UP_COUNT);
        line.draw(12, 120);
    }
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        // 背景とタイトルバーは (x,y) キーが重ならないように分割
        // (retained モデルでは同一キーの再描画は置き換えになるため)
        hostapi_fill_rect(0, 0, 320, 40, 0x60_30_80); // タイトルバー
        hostapi_fill_rect(0, 40, 320, 200, 0x10_18_28); // 背景
        let title = b"touch demo (Rust)";
        hostapi_draw_text(12, 12, title.as_ptr(), title.len() as u32);
        let hint = b"tap anywhere / tap CLICK for sound";
        hostapi_draw_text(12, 56, hint.as_ptr(), hint.len() as u32);

        DOWN_COUNT = 0;
        UP_COUNT = 0;
    }
    let mut line = Line::new();
    line.push(b"last: (none)");
    line.draw(12, 88);
    draw_counters();
    draw_button(false);
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
        let name: &[u8] = match ev.ev_type {
            EV_TOUCH_DOWN => b"DOWN",
            EV_TOUCH_UP => b"UP  ",
            _ => continue, // 未知の type は無視(契約)
        };
        unsafe {
            match ev.ev_type {
                EV_TOUCH_DOWN => DOWN_COUNT += 1,
                EV_TOUCH_UP => UP_COUNT += 1,
                _ => {}
            }
        }

        let mut line = Line::new();
        line.push(b"last: ")
            .push(name)
            .push(b" x=")
            .push_i32(ev.x as i32)
            .push(b" y=")
            .push_i32(ev.y as i32)
            .push(b" t=")
            .push_u32(ev.time_ms);
        line.draw(12, 88);
        draw_counters();

        if ev.ev_type == EV_TOUCH_DOWN && in_button(ev.x, ev.y) {
            unsafe { hostapi_play_click() };
            draw_button(true);
        } else if ev.ev_type == EV_TOUCH_UP {
            draw_button(false);
        }
    }
}
