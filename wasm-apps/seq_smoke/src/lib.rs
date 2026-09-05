// Phase 11 の検証用アプリ。音楽時間軸 API(12 関数)を実機と Linux ホストで
// 同一の .wasm から叩き、自動で一巡して合否を自己判定する。
//
// タップなしで走る(Linux ホストの UI クリック自動化は信頼できないため。
// docs/lessons.md)。起動と同時に下記のステージを順に実行し、各チェックの
// 結果を画面に 'o'(合格)/'-'(未達)で表示する。
//
//   stage 0: transport_start → 0xFA + 24ppqn クロック + seq_write(CLICK / DIN_OUT)
//   stage 1: PLAYING 中の tempomap_set_tempo(120 → 180、キュー積み直しなし)
//   stage 2: tempomap_set_loop(song tick が巻き戻り、playback tick は単調増加)
//   stage 3: transport_locate(song が移動、playback tick は戻らない)
//   stage 4: transport_stop → 0xFC
//   stage 5: STOPPED 中の time_us_to_tick が -1
//   stage 6: transport_continue → 0xFB、位置が停止点から継続
//   常時   : time_us_to_tick(get_position の host_us) ≒ get_position の tick
//
// time_us_to_tick の検証に get_position の host_us を使うのが要点で、これなら
// MIDI IN の受信に依存せず両ホストで同じ判定ができる。
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
    fn hostapi_transport_continue() -> i32;
    fn hostapi_transport_locate(song_tick: i32) -> i32;
    fn hostapi_transport_get_position(buf: *mut u8, buf_len: u32) -> i32;
    fn hostapi_tempomap_set_tempo(at_tick: i32, us_per_quarter: i32) -> i32;
    fn hostapi_tempomap_set_meter(at_tick: i32, numer: i32, denom: i32) -> i32;
    fn hostapi_tempomap_set_loop(start_tick: i32, end_tick: i32) -> i32;
    fn hostapi_seq_write(buf: *const u8, buf_len: u32) -> i32;
    fn hostapi_seq_flush_after(tick: i32) -> i32;
    fn hostapi_seq_filled_until() -> i32;
    fn hostapi_time_us_to_tick(us: i64) -> i32;
    fn hostapi_midi_recv(buf: *mut u8, buf_len: u32) -> i32;
    fn hostapi_midi_send(bytes: *const u8, len: u32) -> i32;
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

const LOCATE_TARGET: u32 = BAR * 20;

// ---- 自己判定フラグ ----
const CHK_TEMPO: u16 = 1 << 0;
const CHK_LOOP: u16 = 1 << 1;
const CHK_LOCATE: u16 = 1 << 2;
const CHK_STOP: u16 = 1 << 3;
const CHK_U2T_STOPPED: u16 = 1 << 4;
const CHK_CONT: u16 = 1 << 5;
const CHK_U2T: u16 = 1 << 6;
const CHK_FLUSH: u16 = 1 << 7;
const CHK_ALL: u16 = 0xFF;

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
static mut ACCEPTED: u32 = 0;
static mut REJECTED: u32 = 0;

static mut STAGE: u8 = 0;
static mut CHK: u16 = 0;
static mut PREV_SONG: u32 = 0;
static mut PREV_PB: u32 = 0;
static mut WRAPS: u32 = 0;
static mut PB_MARK: u32 = 0;
static mut SONG_AT_STOP: u32 = 0;
static mut PB_AT_STOP: u32 = 0;
static mut LAST_HOST_US: u64 = 0;

// 自機 MIDI OUT → MIDI IN のループバック受信(実機での送出確認。任意)
static mut RX_CLOCK: u32 = 0;
static mut RX_START: u32 = 0;
static mut RX_CONT: u32 = 0;
static mut RX_STOP: u32 = 0;
static mut RX_NOTE_ON: u32 = 0;
static mut RX_NOTE_OFF: u32 = 0;

#[repr(C)]
#[derive(Clone, Copy)]
struct RecvRec {
    timestamp_us: u64,
    byte: u8,
    _reserved: [u8; 7],
}

