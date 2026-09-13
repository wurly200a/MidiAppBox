// Sequencer app(Phase 18)。Session 画面と単体再生。
//
// 曲構造の解釈と時間軸の計画は seqcore(Transport / timeline::Planner)が持ち、
// このアプリは Host API への写像と画面だけを担う。設計は docs/results/phase18.md のステップ 0。
//
// 画面(320x240)は Menu / Session 一覧 / Session 画面の 3 つ。描画スロットは座標キーで
// 解放されないため、全画面で同じ座標の組(rect 14 / text 14)を使い回す。
// app_init で rect → text の順に全スロットを作り、以後は更新だけにする(重なり順を固定するため)。
#![no_std]

use core::ptr::{addr_of, addr_of_mut};
use seqcore::{
    effective_meter, ticks_per_beat, BarPlan, Bank, Change, Planner, ProgramChange, Scope, Session,
    SessionId, State, TimeSig, Transport,
};

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

extern "C" {
    fn hostapi_draw_text(x: i32, y: i32, ptr: *const u8, len: u32);
    fn hostapi_fill_rect(x: i32, y: i32, w: i32, h: i32, rgb888: u32);
    fn hostapi_poll_event(buf: *mut u8, buf_len: u32) -> i32;
    fn hostapi_now_ms() -> u32;
    fn hostapi_tone_define(slot: i32, wave: i32, freq_hz: i32, dur_ms: i32, level: i32) -> i32;
    fn hostapi_midi_send(bytes: *const u8, len: u32) -> i32;

    fn hostapi_transport_start() -> i32;
    fn hostapi_transport_stop() -> i32;
    fn hostapi_transport_get_position(buf: *mut u8, buf_len: u32) -> i32;
    fn hostapi_tempomap_set_tempo(at_tick: i32, us_per_quarter: i32) -> i32;
    fn hostapi_tempomap_set_meter(at_tick: i32, numer: i32, denom: i32) -> i32;
    fn hostapi_tempomap_clear() -> i32;
    fn hostapi_seq_write(buf: *const u8, buf_len: u32) -> i32;
    fn hostapi_seq_flush_after(tick: i32) -> i32;
}

// ---- Host API の定数 ----
const PORT_CLICK: u8 = 3;
const OP_TONE: u8 = 1;
const OP_STOP: u8 = 2;
const TRANSPORT_STOPPED: u32 = 0;
const EV_TOUCH_DOWN: u16 = 1;
const EV_TOUCH_UP: u16 = 2;
const ACCENT_SLOT: u32 = 1; // 1 拍目のクリック(slot 0 は既定クリック)

// ---- 操作の定数 ----
/// 変更を「次の小節境界」に間に合わせる締め切り(app_tick 100ms + ジッタの余裕)
const GUARD_MS: u64 = 150;
const LONG_PRESS_MS: u32 = 600;
const BPM_MIN: i32 = 40;
const BPM_MAX: i32 = 240;
// 長押し連打加速(metronome と同じ)
const HOLD_INITIAL_DELAY_MS: u32 = 500;
const HOLD_ACCEL_1_MS: u32 = 1500;
const HOLD_ACCEL_2_MS: u32 = 3000;
const HOLD_INTERVAL_1_MS: u32 = 400;
const HOLD_INTERVAL_2_MS: u32 = 200;
const HOLD_INTERVAL_3_MS: u32 = 100;

// ---- レイアウト(docs/results/phase18.md 0-1)----
const HDR_BG: u32 = 0x30_50_90;
const BODY_BG: u32 = 0x10_18_28;
const ROW_BG: u32 = 0x2a_33_40;
const ROW_SEL: u32 = 0x20_50_a0;
const BTN_BG: u32 = 0x20_40_a0;
const BTN_ON: u32 = 0x20_80_40;
const BTN_OFF: u32 = 0x44_44_4c;
const BTN_STOP: u32 = 0xa0_30_30;

