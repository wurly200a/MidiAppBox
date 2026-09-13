// Transport(再生機構、spec §4)。
//
// 装置に 1 つだけ存在する。MIDI 送信・Host API 呼び出しはしない。
// 小節境界で前進させると、境界で送るべきもの(Start / Stop / PC / テンポ / 拍子)を
// `BarEvents` として値で返す。アプリはそれを Host API(transport_* / tempomap_* /
// seq_write)へ写す。
//
// 先読み: Transport は Copy なので、境界より前に「次の小節で何が起きるか」を知りたいときは
// `let mut peek = transport; let ev = peek.advance_bar(bank);` と複製して進めればよい
// (PC を PC_LEAD_TICKS だけ手前に予約するのに使う)。

use crate::model::{Bank, Session, SessionId, Song, SongPos, TimeSig};
use crate::resolve::{effective_meter, effective_tempo};

/// Transport の初期テンポ(spec §3.2)
pub const DEFAULT_BPM: u16 = 120;

/// SL MK3 が Session 切替の Program Change を受けるチャンネル(ch16。0 始まりで 15)
pub const PC_CHANNEL: u8 = 15;
/// PC 番号に足すと、即時ではなく「再生中パターンの末尾」で切り替わる(SL MK3)
pub const PC_CUE_OFFSET: u8 = 64;
/// Session 境界の PC を境界より何 tick 手前に送るか(24ppqn = MIDI Clock の数)。
/// 暫定値 = 4 分音符 1 つ。根拠は docs/results/phase16.md「SL MK3」
pub const PC_LEAD_TICKS: u32 = 24;

/// 送るべき Program Change。`cue` は SL MK3 のキュー(パターン末尾で切替)を使うか
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ProgramChange {
    pub program: u8,
    pub cue: bool,
}