/// 受信バイトを最小限だけ解釈する。System Realtime は他メッセージの途中に
/// 割り込みうるので別扱いにする。
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
                match r.byte {
                    0xF8 => RX_CLOCK += 1,
                    0xFA => RX_START += 1,
                    0xFB => RX_CONT += 1,
                    0xFC => RX_STOP += 1,
                    b if b & 0xF0 == 0x90 => RX_NOTE_ON += 1,
                    b if b & 0xF0 == 0x80 => RX_NOTE_OFF += 1,
                    _ => {}
                }
            }
            if (n as usize) < 16 {
                return;
            }
        }
    }
}

const BTN_Y: i32 = 198;
const BTN_H: i32 = 38;
const BTN_W: i32 = 110;
const BTN_X0: i32 = 20;

struct Line {
    buf: [u8; 44],
    len: usize,
}

impl Line {
    fn new() -> Line {
        Line { buf: [b' '; 44], len: 0 }
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

/// 拍 n(playback tick 基準)の 1 拍ぶん。クリック(小節頭はアクセント)+
/// DIN_OUT の Note On/Off(8 分音符長。対は必ず同じチャンクに入れる)。
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

    let note: u8 = if in_bar == 0 { 60 } else { 67 };
    out[k] = SeqEvent::zero();
    out[k].tick = tick;
    out[k].port = PORT_DIN_OUT;
    out[k].status = 0x90;
    out[k].data1 = note;
    out[k].data2 = 100;
    k += 1;

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

/// キューを捨てる操作(locate / flush_after / stop)の後に呼ぶ。未受理分を
/// 破棄し、seq_filled_until() から供給を再開する(§5 の契約)。
fn resync_after_discard() {
    unsafe {
        drop_pending();
        let filled = hostapi_seq_filled_until().max(0) as u32;
        NEXT_BEAT = filled / BEAT + 1;
    }
}

/// L2 の供給ループ(docs/hostapi.md §10)。プレフィックス受理なので、
/// 受理されなかった残りは PENDING に持ち越して次 tick で再送する。
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

fn draw_button() {
    unsafe {
        let (label, color): (&[u8], u32) = if RUNNING {
            (b"RUNNING", 0xa0_30_30)
        } else if STAGE >= 7 {
            (b"DONE   ", 0x20_80_40)
        } else {
            (b"IDLE   ", 0x20_40_a0)
        };
        hostapi_fill_rect(BTN_X0, BTN_Y, BTN_W, BTN_H, color);
        hostapi_draw_text(BTN_X0 + 16, BTN_Y + 14, label.as_ptr(), label.len() as u32);
    }
}

/// 判定結果(8 項目)を o / - で表示する
fn draw_checks() {
    unsafe {
        let row1: [(&[u8], u16); 4] = [
            (b"tmp", CHK_TEMPO),
            (b"lop", CHK_LOOP),
            (b"loc", CHK_LOCATE),
            (b"stp", CHK_STOP),
        ];
        let row2: [(&[u8], u16); 4] = [
            (b"u2s", CHK_U2T_STOPPED),
            (b"con", CHK_CONT),
            (b"u2t", CHK_U2T),
            (b"flu", CHK_FLUSH),
        ];
        let mut l = Line::new();
        for (n, bit) in row1.iter() {
            l.push(n).push(if CHK & bit != 0 { b"=o " } else { b"=- " });
        }
        l.draw(12, 104);
        let mut l2 = Line::new();
        for (n, bit) in row2.iter() {
            l2.push(n).push(if CHK & bit != 0 { b"=o " } else { b"=- " });
        }
        l2.draw(12, 124);
        let mut l3 = Line::new();
        l3.push(if CHK == CHK_ALL { b"PASS chk " } else { b"---- chk " })
          .push_u32(CHK as u32).push(b" st").push_u32(STAGE as u32);
        l3.draw(12, 144);
    }
}

fn draw_rx() {
    unsafe {
        let mut l = Line::new();
        l.push(b"rx clk").push_u32(RX_CLOCK).push(b" FA").push_u32(RX_START)
         .push(b" FB").push_u32(RX_CONT).push(b" FC").push_u32(RX_STOP);
        l.draw(12, 160);
        let mut l2 = Line::new();
        l2.push(b"rx on").push_u32(RX_NOTE_ON).push(b" off").push_u32(RX_NOTE_OFF)
          .push(b"  wr").push_u32(ACCEPTED).push(b" carry").push_u32(REJECTED);
        l2.draw(12, 176);
    }
}

fn start() {
    unsafe {
        hostapi_tempomap_set_tempo(0, TEMPO_120);
        hostapi_tempomap_set_meter(0, 4, 4);
        hostapi_tempomap_set_loop(0, 0); // ループ解除から始める
        NEXT_BEAT = 0;
        ACCEPTED = 0;
        REJECTED = 0;
        STAGE = 0;
        CHK = 0;
        WRAPS = 0;
        PREV_SONG = 0;
        PREV_PB = 0;
        RX_CLOCK = 0; RX_START = 0; RX_CONT = 0; RX_STOP = 0;
        RX_NOTE_ON = 0; RX_NOTE_OFF = 0;
        drop_pending();
        if hostapi_transport_start() == 0 {
            RUNNING = true;
        }
    }
    draw_button();
}

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        hostapi_fill_rect(0, 0, 320, 36, 0x30_50_90);
        hostapi_fill_rect(0, 36, 320, 204, 0x10_18_28);
        let title = b"seq_smoke (phase 11)";
        hostapi_draw_text(12, 10, title.as_ptr(), title.len() as u32);
        hostapi_tone_define(ACCENT_SLOT as i32, 0 /*SINE*/, 1568, 30, 100);
        RUNNING = false;
        STAGE = 0;
        CHK = 0;
    }
    draw_checks();
    // タップなしで一巡できるよう自動開始する
    start();
    0
}