const ROWS: usize = 6;
const ROW_X: i32 = 4;
const ROW_Y0: i32 = 32;
const ROW_PITCH: i32 = 26;
const ROW_W: i32 = 270;
const ROW_H: i32 = 24;
const SCR_X: i32 = 280;
const SCR_W: i32 = 36;
const SCR_UP_Y: i32 = 32;
const SCR_DN_Y: i32 = 112;
const SCR_H: i32 = 76;
const BTN_Y: i32 = 196;
const BTN_W: i32 = 74;
const BTN_H: i32 = 40;
const BTN_XS: [i32; 4] = [4, 83, 162, 241];

// スロットの番号(キャッシュの添字)。rect と text で同じ並び
const S_HDR: usize = 0; // rect: ヘッダ背景 / text: パンくず
const S_BODY: usize = 1; // rect: 本体背景 / text: 状態(ヘッダ右)
const S_ROW0: usize = 2; // 2..8: 行
const S_UP: usize = 8;
const S_DN: usize = 9;
const S_BTN0: usize = 10; // 10..14: ボタン
const SLOTS: usize = 14;

fn rect_geom(i: usize) -> (i32, i32, i32, i32) {
    match i {
        S_HDR => (0, 0, 320, 28),
        S_BODY => (0, 28, 320, 212),
        S_UP => (SCR_X, SCR_UP_Y, SCR_W, SCR_H),
        S_DN => (SCR_X, SCR_DN_Y, SCR_W, SCR_H),
        i if i < S_UP => (ROW_X, ROW_Y0 + (i - S_ROW0) as i32 * ROW_PITCH, ROW_W, ROW_H),
        i => (BTN_XS[i - S_BTN0], BTN_Y, BTN_W, BTN_H),
    }
}

fn text_pos(i: usize) -> (i32, i32) {
    match i {
        S_HDR => (8, 7),
        S_BODY => (214, 7),
        S_UP => (SCR_X + 14, SCR_UP_Y + 30),
        S_DN => (SCR_X + 14, SCR_DN_Y + 30),
        i if i < S_UP => (ROW_X + 6, ROW_Y0 + (i - S_ROW0) as i32 * ROW_PITCH + 5),
        i => (BTN_XS[i - S_BTN0] + 8, BTN_Y + 12),
    }
}

// 同じ内容の再描画を省く(LVGL の再描画を減らす)
static mut RECT_CACHE: [u32; SLOTS] = [u32::MAX; SLOTS];
static mut TEXT_CACHE: [u32; SLOTS] = [u32::MAX; SLOTS];

fn fnv(s: &[u8]) -> u32 {
    let mut h: u32 = 0x811c_9dc5;
    for &b in s {
        h ^= b as u32;
        h = h.wrapping_mul(0x0100_0193);
    }
    h
}

fn put_rect(i: usize, color: u32) {
    unsafe {
        if RECT_CACHE[i] == color {
            return;
        }
        RECT_CACHE[i] = color;
        let (x, y, w, h) = rect_geom(i);
        hostapi_fill_rect(x, y, w, h, color);
    }
}

fn put_text(i: usize, s: &[u8]) {
    let h = fnv(s);
    unsafe {
        if TEXT_CACHE[i] == h {
            return;
        }
        TEXT_CACHE[i] = h;
        let (x, y) = text_pos(i);
        hostapi_draw_text(x, y, s.as_ptr(), s.len() as u32);
    }
}

struct Line {
    buf: [u8; 32],
    len: usize,
}

impl Line {
    fn new() -> Line {
        Line { buf: [b' '; 32], len: 0 }
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
    /// 右詰め(width 桁に満たなければ空白で埋める)
    fn push_num(&mut self, mut v: u32, width: usize) -> &mut Line {
        let mut d = [0u8; 10];
        let mut n = 0;
        loop {
            d[n] = b'0' + (v % 10) as u8;
            n += 1;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        for _ in n..width {
            self.push(b" ");
        }
        for k in (0..n).rev() {
            self.push(&[d[k]]);
        }
        self
    }
    /// 2 桁ゼロ詰め
    fn push_2d(&mut self, v: u32) -> &mut Line {
        if v < 10 {
            self.push(b"0");
        }
        self.push_num(v, 0)
    }
    /// 名前を width 文字ぶん(足りなければ空白)
    fn push_padded(&mut self, s: &[u8], width: usize) -> &mut Line {
        let start = self.len;
        self.push(s);
        while self.len - start < width {
            self.push(b" ");
        }
        self
    }
    fn bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }
}

// ---- データ ----

static mut BANK: Bank = Bank::new(); // 全ビット 0 = .bss(docs/results/phase16.md)

fn bank() -> &'static Bank {
    unsafe { &*addr_of!(BANK) }
}

