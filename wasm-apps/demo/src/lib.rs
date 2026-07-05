// Phase 2 デモ: 1 秒ごとにカウンタを描画してクリック音を鳴らす。
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
    fn hostapi_now_ms() -> u32;
}

// app_init/app_tick は同一スレッドから呼ばれる(ホスト側の契約)ので static mut で足りる
static mut START_MS: u32 = 0;
static mut LAST_SEC: u32 = u32::MAX;

/// 数値を 10 進文字列にして buf 末尾側から詰める。書いたスライスを返す。
fn format_u32(buf: &mut [u8], mut v: u32) -> &[u8] {
    let mut i = buf.len();
    loop {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    &buf[i..]
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        hostapi_fill_rect(0, 0, 240, 320, 0x10_18_28); // 背景
        hostapi_fill_rect(0, 0, 240, 40, 0x20_40_a0); // タイトルバー
        let title = b"WASM demo (Rust)";
        hostapi_draw_text(12, 12, title.as_ptr(), title.len() as u32);
        let hint = b"counter + click every 1s";
        hostapi_draw_text(12, 56, hint.as_ptr(), hint.len() as u32);

        START_MS = hostapi_now_ms();
        LAST_SEC = u32::MAX;
    }
    0
}

#[no_mangle]
pub extern "C" fn app_tick() {
    unsafe {
        let elapsed = hostapi_now_ms().wrapping_sub(START_MS);
        let sec = elapsed / 1000;
        if sec == LAST_SEC {
            return;
        }
        LAST_SEC = sec;

        // "count: N" を組み立てて描画(毎回同じ座標なのでホスト側はラベルを更新する)
        let mut line = [b' '; 20];
        let prefix = b"count: ";
        line[..prefix.len()].copy_from_slice(prefix);
        let mut num = [0u8; 10];
        let digits = format_u32(&mut num, sec);
        line[prefix.len()..prefix.len() + digits.len()].copy_from_slice(digits);
        hostapi_draw_text(12, 96, line.as_ptr(), (prefix.len() + digits.len()) as u32);

        hostapi_play_click();
    }
}