/// PLAYING 中のステージ進行。now_tick = playback tick、song = song tick。
fn advance_playing(now_tick: u32, song: u32, upq: u32) {
    unsafe {
        match STAGE {
            // 2 小節走らせてから、次の小節頭にテンポ 180 を投入する
            0 => {
                if song >= BAR * 2 {
                    let at = (song / BAR + 1) * BAR;
                    if hostapi_tempomap_set_tempo(at as i32, TEMPO_180) == 0 {
                        PB_MARK = at;
                        STAGE = 1;
                    }
                }
            }
            // テンポが実際に切り替わったらループ範囲を設定する
            1 => {
                if upq == TEMPO_180 as u32 {
                    CHK |= CHK_TEMPO;
                    let ls = (song / BAR + 1) * BAR;
                    if hostapi_tempomap_set_loop(ls as i32, (ls + BAR) as i32) == 0 {
                        PREV_SONG = song;
                        PREV_PB = now_tick;
                        WRAPS = 0;
                        STAGE = 2;
                    }
                }
            }
            // song tick が巻き戻り、playback tick は単調増加であること
            2 => {
                if song < PREV_SONG && now_tick > PREV_PB {
                    WRAPS += 1;
                }
                PREV_SONG = song;
                PREV_PB = now_tick;
                if WRAPS >= 2 {
                    CHK |= CHK_LOOP;
                    hostapi_tempomap_set_loop(0, 0);
                    // seq_flush_after: 先読み済みの未発火分が実際に減ることを確認。
                    // filled_until を「前後で減ったか」で見る(キューが空になると
                    // 現在 playback tick が返り、その値は時々刻々進むため)
                    let filled_before = hostapi_seq_filled_until();
                    let removed = hostapi_seq_flush_after(now_tick as i32);
                    if removed > 0 && hostapi_seq_filled_until() < filled_before {
                        CHK |= CHK_FLUSH;
                    }
                    hostapi_transport_locate(LOCATE_TARGET as i32);
                    resync_after_discard();
                    PB_MARK = now_tick;
                    STAGE = 3;
                }
            }
            // locate 後: song が移動し、playback tick は戻っていないこと
            3 => {
                if song >= LOCATE_TARGET && now_tick >= PB_MARK {
                    CHK |= CHK_LOCATE;
                    if now_tick >= PB_MARK + BEAT {
                        SONG_AT_STOP = song;
                        PB_AT_STOP = now_tick;
                        if hostapi_transport_stop() == 0 {
                            CHK |= CHK_STOP;
                        }
                        RUNNING = false;
                        resync_after_discard();
                        STAGE = 5;
                        draw_button();
                    }
                }
            }
            // continue 後: 停止点から継続していること。2 小節走らせて終了
            6 => {
                if now_tick >= PB_AT_STOP && song >= SONG_AT_STOP {
                    CHK |= CHK_CONT;
                }
                if now_tick >= PB_AT_STOP + BAR * 2 {
                    hostapi_transport_stop();
                    RUNNING = false;
                    resync_after_discard();
                    STAGE = 7;
                    // 判定結果を CC で外へ出す(データバイトは 7bit なので
                    // 下位 7 個と 8 個目を 2 本に分ける)。画面を読めない
                    // Linux ホストではこれが合否の確認手段になる。
                    let lo = [0xB0u8, 0x77, (CHK & 0x7F) as u8];
                    hostapi_midi_send(lo.as_ptr(), 3);
                    let hi = [0xB0u8, 0x78, ((CHK >> 7) & 0x7F) as u8];
                    hostapi_midi_send(hi.as_ptr(), 3);
                    draw_button();
                }
            }
            _ => {}
        }
    }
}