/// 組み込みのデモデータ(永続化は Phase 20)。docs/results/phase18.md 0-4
fn build_demo_bank(b: &mut Bank) {
    let f44 = TimeSig::FOUR_FOUR;
    let _ = b.set_session(Session::new(0, 0, 4, f44).with_name("Intro"));
    let _ = b.set_session(
        Session::new(1, 1, 8, f44).with_name("Verse").with_bar_meter(7, TimeSig::new(2, 4)),
    );
    let _ = b.set_session(Session::new(2, 2, 8, f44).with_name("Chorus"));
    let _ = b.set_session(
        Session::new(3, 3, 4, TimeSig::new(3, 4))
            .with_name("Bridge")
            .with_bar_meter(3, TimeSig::new(7, 8)),
    );
    let _ = b.set_session(
        Session::new(4, 4, 2, TimeSig::new(7, 8)).with_name("Odd").with_bar_meter(1, TimeSig::new(5, 4)),
    );
    let _ = b.set_session(Session::new(5, 5, 16, f44).with_name("Long"));
}

// ---- 画面と操作の状態 ----

#[derive(Clone, Copy, PartialEq, Eq)]
enum Screen {
    Menu,
    List,
    Session,
}

static mut SCREEN: Screen = Screen::Menu;
static mut LIST_IDS: [SessionId; 64] = [0; 64];
static mut LIST_N: usize = 0;
static mut LIST_SEL: usize = 0;
static mut LIST_OFF: usize = 0;
static mut VIEW: SessionId = 0;
static mut BAR_SEL: u8 = 0;
static mut BAR_OFF: usize = 0;
static mut TOG_ONE: bool = false;
static mut TOG_RPT: bool = false;
static mut BPM: i32 = 120;

// ---- 再生の状態 ----

/// 再生中の計画。None = 停止中
static mut PLAN: Option<Planner> = None;
/// song tick 0 に対応する playback tick(transport_start の直後に取る)
static mut OFFSET: u32 = 0;
static mut SONG_NOW: u32 = 0;

// ---- タッチ ----
static mut PRESS_ACTIVE: bool = false;
static mut PRESS_X: i32 = 0;
static mut PRESS_Y: i32 = 0;
static mut PRESS_T: u32 = 0;
static mut LONG_DONE: bool = false;
static mut HOLD_DELTA: i32 = 0;
static mut HOLD_SINCE: u32 = 0;
static mut NEXT_REPEAT_AT: u32 = 0;

#[repr(C)]
#[derive(Clone, Copy)]
struct Event {
    ev_type: u16,
    param: u16,
    x: i16,
    y: i16,
    time_ms: u32,
}

/// hostapi_seq_event_t(16 バイト、ABI 凍結)
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
    const ZERO: SeqEvent = SeqEvent { tick: 0, port: 0, status: 0, data1: 0, data2: 0, param: 0, reserved: 0 };
}

// プレフィックス受理契約(docs/hostapi.md §5)の未受理分。1 小節ぶんのクリック + 停止が入る大きさ
const PEND_MAX: usize = 40;
static mut PEND: [SeqEvent; PEND_MAX] = [SeqEvent::ZERO; PEND_MAX];
static mut PEND_LEN: usize = 0;
static mut PEND_OFF: usize = 0;

struct Pos {
    tick: u32,
    song_tick: u32,
    upq: u32,
    state: u32,
}

fn get_position() -> Option<Pos> {
    let mut b = [0u8; 32];
    if unsafe { hostapi_transport_get_position(b.as_mut_ptr(), 32) } != 0 {
        return None;
    }
    let u32_at = |o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    Some(Pos { tick: u32_at(8), song_tick: u32_at(12), upq: u32_at(20), state: u32_at(28) })
}

fn upq_of(bpm: u16) -> i32 {
    let bpm = bpm.max(1) as u32;
    ((60_000_000u32 + bpm / 2) / bpm) as i32
}