impl ProgramChange {
    /// 送信バイト列(ステータス + データ)。
    /// cue なら program(0..=63)に PC_CUE_OFFSET を足す。範囲外の上位ビットは落とす
    pub const fn bytes(self) -> [u8; 2] {
        let data = if self.cue {
            (self.program & 0x3F) | PC_CUE_OFFSET
        } else {
            self.program & 0x7F
        };
        [0xC0 | PC_CHANNEL, data]
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Scope {
    /// arrangement 全体を 1 回
    Song { song: u8 },
    /// `single_bar` = トグル `1`(Some なら選択した小節)、`repeat` = 矢印トグル
    Session {
        session: SessionId,
        single_bar: Option<u8>,
        repeat: bool,
    },
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Position {
    pub song_pos: Option<SongPos>, // Song scope のときだけ
    pub session: SessionId,
    pub bar: u8,
    pub beat: u8, // 小節内の拍(0 始まり、拍子の分母単位)
}

/// 次の小節境界で実行する操作
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum QueuedAction {
    Stop,
    /// Song scope 用
    Jump(SongPos),
    /// Session scope 用。spec §4 には無い(Session 画面の小節ジャンプに要るため追加)
    JumpBar(u8),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
    Stopped,
    Playing {
        scope: Scope,
        pos: Position,
        queued: Option<QueuedAction>,
    },
}

/// 境界で送るべきもの。`None` / `false` は「送らなくてよい」
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct BarEvents {
    /// MIDI Start(再生開始)
    pub start: bool,
    /// MIDI Stop(この境界で終了した)
    pub stop: bool,
    /// Session 切替の Program Change
    pub pc: Option<ProgramChange>,
    /// テンポが変わった(開始時は必ず Some)
    pub tempo: Option<u16>,
    /// 拍子が変わった(開始時は必ず Some)
    pub meter: Option<TimeSig>,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Error {
    AlreadyPlaying,
    NotPlaying,
    NoSuchSong,
    NoSuchSession,
    /// 再生できる Session が 1 つも無い
    EmptySong,
    BarOutOfRange,
    InvalidPosition,
    /// 現在の Scope では使えない操作
    WrongScope,
}

/// spec §4 の `enum Transport` を `State` とし、Session scope が使う
/// 「Transport の現在テンポ」(spec §3.2)と、変化を検出するための現在の拍子を足した
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Transport {
    state: State,
    bpm: u16,
    /// Session scope 再生中に set_bpm された値。次の小節境界で効く
    requested_bpm: Option<u16>,
    meter: TimeSig,
}

impl Default for Transport {
    fn default() -> Transport {
        Transport::new()
    }
}

impl Transport {
    pub const fn new() -> Transport {
        Transport {
            state: State::Stopped,
            bpm: DEFAULT_BPM,
            requested_bpm: None,
            meter: TimeSig::FOUR_FOUR,
        }
    }

    pub fn state(&self) -> State {
        self.state
    }

    pub fn is_playing(&self) -> bool {
        matches!(self.state, State::Playing { .. })
    }

    pub fn position(&self) -> Option<Position> {
        match self.state {
            State::Playing { pos, .. } => Some(pos),
            State::Stopped => None,
        }
    }

    /// 現在の小節のテンポ(停止中は前回使用値)
    pub fn bpm(&self) -> u16 {
        self.bpm
    }

    /// 現在の小節の拍子
    pub fn meter(&self) -> TimeSig {
        self.meter
    }

    /// Song の arrangement を先頭から 1 回再生する。
    /// 開始時の PC は SL MK3 がまだ再生していないので即時(cue なし)
    pub fn play_song(&mut self, bank: &Bank, song: u8) -> Result<BarEvents, Error> {
        if self.is_playing() {
            return Err(Error::AlreadyPlaying);
        }
        let s = bank.songs.get(song as usize).ok_or(Error::NoSuchSong)?;
        let p = s.first_pos(bank).ok_or(Error::EmptySong)?;
        let sess = s.session_at(bank, p).ok_or(Error::EmptySong)?;
        self.requested_bpm = None;
        self.bpm = effective_tempo(s, p);
        self.meter = effective_meter(sess, p.bar);
        self.state = State::Playing {
            scope: Scope::Song { song },
            pos: Position { song_pos: Some(p), session: sess.id, bar: p.bar, beat: 0 },
            queued: None,
        };
        Ok(BarEvents {
            start: true,
            pc: Some(ProgramChange { program: sess.program, cue: false }),
            tempo: Some(self.bpm),
            meter: Some(self.meter),
            ..BarEvents::default()
        })
    }

    /// Session を単体で再生する(テンポは Transport の現在テンポ)。
    /// spec §4.2 に従い PC は送らない(Song scope のみ)
    pub fn play_session(
        &mut self,
        bank: &Bank,
        session: SessionId,
        single_bar: Option<u8>,
        repeat: bool,
    ) -> Result<BarEvents, Error> {
        if self.is_playing() {
            return Err(Error::AlreadyPlaying);
        }
        let s = bank.session(session).ok_or(Error::NoSuchSession)?;
        if single_bar.is_some_and(|b| b >= s.bars) {
            return Err(Error::BarOutOfRange);
        }
        let bar = single_bar.unwrap_or(0);
        self.meter = effective_meter(s, bar);
        self.state = State::Playing {
            scope: Scope::Session { session, single_bar, repeat },
            pos: Position { song_pos: None, session, bar, beat: 0 },
            queued: None,
        };
        Ok(BarEvents {
            start: true,
            tempo: Some(self.bpm),
            meter: Some(self.meter),
            ..BarEvents::default()
        })
    }

    /// Transport の現在テンポを変える。停止中は即時、Session scope の再生中は
    /// 次の小節境界から。Song scope ではテンポは TempoTrigger が決めるので不可
    pub fn set_bpm(&mut self, bpm: u16) -> Result<(), Error> {
        match self.state {
            State::Stopped => {
                self.bpm = bpm;
                Ok(())
            }
            State::Playing { scope: Scope::Session { .. }, .. } => {
                self.requested_bpm = Some(bpm);
                Ok(())
            }
            State::Playing { .. } => Err(Error::WrongScope),
        }
    }

    /// 再生中の Session scope のトグル(`1` / 矢印)を変える。次の小節境界から効く
    pub fn set_session_toggles(
        &mut self,
        bank: &Bank,
        single_bar: Option<u8>,
        repeat: bool,
    ) -> Result<(), Error> {
        let playing = self.is_playing();
        let State::Playing { scope: Scope::Session { session, single_bar: sb, repeat: rp }, .. } =
            &mut self.state
        else {
            return Err(if playing { Error::WrongScope } else { Error::NotPlaying });
        };
        let s = bank.session(*session).ok_or(Error::NoSuchSession)?;
        if single_bar.is_some_and(|b| b >= s.bars) {
            return Err(Error::BarOutOfRange);
        }
        *sb = single_bar;
        *rp = repeat;
        Ok(())
    }

    /// 次の小節境界で実行する操作を予約する(既存の予約は置き換える)
    pub fn queue(&mut self, bank: &Bank, action: QueuedAction) -> Result<(), Error> {
        let State::Playing { scope, queued, .. } = &mut self.state else {
            return Err(Error::NotPlaying);
        };
        match (action, *scope) {
            (QueuedAction::Stop, _) => {}
            (QueuedAction::Jump(p), Scope::Song { song }) => {
                let s = bank.songs.get(song as usize).ok_or(Error::NoSuchSong)?;
                s.session_at(bank, p).ok_or(Error::InvalidPosition)?;
            }
            (QueuedAction::JumpBar(b), Scope::Session { session, .. }) => {
                let s = bank.session(session).ok_or(Error::NoSuchSession)?;
                if b >= s.bars {
                    return Err(Error::BarOutOfRange);
                }
            }
            _ => return Err(Error::WrongScope),
        }
        *queued = Some(action);
        Ok(())
    }

    /// 即時停止
    pub fn stop(&mut self) -> BarEvents {
        if !self.is_playing() {
            return BarEvents::default();
        }
        if let Some(bpm) = self.requested_bpm.take() {
            self.bpm = bpm;
        }
        self.state = State::Stopped;
        BarEvents { stop: true, ..BarEvents::default() }
    }

    /// 1 拍進める。小節をまたいだら `advance_bar` の結果を返す
    pub fn advance_beat(&mut self, bank: &Bank) -> Option<BarEvents> {
        let State::Playing { pos, .. } = &mut self.state else {
            return None;
        };
        if pos.beat + 1 < self.meter.num {
            pos.beat += 1;
            return None;
        }
        Some(self.advance_bar(bank))
    }

    /// 次の小節へ進める。ここで予約操作・トグル・テンポ要求が反映され、
    /// 有効テンポ / 有効拍子が再評価される。進む先が無ければ停止する
    pub fn advance_bar(&mut self, bank: &Bank) -> BarEvents {
        let State::Playing { scope, pos, queued } = self.state else {
            return BarEvents::default();
        };
        let step = match scope {
            Scope::Song { song } => bank
                .songs
                .get(song as usize)
                .and_then(|s| song_step(bank, s, pos.song_pos?, queued)),
            Scope::Session { session, single_bar, repeat } => bank.session(session).and_then(|s| {
                let bar = next_session_bar(s, pos.bar, single_bar, repeat, queued)?;
                Some(Step {
                    song_pos: None,
                    session,
                    bar,
                    bpm: self.requested_bpm.unwrap_or(self.bpm),
                    meter: effective_meter(s, bar),
                    pc: None,
                })
            }),
        };
        let Some(step) = step else {
            return self.stop();
        };
        let ev = BarEvents {
            pc: step.pc,
            tempo: (step.bpm != self.bpm).then_some(step.bpm),
            meter: (step.meter != self.meter).then_some(step.meter),
            ..BarEvents::default()
        };
        self.requested_bpm = None;
        self.bpm = step.bpm;
        self.meter = step.meter;
        self.state = State::Playing {
            scope,
            pos: Position { song_pos: step.song_pos, session: step.session, bar: step.bar, beat: 0 },
            queued: None,
        };
        ev
    }
}

/// 小節境界の遷移先
struct Step {
    song_pos: Option<SongPos>,
    session: SessionId,
    bar: u8,
    bpm: u16,
    meter: TimeSig,
    pc: Option<ProgramChange>,
}

fn song_step(bank: &Bank, song: &Song, cur: SongPos, queued: Option<QueuedAction>) -> Option<Step> {
    let p = match queued {
        Some(QueuedAction::Stop) => return None,
        // 予約後に Bank が変わって無効になったジャンプは捨てて通常進行
        Some(QueuedAction::Jump(p)) if song.session_at(bank, p).is_some() => p,
        _ => song.next_pos(bank, cur)?,
    };
    let sess = song.session_at(bank, p)?;
    // Session 境界 = arrangement 上の Session の枠が変わったとき。
    // 同じ Session が続けて参照されていても枠が違えば送る(spec §4.2)
    let crossed = (p.arr_idx, p.sess_idx) != (cur.arr_idx, cur.sess_idx);
    Some(Step {
        song_pos: Some(p),
        session: sess.id,
        bar: p.bar,
        bpm: effective_tempo(song, p),
        meter: effective_meter(sess, p.bar),
        pc: crossed.then_some(ProgramChange { program: sess.program, cue: true }),
    })
}

/// spec §4.1 の 4 通り:
///   `1` OFF・矢印 OFF: 全小節を再生して停止 / `1` OFF・矢印 ON: 全小節を繰り返し
///   `1` ON ・矢印 OFF: 選択小節だけ再生して停止 / `1` ON ・矢印 ON: 選択小節を繰り返し
/// 再生中に `1` を ON にした場合は、次の境界で選択小節へ移り、それを弾き終えたら停止する
fn next_session_bar(
    s: &Session,
    cur: u8,
    single_bar: Option<u8>,
    repeat: bool,
    queued: Option<QueuedAction>,
) -> Option<u8> {
    match queued {
        Some(QueuedAction::Stop) => return None,
        Some(QueuedAction::JumpBar(b)) if b < s.bars => return Some(b),
        _ => {}
    }
    match single_bar {
        Some(b) if b < s.bars => (repeat || cur != b).then_some(b),
        _ if cur.saturating_add(1) < s.bars => Some(cur + 1),
        _ => repeat.then_some(0),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::tests::sample_bank;
    use crate::model::{Chapter, TempoTrigger};
    use std::vec::Vec;

    fn bar(t: &Transport) -> u8 {
        t.position().unwrap().bar
    }

    fn song_pos(t: &Transport) -> (u8, u8, u8) {
        let p = t.position().unwrap().song_pos.unwrap();
        (p.arr_idx, p.sess_idx, p.bar)
    }

    fn pc(program: u8, cue: bool) -> Option<ProgramChange> {
        Some(ProgramChange { program, cue })
    }

    #[test]
    fn song_plays_the_whole_arrangement_once_then_stops() {
        let bank = sample_bank();
        let mut t = Transport::new();
        let ev = t.play_song(&bank, 0).unwrap();
        assert!(ev.start && !ev.stop);
        let mut seen = Vec::new();
        loop {
            seen.push(song_pos(&t));
            let ev = t.advance_bar(&bank);
            if ev.stop {
                break;
            }
            assert!(seen.len() < 100, "終端で止まらない");
        }
        assert_eq!(
            seen,
            [(0, 0, 0), (0, 0, 1), (0, 1, 0), (1, 0, 0), (1, 0, 1), (2, 0, 0), (2, 0, 1), (2, 1, 0)]
        );
        assert_eq!(t.state(), State::Stopped);
        // 停止後に進めても何も起きない
        assert_eq!(t.advance_bar(&bank), BarEvents::default());
    }

    #[test]
    fn song_events_at_each_bar_of_the_sample() {
        let bank = sample_bank();
        let mut t = Transport::new();
        let four = Some(TimeSig::FOUR_FOUR);
        let start = t.play_song(&bank, 0).unwrap();
        assert_eq!((start.pc, start.tempo, start.meter), (pc(10, false), Some(100), four));
        // (pc, meter) を小節ごとに。テンポは TempoTrigger が無いので変わらない
        let expected = [
            (None, None),                         // (0,0,1)
            (pc(11, true), Some(TimeSig::new(3, 4))), // (0,1,0)
            (pc(12, true), four),                 // (1,0,0)
            (None, Some(TimeSig::new(7, 8))),     // (1,0,1)
            (pc(10, true), four),                 // (2,0,0)
            (None, None),                         // (2,0,1)
            (pc(11, true), Some(TimeSig::new(3, 4))), // (2,1,0)
        ];
        for (i, (want_pc, want_meter)) in expected.into_iter().enumerate() {
            let ev = t.advance_bar(&bank);
            assert_eq!((ev.pc, ev.meter, ev.tempo), (want_pc, want_meter, None), "bar {}", i + 1);
        }
    }

    #[test]
    fn session_all_bars_once_then_stops() {
        let bank = sample_bank();
        let mut t = Transport::new();
        let ev = t.play_session(&bank, 0, None, false).unwrap();
        assert!(ev.start);
        assert_eq!(ev.pc, None); // Session scope では PC を送らない
        assert_eq!(bar(&t), 0);
        assert!(!t.advance_bar(&bank).stop);
        assert_eq!(bar(&t), 1);
        assert!(t.advance_bar(&bank).stop);
        assert!(!t.is_playing());
    }

    #[test]
    fn session_all_bars_repeat_loops() {
        let bank = sample_bank();
        let mut t = Transport::new();
        t.play_session(&bank, 0, None, true).unwrap();
        let mut bars = Vec::new();
        for _ in 0..5 {
            bars.push(bar(&t));
            assert!(!t.advance_bar(&bank).stop);
        }
        assert_eq!(bars, [0, 1, 0, 1, 0]);
    }

    #[test]
    fn session_single_bar_once_then_stops() {
        let bank = sample_bank();
        let mut t = Transport::new();
        t.play_session(&bank, 0, Some(1), false).unwrap();
        assert_eq!(bar(&t), 1);
        assert!(t.advance_bar(&bank).stop);
    }

    #[test]
    fn session_single_bar_repeat_loops_that_bar() {
        let bank = sample_bank();
        let mut t = Transport::new();
        t.play_session(&bank, 2, Some(1), true).unwrap();
        assert_eq!(t.meter(), TimeSig::new(7, 8));
        for _ in 0..3 {
            let ev = t.advance_bar(&bank);
            assert!(!ev.stop);
            assert_eq!(bar(&t), 1);
            assert_eq!(ev.meter, None); // 同じ小節なので拍子は変わらない
        }
    }

    #[test]
    fn toggle_change_during_playback_takes_effect_at_the_next_bar() {
        let bank = sample_bank();
        let mut t = Transport::new();
        t.play_session(&bank, 0, None, false).unwrap(); // 全小節・1 回
        t.advance_beat(&bank); // 小節の途中
        t.set_session_toggles(&bank, None, true).unwrap();
        assert_eq!(bar(&t), 0); // 今の小節は変わらない
        t.advance_bar(&bank);
        assert_eq!(bar(&t), 1);
        assert!(!t.advance_bar(&bank).stop); // 矢印 ON が効いて先頭へ戻る
        assert_eq!(bar(&t), 0);

        // `1` を ON(小節 1)・矢印 OFF: 次の境界で小節 1 へ移り、弾き終えたら停止
        t.set_session_toggles(&bank, Some(1), false).unwrap();
        assert_eq!(bar(&t), 0);
        t.advance_bar(&bank);
        assert_eq!(bar(&t), 1);
        assert!(t.advance_bar(&bank).stop);
    }

    #[test]
    fn toggles_are_validated() {
        let bank = sample_bank();
        let mut t = Transport::new();
        assert_eq!(t.set_session_toggles(&bank, None, true), Err(Error::NotPlaying));
        t.play_session(&bank, 0, None, false).unwrap();
        assert_eq!(t.set_session_toggles(&bank, Some(2), true), Err(Error::BarOutOfRange));
        t.stop();
        t.play_song(&bank, 0).unwrap();
        assert_eq!(t.set_session_toggles(&bank, None, true), Err(Error::WrongScope));
    }

    #[test]
    fn queued_jump_executes_at_the_next_bar_boundary() {
        let mut bank = sample_bank();
        bank.songs[0]
            .tempo_triggers
            .push(TempoTrigger { at: SongPos::new(1, 0, 0), bpm: 140 })
            .unwrap();
        let mut t = Transport::new();
        t.play_song(&bank, 0).unwrap();
        t.queue(&bank, QueuedAction::Jump(SongPos::new(2, 0, 1))).unwrap();
        assert_eq!(song_pos(&t), (0, 0, 0)); // 予約しただけでは動かない

        let ev = t.advance_bar(&bank);
        assert_eq!(song_pos(&t), (2, 0, 1));
        assert_eq!(ev.tempo, Some(140)); // 飛び先で有効テンポを再評価
        assert_eq!(ev.pc, pc(10, true)); // Session の枠が変わった
        assert_eq!(ev.meter, None);

        // 予約は 1 回で消え、以後は通常進行
        let ev = t.advance_bar(&bank);
        assert_eq!(song_pos(&t), (2, 1, 0));
        assert_eq!(ev.pc, pc(11, true));
        assert!(t.advance_bar(&bank).stop);
    }

    #[test]
    fn queued_jump_within_the_same_session_sends_no_pc() {
        let bank = sample_bank();
        let mut t = Transport::new();
        t.play_song(&bank, 0).unwrap();
        t.advance_bar(&bank); // (0,0,1)
        t.queue(&bank, QueuedAction::Jump(SongPos::new(0, 0, 0))).unwrap();
        let ev = t.advance_bar(&bank);
        assert_eq!(song_pos(&t), (0, 0, 0));
        assert_eq!(ev.pc, None);
    }

    #[test]
    fn queued_stop_and_jump_bar() {
        let mut bank = sample_bank();
        bank.set_session(Session::new(3, 13, 4, TimeSig::FOUR_FOUR)).unwrap();
        let mut t = Transport::new();
        t.play_session(&bank, 3, None, false).unwrap();
        assert_eq!(t.queue(&bank, QueuedAction::Jump(SongPos::START)), Err(Error::WrongScope));
        assert_eq!(t.queue(&bank, QueuedAction::JumpBar(4)), Err(Error::BarOutOfRange));
        t.queue(&bank, QueuedAction::JumpBar(3)).unwrap();
        t.advance_bar(&bank);
        assert_eq!(bar(&t), 3);

        t.queue(&bank, QueuedAction::Stop).unwrap();
        let ev = t.advance_bar(&bank);
        assert!(ev.stop && !t.is_playing());
        assert_eq!(t.queue(&bank, QueuedAction::Stop), Err(Error::NotPlaying));
    }

    #[test]
    fn pc_is_sent_at_every_session_boundary_of_a_shared_session() {
        let mut bank = sample_bank();
        // Session 1 を Chapter A と B の両方から参照する(B では 2 回続けて)
        let mut song = Song::new("Shared", 120);
        song.chapters.push(Chapter::new("A", &[1])).unwrap();
        song.chapters.push(Chapter::new("B", &[1, 1])).unwrap();
        song.arrangement.extend_from_slice(&[0, 1]).unwrap();
        bank.songs.push(song).unwrap();

        let mut t = Transport::new();
        assert_eq!(t.play_song(&bank, 1).unwrap().pc, pc(11, false));
        assert_eq!(t.advance_bar(&bank).pc, pc(11, true)); // A → B
        assert_eq!(t.advance_bar(&bank).pc, pc(11, true)); // B の中で同じ Session が続く
        let ev = t.advance_bar(&bank);
        assert!(ev.stop);
        assert_eq!(ev.pc, None);
    }

    #[test]
    fn session_scope_uses_the_transport_tempo() {
        let bank = sample_bank();
        let mut t = Transport::new();
        assert_eq!(t.bpm(), DEFAULT_BPM);
        t.set_bpm(90).unwrap();
        assert_eq!(t.play_session(&bank, 0, None, true).unwrap().tempo, Some(90));

        t.set_bpm(100).unwrap();
        assert_eq!(t.bpm(), 90); // 次の境界までは変わらない
        assert_eq!(t.advance_bar(&bank).tempo, Some(100));
        assert_eq!(t.advance_bar(&bank).tempo, None);
        t.stop();

        // Song ではテンポは Song が決め、終わった後はその値が「前回使用値」として残る
        t.set_bpm(80).unwrap();
        assert_eq!(t.play_song(&bank, 0).unwrap().tempo, Some(100));
        assert_eq!(t.set_bpm(60), Err(Error::WrongScope));
        t.stop();
        assert_eq!(t.bpm(), 100);
    }

    #[test]
    fn beats_roll_over_into_the_next_bar_by_the_meter() {
        let bank = sample_bank();
        let mut t = Transport::new();
        t.play_session(&bank, 1, None, true).unwrap(); // 3/4 x1 小節
        assert_eq!(t.advance_beat(&bank), None);
        assert_eq!(t.advance_beat(&bank), None);
        assert_eq!(t.position().unwrap().beat, 2);
        let ev = t.advance_beat(&bank).expect("3 拍で小節をまたぐ");
        assert!(!ev.stop);
        assert_eq!((bar(&t), t.position().unwrap().beat), (0, 0));

        t.stop();
        t.play_session(&bank, 2, Some(1), false).unwrap(); // 7/8
        for _ in 0..6 {
            assert_eq!(t.advance_beat(&bank), None);
        }
        assert!(t.advance_beat(&bank).unwrap().stop);
    }

    #[test]
    fn play_is_validated() {
        let mut bank = sample_bank();
        bank.songs.push(Song::new("Empty", 120)).unwrap();
        let mut t = Transport::new();
        assert_eq!(t.play_song(&bank, 9), Err(Error::NoSuchSong));
        assert_eq!(t.play_song(&bank, 1), Err(Error::EmptySong));
        assert_eq!(t.play_session(&bank, 9, None, false), Err(Error::NoSuchSession));
        assert_eq!(t.play_session(&bank, 0, Some(2), false), Err(Error::BarOutOfRange));
        t.play_session(&bank, 0, None, false).unwrap();
        assert_eq!(t.play_song(&bank, 0), Err(Error::AlreadyPlaying));
        assert!(t.stop().stop);
        assert_eq!(t.stop(), BarEvents::default());
    }

    #[test]
    fn program_change_bytes_for_sl_mk3() {
        assert_eq!(ProgramChange { program: 5, cue: false }.bytes(), [0xCF, 5]);
        assert_eq!(ProgramChange { program: 5, cue: true }.bytes(), [0xCF, 69]);
        assert_eq!(ProgramChange { program: 63, cue: true }.bytes(), [0xCF, 127]);
    }

    #[test]
    fn transport_size() {
        std::println!("size_of: Transport={}", core::mem::size_of::<Transport>());
    }
}