#[no_mangle]
pub extern "C" fn app_tick() {
    drain_rx();

    let mut evs = [Event { ev_type: 0, param: 0, x: 0, y: 0, time_ms: 0 }; 8];
    let n = unsafe {
        hostapi_poll_event(evs.as_mut_ptr() as *mut u8,
                           (8 * core::mem::size_of::<Event>()) as u32)
    };
    for ev in &evs[..n.max(0) as usize] {
        // 再実行はボタン領域のタップのみ(結果を読んでいる最中に画面を
        // 触っても走り出さないようにする)
        if ev.ev_type == EV_TOUCH_DOWN && unsafe { !RUNNING }
            && (ev.y as i32) >= BTN_Y && (ev.x as i32) >= BTN_X0
            && (ev.x as i32) < BTN_X0 + BTN_W
        {
            start();
        }
    }

    let mut pos = [0u8; 32];
    if unsafe { hostapi_transport_get_position(pos.as_mut_ptr(), 32) } != 0 {
        return;
    }
    let host_us = u64::from_le_bytes([pos[0], pos[1], pos[2], pos[3],
                                      pos[4], pos[5], pos[6], pos[7]]);
    let now_tick = u32::from_le_bytes([pos[8], pos[9], pos[10], pos[11]]);
    let song = u32::from_le_bytes([pos[12], pos[13], pos[14], pos[15]]);
    let bar = u32::from_le_bytes([pos[16], pos[17], pos[18], pos[19]]);
    let upq = u32::from_le_bytes([pos[20], pos[21], pos[22], pos[23]]);
    let beat = u16::from_le_bytes([pos[24], pos[25]]);

    unsafe {
        LAST_HOST_US = host_us;

        if !RUNNING {
            // stage 5: STOPPED 中の time_us_to_tick は -1、その後 continue する
            if STAGE == 5 {
                if hostapi_time_us_to_tick(host_us as i64) == -1 {
                    CHK |= CHK_U2T_STOPPED;
                }
                if hostapi_transport_continue() == 0 {
                    RUNNING = true;
                    resync_after_discard();
                    STAGE = 6;
                    draw_button();
                }
            }
            draw_checks();
            draw_rx();
            return;
        }

        // time_us_to_tick(get_position の host_us) は同 tick を返すはず。
        // 2 回の呼び出しの間に進む分だけずれるので余裕を持って判定する。
        let t = hostapi_time_us_to_tick(host_us as i64);
        if t > 0 {
            let tu = t as u32;
            let d = if tu > now_tick { tu - now_tick } else { now_tick - tu };
            if d <= 100 {
                CHK |= CHK_U2T;
            }
        }

        advance_playing(now_tick, song, upq);
        supply(now_tick);

        let mut l = Line::new();
        l.push(b"bar ").push_u32(bar + 1).push(b" beat ").push_u32(beat as u32 + 1)
         .push(b" upq ").push_u32(upq);
        l.draw(12, 56);
        let mut l2 = Line::new();
        l2.push(b"pb ").push_u32(now_tick).push(b" song ").push_u32(song)
          .push(b" wrap ").push_u32(WRAPS);
        l2.draw(12, 80);
    }

    draw_checks();
    draw_rx();
}