fn plan() -> Option<Planner> {
    unsafe { *addr_of!(PLAN) }
}

fn playing_session() -> Option<SessionId> {
    plan().map(|p| p.current().session())
}

// ---- キューへの書き込み ----

fn drop_pending() {
    unsafe {
        PEND_LEN = 0;
        PEND_OFF = 0;
    }
}

fn push_event(e: SeqEvent) {
    unsafe {
        if PEND_LEN == PEND_MAX && PEND_OFF > 0 {
            // 受理済みの分を詰める
            let n = PEND_LEN - PEND_OFF;
            for i in 0..n {
                PEND[i] = PEND[PEND_OFF + i];
            }
            PEND_LEN = n;
            PEND_OFF = 0;
        }
        if PEND_LEN < PEND_MAX {
            PEND[PEND_LEN] = e;
            PEND_LEN += 1;
        }
    }
}

fn flush_pending() {
    unsafe {
        while PEND_OFF < PEND_LEN {
            let remain = PEND_LEN - PEND_OFF;
            let ptr = (addr_of!(PEND) as *const SeqEvent).add(PEND_OFF) as *const u8;
            let n = hostapi_seq_write(ptr, (remain * 16) as u32);
            if n <= 0 {
                return; // キュー満杯。次の tick で再送する
            }
            PEND_OFF += n as usize;
        }
        PEND_LEN = 0;
        PEND_OFF = 0;
    }
}

/// 小節のクリック(1 拍目はアクセント)
fn write_clicks(bar: &BarPlan) {
    let offset = unsafe { OFFSET };
    for i in 0..bar.meter.num {
        let mut e = SeqEvent::ZERO;
        e.tick = offset.wrapping_add(bar.beat_tick(i));
        e.port = PORT_CLICK;
        e.status = OP_TONE;
        e.param = if i == 0 { ACCENT_SLOT } else { 0 };
        push_event(e);
    }
}

/// 次の小節を積む: テンポ / 拍子(変化が無くても毎回同じ at_tick に書く)+ クリック。
/// 次が無ければ今の小節の終わりに OP_STOP を積む
fn write_next(pl: &Planner) {
    let offset = unsafe { OFFSET };
    match pl.next() {
        Some(n) => {
            unsafe {
                hostapi_tempomap_set_tempo(n.start_tick as i32, upq_of(n.bpm));
                hostapi_tempomap_set_meter(n.start_tick as i32, n.meter.num as i32, n.meter.den as i32);
            }
            write_clicks(n);
        }
        None => {
            let mut e = SeqEvent::ZERO;
            e.tick = offset.wrapping_add(pl.current().end_tick());
            e.status = OP_STOP;
            push_event(e);
        }
    }
    flush_pending();
}

// ---- 再生 ----

fn play(id: SessionId) {
    if plan().is_some() {
        stop_playback();
    }
    unsafe {
        let b = bank();
        let mut t = Transport::new();
        let _ = t.set_bpm(BPM as u16);
        let single = if TOG_ONE { Some(BAR_SEL) } else { None };
        let ev = match t.play_session(b, id, single, TOG_RPT) {
            Ok(ev) => ev,
            Err(_) => return,
        };
        // 再生を始め直すたびに stop → clear → 初期値 → start(docs/results/phase17.md)
        hostapi_transport_stop();
        hostapi_tempomap_clear();
        hostapi_tempomap_set_tempo(0, upq_of(t.bpm()));
        let m = t.meter();
        hostapi_tempomap_set_meter(0, m.num as i32, m.den as i32);
        if let Some(s) = b.session(id) {
            // Session scope でも再生開始時に即時 PC を送る(Phase 18 の決定、spec §4.2 からの逸脱)
            let pc = ProgramChange { program: s.program, cue: false }.bytes();
            hostapi_midi_send(pc.as_ptr(), 2);
        }
        if hostapi_transport_start() != 0 {
            return;
        }
        // transport_start はキューを空にするので、クリックと OP_STOP はこの後に積む
        drop_pending();
        OFFSET = match get_position() {
            Some(p) => p.tick.wrapping_sub(p.song_tick),
            None => 0,
        };
        SONG_NOW = 0;
        let Some(pl) = Planner::begin(b, t, ev) else {
            hostapi_transport_stop();
            return;
        };
        write_clicks(pl.current());
        write_next(&pl);
        PLAN = Some(pl);
    }
}

