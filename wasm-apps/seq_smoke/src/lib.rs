// Phase 11 ステップ 2 の検証用アプリ。新しい音楽時間軸 API(12 関数)を
// 実機と Linux ホストで同一の .wasm から叩き、下記を確認する:
//   - transport_start で 0xFA と 24ppqn クロックが DIN_OUT に出る
//   - seq_write した DIN_OUT の Note On/Off が tick どおりに出る
//   - seq_write した CLICK イベントが鳴る
//   - PLAYING 中の tempomap_set_tempo でキュー積み直しなしにテンポが変わる
//   - transport_stop で 0xFC が出てクロックが止まる
//
// L2 の供給ループは architecture.md §11-9 のプレフィックス受理契約どおりに
// 実装する(受理されなかった残りを保持して次 tick で再送する)。
// アプリは実時間を一切扱わない(tick のみ)。
#![no_std]

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

extern "C" {
    fn hostapi_draw_text(x: i32, y: i32, ptr: *const u8, len: u32);
    fn hostapi_fill_rect(x: i32, y: i32, w: i32, h: i32, rgb888: u32);
    fn hostapi_poll_event(buf: *mut u8, buf_len: u32) -> i32;
    fn hostapi_tone_define(slot: i32, wave: i32, freq_hz: i32, dur_ms: i32, level: i32) -> i32;

    fn hostapi_transport_start() -> i32;
    fn hostapi_transport_stop() -> i32;
    fn hostapi_transport_get_position(buf: *mut u8, buf_len: u32) -> i32;
    fn hostapi_tempomap_set_tempo(at_tick: i32, us_per_quarter: i32) -> i32;
    fn hostapi_tempomap_set_meter(at_tick: i32, numer: i32, denom: i32) -> i32;
    fn hostapi_seq_write(buf: *const u8, buf_len: u32) -> i32;
    fn hostapi_seq_filled_until() -> i32;
    fn hostapi_midi_recv(buf: *mut u8, buf_len: u32) -> i32;
}

const PPQN: u32 = 960;
const BEAT: u32 = PPQN;
const BAR: u32 = PPQN * 4; // 4/4
const HORIZON: u32 = BAR * 2; // 2 小節先まで供給する

const PORT_DIN_OUT: u8 = 0;
const PORT_CLICK: u8 = 3;
const OP_TONE: u8 = 1;

const ACCENT_SLOT: u32 = 1;

const TEMPO_120: i32 = 500000;
const TEMPO_180: i32 = 333333;
// この song tick(4 小節目の頭)でテンポを 180 に切り替える
const TEMPO_SWITCH_TICK: u32 = BAR * 4;

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

#[repr(C)]
#[derive(Clone, Copy)]
struct SeqEvent {
    tick: u32,
    port: u8,
    status: u8,
    data1: u8,
    data2: u8,
    param: u32,
    reserved: u32,
}

impl SeqEvent {
    const fn zero() -> SeqEvent {
        SeqEvent { tick: 0, port: 0, status: 0, data1: 0, data2: 0, param: 0, reserved: 0 }
    }
}

// L2 の未受理分(プレフィックス受理契約)。1 拍ぶん = クリック + Note On/Off
const CHUNK_MAX: usize = 8;
static mut PENDING: [SeqEvent; CHUNK_MAX] = [SeqEvent::zero(); CHUNK_MAX];
static mut PENDING_LEN: usize = 0;
static mut PENDING_OFF: usize = 0;

static mut RUNNING: bool = false;
static mut NEXT_BEAT: u32 = 0; // 次に供給する拍(playback tick / BEAT)
static mut TEMPO_SWITCHED: bool = false;
static mut ACCEPTED: u32 = 0; // 受理できたイベント総数(表示用)
static mut REJECTED: u32 = 0; // 満杯で持ち越した回数(表示用)

