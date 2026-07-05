// Phase 4 計測用: ホスト API 呼び出しコストと interpreter ループ速度の測定。
// ホスト側が esp_cpu_get_cycle_count() で外側から時間を測る。
#![no_std]

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

extern "C" {
    fn hostapi_now_ms() -> u32;
}

/// 純 wasm ループ(ホスト呼び出しなし)。LCG 形式の更新にして
/// LLVM による閉形式化(等差数列の和への畳み込み)を防ぐ。
#[no_mangle]
pub extern "C" fn bench_empty(n: u32) -> u32 {
    let mut acc: u32 = 1;
    let mut i: u32 = 0;
    while i < n {
        acc = acc.wrapping_mul(1664525).wrapping_add(i);
        i += 1;
    }
    acc
}

/// 同じループ構造で hostapi_now_ms() を n 回呼ぶ。
/// (bench_hostcall(n) - bench_empty(n)) / n ≒ ホスト API 1 回のコスト。
#[no_mangle]
pub extern "C" fn bench_hostcall(n: u32) -> u32 {
    let mut acc: u32 = 0;
    let mut i: u32 = 0;
    while i < n {
        acc = acc.wrapping_add(unsafe { hostapi_now_ms() });
        i += 1;
    }
    acc
}