fn stop_playback() {
    unsafe {
        hostapi_transport_stop();
        drop_pending();
        PLAN = None;
    }
}

/// 再生中の計画に変更を渡す。次の小節の頭まで GUARD_MS 以上あれば次の境界から、
/// なければその次の境界から効く(docs/results/phase18.md 0-3)
fn apply_change(change: Change) {
    unsafe {
        let Some(pl) = (*addr_of_mut!(PLAN)).as_mut() else {
            return;
        };
        let in_time = match get_position() {
            Some(p) => {
                let remain_ticks = pl.current().end_tick().saturating_sub(p.song_tick) as u64;
                remain_ticks * p.upq as u64 / 960 / 1000 >= GUARD_MS
            }
            None => false,
        };
        if pl.apply(bank(), change, in_time).is_err() {
            return;
        }
        if in_time {
            // 次の小節の頭以降を捨てて積み直す。テンポ / 拍子は同じ at_tick へ上書きされる
            hostapi_seq_flush_after(OFFSET.wrapping_add(pl.current().end_tick()) as i32);
            drop_pending();
            let snapshot = *pl;
            write_next(&snapshot);
        }
    }
}

fn apply_toggles() {
    unsafe {
        if playing_session() == Some(VIEW) {
            let single_bar = if TOG_ONE { Some(BAR_SEL) } else { None };
            apply_change(Change::Toggles { single_bar, repeat: TOG_RPT });
        }
    }
}

fn apply_bpm_delta(delta: i32) {
    unsafe {
        let v = (BPM + delta).clamp(BPM_MIN, BPM_MAX);
        if v != BPM {
            BPM = v;
            if plan().is_some() {
                apply_change(Change::Bpm(v as u16));
            }
        }
    }
}

// ---- 描画 ----

fn blink_on() -> bool {
    match plan() {
        Some(pl) => {
            let cur = pl.current();
            let beat = ticks_per_beat(cur.meter).max(1);
            let off = unsafe { SONG_NOW }.saturating_sub(cur.start_tick);
            off % beat < beat / 2
        }
        None => false,
    }
}

fn session_bars(id: SessionId) -> usize {
    bank().session(id).map_or(0, |s| s.bars as usize)
}

fn row_content(screen: Screen, r: usize) -> (u32, Line) {
    let mut l = Line::new();
    unsafe {
        match screen {
            Screen::Menu => match r {
                0 => {
                    l.push(b"Session");
                    (ROW_BG, l)
                }
                1 => {
                    l.push(b"Song (Phase 19)");
                    (ROW_BG, l)
                }
                _ => (BODY_BG, l),
            },
            Screen::List => {
                let idx = LIST_OFF + r;
                if idx >= LIST_N {
                    return (BODY_BG, l);
                }
                let id = LIST_IDS[idx];
                let Some(s) = bank().session(id) else {
                    return (BODY_BG, l);
                };
                let mark = playing_session() == Some(id) && blink_on();
                l.push(if mark { b">" } else { b" " })
                    .push(b"S")
                    .push_2d(id as u32 + 1)
                    .push(b" ")
                    .push_padded(s.name.as_str().as_bytes(), 7)
                    .push_num(s.bars as u32, 2)
                    .push(b"bar ")
                    .push_num(s.meter.num as u32, 0)
                    .push(b"/")
                    .push_num(s.meter.den as u32, 0);
                (if idx == LIST_SEL { ROW_SEL } else { ROW_BG }, l)
            }
            Screen::Session => {
                let Some(s) = bank().session(VIEW) else {
                    return (BODY_BG, l);
                };
                let bar = BAR_OFF + r;
                if bar >= s.bars as usize {
                    return (BODY_BG, l);
                }
                let playing_here = match plan() {
                    Some(pl) => pl.current().session() == VIEW && pl.current().pos.bar as usize == bar,
                    None => false,
                };
                l.push(if playing_here && blink_on() { b">" } else { b" " })
                    .push_num(bar as u32 + 1, 2);
                let m = effective_meter(s, bar as u8);
                if m != s.meter {
                    l.push(b"  [").push_num(m.num as u32, 0).push(b"/").push_num(m.den as u32, 0).push(b"]");
                }
                (if bar == BAR_SEL as usize { ROW_SEL } else { ROW_BG }, l)
            }
        }
    }
}