// 自機 MIDI OUT → MIDI IN のループバック受信で送出を検証する(実機用)。
// midi_loopback の E1 と同じ考え方だが、ここは合否判定に足る最小限だけを持つ。
static mut RX_CLOCK: u32 = 0;
static mut RX_START: u32 = 0;
static mut RX_STOP: u32 = 0;
static mut RX_NOTE_ON: u32 = 0;
static mut RX_NOTE_OFF: u32 = 0;
static mut RX_PREV_US: u64 = 0;
static mut RX_MIN: u32 = u32::MAX;
static mut RX_MAX: u32 = 0;
static mut RX_SUM: u64 = 0;
static mut RX_N: u32 = 0;
static mut RX_RUNSTAT: u8 = 0;

#[repr(C)]
#[derive(Clone, Copy)]
struct RecvRec {
    timestamp_us: u64,
    byte: u8,
    _reserved: [u8; 7],
}

/// 受信バイトを最小限だけ解釈する。System Realtime(0xF8/0xFA/0xFC)は
/// ランニングステータスを壊さないので別扱いにする。
fn drain_rx() {
    unsafe {
        let mut recs = [RecvRec { timestamp_us: 0, byte: 0, _reserved: [0; 7] }; 16];
        loop {
            let n = hostapi_midi_recv(recs.as_mut_ptr() as *mut u8,
                                      (16 * core::mem::size_of::<RecvRec>()) as u32);
            if n <= 0 {
                return;
            }
            for r in &recs[..n as usize] {
                let b = r.byte;
                if b >= 0xF8 {
                    match b {
                        0xF8 => {
                            RX_CLOCK += 1;
                            if RX_PREV_US != 0 {
                                let d = (r.timestamp_us - RX_PREV_US) as u32;
                                if d < RX_MIN { RX_MIN = d; }
                                if d > RX_MAX { RX_MAX = d; }
                                RX_SUM += d as u64;
                                RX_N += 1;
                            }
                            RX_PREV_US = r.timestamp_us;
                        }
                        0xFA => RX_START += 1,
                        0xFC => RX_STOP += 1,
                        _ => {}
                    }
                } else if b >= 0x80 {
                    RX_RUNSTAT = b;
                    if b & 0xF0 == 0x90 { RX_NOTE_ON += 1; }
                    if b & 0xF0 == 0x80 { RX_NOTE_OFF += 1; }
                }
            }
            if (n as usize) < 16 {
                return;
            }
        }
    }
}

const BTN_Y: i32 = 176;
const BTN_H: i32 = 52;
const BTN_W: i32 = 100;
const BTN_X0: i32 = 20;
const BTN_X1: i32 = 180;

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
        for k in i..digits.len() {
            if self.len < self.buf.len() {
                self.buf[self.len] = digits[k];
                self.len += 1;
            }
        }
        self
    }
    fn draw(&self, x: i32, y: i32) {
        unsafe { hostapi_draw_text(x, y, self.buf.as_ptr(), self.len as u32) };
    }
}

/// 拍 n(playback tick 基準)の 1 拍ぶんのイベントを組み立てる。
/// クリック(小節頭はアクセント)+ DIN_OUT の Note On/Off(8 分音符長)。
fn build_beat(n: u32, out: &mut [SeqEvent; CHUNK_MAX]) -> usize {
    let tick = n * BEAT;
    let in_bar = n % 4;
    let mut k = 0;

    out[k] = SeqEvent::zero();
    out[k].tick = tick;
    out[k].port = PORT_CLICK;
    out[k].status = OP_TONE;
    out[k].param = if in_bar == 0 { ACCENT_SLOT } else { 0 };
    k += 1;

    // Note On(ch1)。小節頭は C4、それ以外は G4
    let note: u8 = if in_bar == 0 { 60 } else { 67 };
    out[k] = SeqEvent::zero();
    out[k].tick = tick;
    out[k].port = PORT_DIN_OUT;
    out[k].status = 0x90;
    out[k].data1 = note;
    out[k].data2 = 100;
    k += 1;

    // Note Off は 8 分音符後(対で必ず同じチャンクに入れる)
    out[k] = SeqEvent::zero();
    out[k].tick = tick + BEAT / 2;
    out[k].port = PORT_DIN_OUT;
    out[k].status = 0x80;
    out[k].data1 = note;
    out[k].data2 = 0;
    k += 1;

    k
}

