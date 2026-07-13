// Phase 7A 検証: hostapi_click_schedule の予約発音でメトロノームを刻む。
// - BPM 120 固定(period=500ms)。毎 tick「次の拍」を再予約する
//   (last_fired ガードにより二重発音しない、が契約)。
// - タップで SCHED(予約発音)⇔ LEGACY(tick 内で play_click 直呼び)を切替。
//   両方式のジッタをホスト側統計で比較するための構成。
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
    fn hostapi_play_click();
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
const PERIOD_MS: u32 = 500; // BPM 120

static mut ANCHOR: u32 = 0;
static mut LAST_BEAT: u32 = u32::MAX; // 表示済み拍数(LEGACY の発音判定と共用)
static mut SCHED_MODE: bool = true;

fn draw_num(prefix: &[u8], v: u32, x: i32, y: i32) {
    let mut buf = [b' '; 32];
    let mut n = 0;
    for &b in prefix {
        buf[n] = b;
        n += 1;
    }
    let mut digits = [0u8; 10];
    let mut i = digits.len();
    let mut v = v;
    loop {
        i -= 1;
        digits[i] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    for k in i..digits.len() {
        buf[n] = digits[k];
        n += 1;
    }
    unsafe { hostapi_draw_text(x, y, buf.as_ptr(), n as u32) };
}

fn draw_mode() {
    let label: &[u8] = if unsafe { SCHED_MODE } {
        b"mode: SCHED  (tap to toggle)"
    } else {
        b"mode: LEGACY (tap to toggle)"
    };
    unsafe { hostapi_draw_text(12, 120, label.as_ptr(), label.len() as u32) };
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        hostapi_fill_rect(0, 0, 320, 40, 0x80_40_20); // タイトルバー
        hostapi_fill_rect(0, 40, 320, 200, 0x10_18_28); // 背景
        let title = b"click test (BPM 120)";
        hostapi_draw_text(12, 12, title.as_ptr(), title.len() as u32);

        ANCHOR = hostapi_now_ms();
        LAST_BEAT = 0;
        SCHED_MODE = true;
    }
    draw_num(b"BPM: ", 120, 12, 56);
    draw_num(b"beats: ", 0, 12, 88);
    draw_mode();
    0
}

#[no_mangle]
pub extern "C" fn app_tick() {
    // タップでモード切替
    let mut evs = [Event { ev_type: 0, param: 0, x: 0, y: 0, time_ms: 0 }; 8];
    let n = unsafe {
        hostapi_poll_event(evs.as_mut_ptr() as *mut u8,
                           (8 * core::mem::size_of::<Event>()) as u32)
    };
    for ev in &evs[..n.max(0) as usize] {
        if ev.ev_type == EV_TOUCH_DOWN {
            unsafe {
                SCHED_MODE = !SCHED_MODE;
                if !SCHED_MODE {
                    hostapi_click_schedule(0); // LEGACY へ: 予約をキャンセル
                }
            }
            draw_mode();
        }
    }

    unsafe {
        let now = hostapi_now_ms();
        let elapsed = now.wrapping_sub(ANCHOR);
        let beat = elapsed / PERIOD_MS;

        if SCHED_MODE {
            // 次の拍を毎 tick 再予約(同一時刻の再予約は last_fired ガードで無視される)
            let next = ANCHOR.wrapping_add((beat + 1) * PERIOD_MS);
            hostapi_click_schedule(next as i32);
        } else if beat != LAST_BEAT {
            // 従来方式: tick 格子上で拍を跨いだら即時発音(比較計測用)
            hostapi_play_click();
        }

        if beat != LAST_BEAT {
            LAST_BEAT = beat;
            draw_num(b"beats: ", beat, 12, 88);
        }
    }
}