fn button(screen: Screen, i: usize) -> (u32, &'static [u8]) {
    unsafe {
        match screen {
            Screen::Menu => (BODY_BG, b""),
            Screen::List => (BTN_BG, [&b"BACK"[..], b"BPM-", b"BPM+", b"OPEN"][i]),
            Screen::Session => match i {
                0 => (BTN_BG, b"BACK"),
                1 => if TOG_ONE { (BTN_ON, b"1:ON") } else { (BTN_OFF, b"1:OFF") },
                2 => if TOG_RPT { (BTN_ON, b"RPT:ON") } else { (BTN_OFF, b"RPT:OFF") },
                _ => {
                    if playing_session() == Some(VIEW) {
                        (BTN_STOP, b"STOP")
                    } else {
                        (BTN_ON, b"PLAY")
                    }
                }
            },
        }
    }
}

fn render() {
    unsafe {
        let screen = SCREEN;
        put_rect(S_HDR, HDR_BG);
        put_rect(S_BODY, BODY_BG);

        let mut title = Line::new();
        match screen {
            Screen::Menu => {
                title.push(b"Menu");
            }
            Screen::List => {
                title.push(b"Session  ").push_num(BPM as u32, 0).push(b"bpm");
            }
            Screen::Session => {
                title.push(b"Session > S").push_2d(VIEW as u32 + 1);
            }
        }
        put_text(S_HDR, title.bytes());

        // ヘッダ右: 再生中は位置と今の小節の拍子(画面外でも見えるように。一覧の行は
        // Session の既定の拍子なので、小節の上書き(7/8 等)はここでしか見えない)、停止中は BPM
        let mut status = Line::new();
        match plan() {
            Some(pl) => {
                let cur = pl.current();
                status
                    .push(b"S")
                    .push_2d(cur.session() as u32 + 1)
                    .push(b" B")
                    .push_num(cur.pos.bar as u32 + 1, 0)
                    .push(b" ")
                    .push_num(cur.meter.num as u32, 0)
                    .push(b"/")
                    .push_num(cur.meter.den as u32, 0);
            }
            // 一覧はタイトルに BPM を出しているので、ここでは重ねて出さない
            None if screen == Screen::List => {}
            None => {
                status.push_num(BPM as u32, 0).push(b"bpm");
            }
        }
        put_text(S_BODY, status.bytes());

        for r in 0..ROWS {
            let (color, line) = row_content(screen, r);
            put_rect(S_ROW0 + r, color);
            put_text(S_ROW0 + r, line.bytes());
        }

        let scroll = screen != Screen::Menu;
        put_rect(S_UP, if scroll { ROW_BG } else { BODY_BG });
        put_rect(S_DN, if scroll { ROW_BG } else { BODY_BG });
        put_text(S_UP, if scroll { b"^" } else { b"" });
        put_text(S_DN, if scroll { b"v" } else { b"" });

        for i in 0..4 {
            let (color, label) = button(screen, i);
            put_rect(S_BTN0 + i, color);
            put_text(S_BTN0 + i, label);
        }
    }
}

// ---- 入力 ----

fn row_at(x: i32, y: i32) -> Option<usize> {
    if x < ROW_X || x >= ROW_X + ROW_W || y < ROW_Y0 || y >= ROW_Y0 + ROWS as i32 * ROW_PITCH {
        return None;
    }
    let dy = y - ROW_Y0;
    if dy % ROW_PITCH >= ROW_H {
        return None;
    }
    Some((dy / ROW_PITCH) as usize)
}

fn in_rect(x: i32, y: i32, rx: i32, ry: i32, rw: i32, rh: i32) -> bool {
    x >= rx && x < rx + rw && y >= ry && y < ry + rh
}

fn button_at(x: i32, y: i32) -> Option<usize> {
    BTN_XS.iter().position(|&bx| in_rect(x, y, bx, BTN_Y, BTN_W, BTN_H))
}