fn drop_pending() {
    unsafe {
        PENDING_LEN = 0;
        PENDING_OFF = 0;
    }
}

/// L2 の供給ループ(docs/hostapi-next.md §10)。
/// プレフィックス受理なので、受理されなかった残りは PENDING に持ち越す。
fn supply(now_tick: u32) {
    unsafe {
        loop {
            if PENDING_OFF == PENDING_LEN {
                if hostapi_seq_filled_until() >= (now_tick + HORIZON) as i32 {
                    return;
                }
                PENDING_LEN = build_beat(NEXT_BEAT, &mut PENDING);
                PENDING_OFF = 0;
                NEXT_BEAT += 1;
                if PENDING_LEN == 0 {
                    return;
                }
            }
            let remain = PENDING_LEN - PENDING_OFF;
            let ptr = PENDING.as_ptr().add(PENDING_OFF) as *const u8;
            let n = hostapi_seq_write(ptr, (remain * 16) as u32);
            if n < 0 {
                return;
            }
            PENDING_OFF += n as usize;
            ACCEPTED += n as u32;
            if (n as usize) < remain {
                REJECTED += 1; // キュー満杯。次の tick で残りを再送する
                return;
            }
        }
    }
}

/// ループバック受信の集計を表示する(実機の合否判定用)
fn draw_rx() {
    unsafe {
        let mut l = Line::new();
        l.push(b"rx clk ").push_u32(RX_CLOCK).push(b" FA").push_u32(RX_START)
         .push(b" FC").push_u32(RX_STOP)
         .push(b" on").push_u32(RX_NOTE_ON).push(b" off").push_u32(RX_NOTE_OFF);
        l.draw(12, 132);
        let mut l2 = Line::new();
        let avg = if RX_N > 0 { (RX_SUM / RX_N as u64) as u32 } else { 0 };
        l2.push(b"int ").push_u32(if RX_MIN == u32::MAX { 0 } else { RX_MIN })
          .push(b"/").push_u32(avg).push(b"/").push_u32(RX_MAX).push(b" us");
        l2.draw(12, 152);
    }
}

fn draw_buttons() {
    unsafe {
        let running = RUNNING;
        let (label, color): (&[u8], u32) =
            if running { (b"STOP ", 0xa0_30_30) } else { (b"START", 0x20_80_40) };
        hostapi_fill_rect(BTN_X0, BTN_Y, BTN_W, BTN_H, color);
        hostapi_draw_text(BTN_X0 + 24, BTN_Y + 16, label.as_ptr(), label.len() as u32);
        let l2 = b"TEMPO";
        hostapi_fill_rect(BTN_X1, BTN_Y, BTN_W, BTN_H, 0x20_40_a0);
        hostapi_draw_text(BTN_X1 + 24, BTN_Y + 16, l2.as_ptr(), l2.len() as u32);
    }
}

fn start() {
    unsafe {
        hostapi_tempomap_set_tempo(0, TEMPO_120);
        hostapi_tempomap_set_meter(0, 4, 4);
        NEXT_BEAT = 0;
        TEMPO_SWITCHED = false;
        ACCEPTED = 0;
        REJECTED = 0;
        RX_CLOCK = 0; RX_START = 0; RX_STOP = 0;
        RX_NOTE_ON = 0; RX_NOTE_OFF = 0;
        RX_PREV_US = 0; RX_MIN = u32::MAX; RX_MAX = 0; RX_SUM = 0; RX_N = 0;
        drop_pending();
        if hostapi_transport_start() == 0 {
            RUNNING = true;
        }
    }
    draw_buttons();
}

