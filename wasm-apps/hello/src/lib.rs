// Phase 1 最小 wasm: ホスト API なし、整数を返すのみ。
#![no_std]

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    42
}