fn scroll_at(x: i32, y: i32) -> Option<bool> {
    if in_rect(x, y, SCR_X, SCR_UP_Y, SCR_W, SCR_H) {
        Some(true)
    } else if in_rect(x, y, SCR_X, SCR_DN_Y, SCR_W, SCR_H) {
        Some(false)
    } else {
        None
    }
}

fn open_session(id: SessionId) {
    unsafe {
        VIEW = id;
        BAR_SEL = 0;
        BAR_OFF = 0;
        // 再生中の Session を開いたら、トグルの表示を実際の再生状態に合わせる
        if let Some(pl) = plan() {
            if pl.current().session() == id {
                if let State::Playing { scope: Scope::Session { single_bar, repeat, .. }, .. } =
                    pl.live().state()
                {
                    TOG_ONE = single_bar.is_some();
                    TOG_RPT = repeat;
                    BAR_SEL = single_bar.unwrap_or(0);
                }
            }
        }
        SCREEN = Screen::Session;
    }
}

fn on_tap(x: i32, y: i32) {
    unsafe {
        match SCREEN {
            Screen::Menu => {
                if row_at(x, y) == Some(0) {
                    SCREEN = Screen::List;
                }
            }
            Screen::List => {
                if let Some(r) = row_at(x, y) {
                    let idx = LIST_OFF + r;
                    if idx < LIST_N {
                        if idx == LIST_SEL {
                            open_session(LIST_IDS[idx]);
                        } else {
                            LIST_SEL = idx;
                        }
                    }
                } else if let Some(up) = scroll_at(x, y) {
                    if up && LIST_OFF > 0 {
                        LIST_OFF -= 1;
                    } else if !up && LIST_OFF + ROWS < LIST_N {
                        LIST_OFF += 1;
                    }
                } else {
                    match button_at(x, y) {
                        Some(0) => SCREEN = Screen::Menu,
                        Some(3) if LIST_SEL < LIST_N => open_session(LIST_IDS[LIST_SEL]),
                        _ => {} // BPM± は押下時に処理済み
                    }
                }
            }
            Screen::Session => {
                let bars = session_bars(VIEW);
                if let Some(r) = row_at(x, y) {
                    let bar = BAR_OFF + r;
                    if bar < bars {
                        BAR_SEL = bar as u8;
                        if TOG_ONE {
                            apply_toggles(); // `1` の対象が変わる
                        }
                    }
                } else if let Some(up) = scroll_at(x, y) {
                    if up && BAR_OFF > 0 {
                        BAR_OFF -= 1;
                    } else if !up && BAR_OFF + ROWS < bars {
                        BAR_OFF += 1;
                    }
                } else {
                    match button_at(x, y) {
                        Some(0) => {
                            SCREEN = Screen::List;
                            if let Some(idx) = LIST_IDS[..LIST_N].iter().position(|&i| i == VIEW) {
                                LIST_SEL = idx;
                                if idx < LIST_OFF {
                                    LIST_OFF = idx;
                                } else if idx >= LIST_OFF + ROWS {
                                    LIST_OFF = idx + 1 - ROWS;
                                }
                            }
                        }
                        Some(1) => {
                            TOG_ONE = !TOG_ONE;
                            apply_toggles();
                        }
                        Some(2) => {
                            TOG_RPT = !TOG_RPT;
                            apply_toggles();
                        }
                        Some(3) => {
                            if playing_session() == Some(VIEW) {
                                stop_playback();
                            } else {
                                play(VIEW);
                            }
                        }
                        _ => {}
                    }
                }
            }
        }
    }
}

fn on_down(x: i32, y: i32, now: u32) {
    unsafe {
        PRESS_ACTIVE = true;
        PRESS_X = x;
        PRESS_Y = y;
        PRESS_T = now;
        LONG_DONE = false;
        if SCREEN == Screen::List {
            match button_at(x, y) {
                Some(1) => start_repeat(-1, now),
                Some(2) => start_repeat(1, now),
                _ => {}
            }
        }
    }
}