fn stop() {
    unsafe {
        hostapi_transport_stop();
        RUNNING = false;
        drop_pending(); // 未発火イベントが破棄されるので残りも捨てる(§5 の契約)
    }
    draw_buttons();
}

fn handle_tap(x: i16, y: i16) {
    let (x, y) = (x as i32, y as i32);
    if y < BTN_Y || y >= BTN_Y + BTN_H {
        return;
    }
    if x >= BTN_X0 && x < BTN_X0 + BTN_W {
        unsafe {
            if RUNNING {
                stop();
            } else {
                start();
            }
        }
    } else if x >= BTN_X1 && x < BTN_X1 + BTN_W {
        // 手動でも次の小節頭にテンポ 180 を投入できるようにしておく
        unsafe {
            let mut pos = [0u8; 32];
            if hostapi_transport_get_position(pos.as_mut_ptr(), 32) == 0 {
                let song = u32::from_le_bytes([pos[12], pos[13], pos[14], pos[15]]);
                let at = ((song / BAR) + 1) * BAR;
                hostapi_tempomap_set_tempo(at as i32, TEMPO_180);
                TEMPO_SWITCHED = true;
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        hostapi_fill_rect(0, 0, 320, 40, 0x30_50_90);
        hostapi_fill_rect(0, 40, 320, 200, 0x10_18_28);
        let title = b"seq_smoke (phase 11)";
        hostapi_draw_text(12, 12, title.as_ptr(), title.len() as u32);
        hostapi_tone_define(ACCENT_SLOT as i32, 0 /*SINE*/, 1568, 30, 100);
        RUNNING = false;
        NEXT_BEAT = 0;
        TEMPO_SWITCHED = false;
        ACCEPTED = 0;
        REJECTED = 0;
        drop_pending();
    }
    draw_buttons();
    // タップなしで一巡できるよう自動開始する(Linux ホストのクリック自動化は
    // 信頼できないため。docs/lessons.md)。8 小節で自動停止する。
    start();
    0
}

#[no_mangle]
pub extern "C" fn app_tick() {
    drain_rx();
    draw_rx();

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
        let mut pos = [0u8; 32];
        if hostapi_transport_get_position(pos.as_mut_ptr(), 32) != 0 {
            return;
        }
        let now_tick = u32::from_le_bytes([pos[8], pos[9], pos[10], pos[11]]);
        let song_tick = u32::from_le_bytes([pos[12], pos[13], pos[14], pos[15]]);
        let bar = u32::from_le_bytes([pos[16], pos[17], pos[18], pos[19]]);
        let upq = u32::from_le_bytes([pos[20], pos[21], pos[22], pos[23]]);
        let beat = u16::from_le_bytes([pos[24], pos[25]]);

        // 8 小節で自動停止(transport_stop → 0xFC、クロック停止の確認)
        if song_tick >= BAR * 8 {
            stop();
            let done = b"STOPPED (8 bars done)";
            hostapi_draw_text(12, 132, done.as_ptr(), done.len() as u32);
            return;
        }

        // 4 小節目の頭でテンポを 180 へ(PLAYING 中の投入。積み直しは不要)
        if !TEMPO_SWITCHED && song_tick + BAR > TEMPO_SWITCH_TICK {
            hostapi_tempomap_set_tempo(TEMPO_SWITCH_TICK as i32, TEMPO_180);
            TEMPO_SWITCHED = true;
        }

        supply(now_tick);

        let mut l = Line::new();
        l.push(b"bar ").push_u32(bar + 1).push(b" beat ").push_u32(beat as u32 + 1)
         .push(b"  upq ").push_u32(upq);
        l.draw(12, 60);
        let mut l2 = Line::new();
        l2.push(b"tick ").push_u32(now_tick).push(b" filled ")
          .push_u32(hostapi_seq_filled_until().max(0) as u32);
        l2.draw(12, 84);
        let mut l3 = Line::new();
        l3.push(b"written ").push_u32(ACCEPTED).push(b"  carried ").push_u32(REJECTED);
        l3.draw(12, 108);
    }
}
