// bars: イコライザ風のバーアニメーション。demo との見た目の差別化用。
//
// ホスト API v0 の描画モデルは (x,y) キーの retained オブジェクトなので、
// アニメーションは「座標固定・サイズと色を毎 tick 更新」で表現する。
// (x,y) を動かすと tick ごとに新スロットを消費するため、バーは上端固定で
// 下方向に伸縮させる。縮んだ領域は LVGL の再描画で背景に戻るので
// 消し込み用の矩形は不要。スロット消費: 背景 1 + バー 8 = 9 ≦ 16。
#![no_std]

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

extern "C" {
    fn hostapi_draw_text(x: i32, y: i32, ptr: *const u8, len: u32);
    fn hostapi_fill_rect(x: i32, y: i32, w: i32, h: i32, rgb888: u32);
    fn hostapi_play_click();
    fn hostapi_now_ms() -> u32;
}

// 画面はランドスケープ 320x240
const NUM_BARS: i32 = 8;
const BAR_W: i32 = 30;
const BAR_GAP: i32 = 8;
const BAR_X0: i32 = (320 - (NUM_BARS * BAR_W + (NUM_BARS - 1) * BAR_GAP)) / 2;
const BAR_TOP: i32 = 50; // バー上端(固定)
const BAR_MAX_H: i32 = 170;
const BG: u32 = 0x18_10_20;

static mut RNG: u32 = 0x1234_5678;
static mut PHASE: [u32; NUM_BARS as usize] = [0; NUM_BARS as usize];
static mut LAST_BEAT: u32 = 0;

fn rand_next() -> u32 {
    unsafe {
        RNG = RNG.wrapping_mul(1664525).wrapping_add(1013904223);
        RNG
    }
}

/// バーの色: 高さに応じて緑→黄→赤
fn bar_color(h: i32) -> u32 {
    if h > BAR_MAX_H * 3 / 4 {
        0xe0_40_30
    } else if h > BAR_MAX_H / 2 {
        0xe0_c0_30
    } else {
        0x30_c0_60
    }
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        hostapi_fill_rect(0, 0, 320, 240, BG); // 背景
        let title = b"BARS (wasm)";
        hostapi_draw_text(12, 12, title.as_ptr(), title.len() as u32);
        RNG ^= hostapi_now_ms();
        for i in 0..NUM_BARS as usize {
            PHASE[i] = rand_next() % 64;
        }
        LAST_BEAT = hostapi_now_ms() / 2000;
    }
    0
}

#[no_mangle]
pub extern "C" fn app_tick() {
    unsafe {
        let now = hostapi_now_ms();

        for i in 0..NUM_BARS {
            PHASE[i as usize] = PHASE[i as usize].wrapping_add(2);
            let phase = PHASE[i as usize];
            // 三角波 + ノイズで高さを揺らす
            let t = (phase % 64) as i32;
            let tri = if t < 32 { t } else { 63 - t };
            let noise = (rand_next() % 24) as i32;
            let mut h = 16 + tri * (BAR_MAX_H - 40) / 32 + noise;
            if h > BAR_MAX_H {
                h = BAR_MAX_H;
            }

            let x = BAR_X0 + i * (BAR_W + BAR_GAP);
            hostapi_fill_rect(x, BAR_TOP, BAR_W, h, bar_color(h));
        }

        // 2 秒ごとにクリック
        let beat = now / 2000;
        if beat != LAST_BEAT {
            LAST_BEAT = beat;
            hostapi_play_click();
        }
    }
}