fn on_up() {
    unsafe {
        if PRESS_ACTIVE && !LONG_DONE && HOLD_DELTA == 0 {
            on_tap(PRESS_X, PRESS_Y);
        }
        PRESS_ACTIVE = false;
        HOLD_DELTA = 0;
    }
}

/// now_ms は wraparound しうるので、差分を符号付きで見て到達判定する
fn time_reached(now: u32, target: u32) -> bool {
    (now.wrapping_sub(target) as i32) >= 0
}

fn start_repeat(delta: i32, now: u32) {
    apply_bpm_delta(delta);
    unsafe {
        HOLD_DELTA = delta;
        HOLD_SINCE = now;
        NEXT_REPEAT_AT = now.wrapping_add(HOLD_INITIAL_DELAY_MS);
    }
}

fn process_hold(now: u32) {
    unsafe {
        // BPM± の長押し連打
        if HOLD_DELTA != 0 && time_reached(now, NEXT_REPEAT_AT) {
            apply_bpm_delta(HOLD_DELTA);
            let elapsed = now.wrapping_sub(HOLD_SINCE);
            let interval = if elapsed < HOLD_ACCEL_1_MS {
                HOLD_INTERVAL_1_MS
            } else if elapsed < HOLD_ACCEL_2_MS {
                HOLD_INTERVAL_2_MS
            } else {
                HOLD_INTERVAL_3_MS
            };
            NEXT_REPEAT_AT = now.wrapping_add(interval);
        }
        // Session 画面の行の長押し = ジャンプ(Q5)。停止中は選択だけ
        if PRESS_ACTIVE
            && !LONG_DONE
            && SCREEN == Screen::Session
            && now.wrapping_sub(PRESS_T) >= LONG_PRESS_MS
        {
            if let Some(r) = row_at(PRESS_X, PRESS_Y) {
                let bar = BAR_OFF + r;
                if bar < session_bars(VIEW) {
                    LONG_DONE = true;
                    BAR_SEL = bar as u8;
                    if playing_session() == Some(VIEW) {
                        apply_change(Change::JumpBar(bar as u8));
                    }
                }
            }
        }
    }
}

// ---- エントリポイント ----

#[no_mangle]
pub extern "C" fn app_init() -> i32 {
    unsafe {
        build_demo_bank(&mut *addr_of_mut!(BANK));
        LIST_N = 0;
        for id in 0..64u8 {
            if bank().session(id).is_some() {
                LIST_IDS[LIST_N] = id;
                LIST_N += 1;
            }
        }
        hostapi_tone_define(ACCENT_SLOT as i32, 0 /*SINE*/, 1568, 30, 100);
    }
    // 全スロットを rect → text の順に作る(以後は同じ座標を更新するだけ)
    for i in 0..SLOTS {
        put_rect(i, BODY_BG);
    }
    for i in 0..SLOTS {
        put_text(i, b"");
    }
    render();
    0
}

#[no_mangle]
pub extern "C" fn app_tick() {
    let now = unsafe { hostapi_now_ms() };

    let mut evs = [Event { ev_type: 0, param: 0, x: 0, y: 0, time_ms: 0 }; 8];
    let n = unsafe {
        hostapi_poll_event(evs.as_mut_ptr() as *mut u8, (8 * core::mem::size_of::<Event>()) as u32)
    };
    for ev in &evs[..n.max(0) as usize] {
        match ev.ev_type {
            EV_TOUCH_DOWN => on_down(ev.x as i32, ev.y as i32, ev.time_ms),
            EV_TOUCH_UP => on_up(),
            _ => {}
        }
    }
    process_hold(now);

    unsafe {
        let mut finished = false;
        if let Some(pl) = (*addr_of_mut!(PLAN)).as_mut() {
            match get_position() {
                // OP_STOP による自然終了(または他の理由で止まった)
                Some(p) if p.state == TRANSPORT_STOPPED => finished = true,
                Some(p) => {
                    SONG_NOW = p.song_tick;
                    while pl.advance_to(bank(), p.song_tick) {
                        let snapshot = *pl;
                        write_next(&snapshot);
                    }
                    flush_pending();
                }
                None => {}
            }
        }
        if finished {
            drop_pending();
            PLAN = None;
        }
    }
    render();
}
