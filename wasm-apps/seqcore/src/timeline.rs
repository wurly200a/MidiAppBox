// 時間軸の計画(Phase 18、docs/results/phase18.md ステップ 0 の 0-3)。
//
// song tick は再生開始で 0 から単調に進むタイムラインとして使う(locate / loop は
// 使わない)。先読みは「今鳴っている小節の次の 1 小節」だけにする。次の小節の
// 開始 tick は今の小節の長さで決まり、トグルやジャンプでは動かないので、
// 積み直しは「その tick 以降の flush + 同じ at_tick へのテンポ / 拍子の上書き」で済む。
//
// このモジュールは Host API を呼ばない。アプリは BarPlan を seq_write /
// tempomap_* に写すだけにする。

use crate::model::{Bank, SessionId, TimeSig};
use crate::transport::{BarEvents, Error, Position, QueuedAction, Transport};

/// 内部 PPQN(shared/hostapi_defs.h の HOSTAPI_PPQN と同じ)
pub const PPQN: u32 = 960;

/// 1 拍(拍子の分母の音符)の tick 数。分母 0 は 4 として扱う
pub const fn ticks_per_beat(m: TimeSig) -> u32 {
    let den = if m.den == 0 { 4 } else { m.den as u32 };
    PPQN * 4 / den
}

/// 1 小節の tick 数
pub const fn ticks_per_bar(m: TimeSig) -> u32 {
    ticks_per_beat(m) * m.num as u32
}

/// 1 小節ぶんの計画
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct BarPlan {
    /// この小節の頭の song tick
    pub start_tick: u32,
    pub meter: TimeSig,
    pub bpm: u16,
    pub pos: Position,
    /// この小節の頭で送るもの(Start / PC / テンポ / 拍子)
    pub events: BarEvents,
    /// この小節の終わりで止まる(アプリは end_tick に OP_STOP を積む)
    pub stop_at_end: bool,
}

impl BarPlan {
    pub fn end_tick(&self) -> u32 {
        self.start_tick + ticks_per_bar(self.meter)
    }

    /// 拍 i の song tick
    pub fn beat_tick(&self, i: u8) -> u32 {
        self.start_tick + ticks_per_beat(self.meter) * i as u32
    }

    pub fn session(&self) -> SessionId {
        self.pos.session
    }
}

/// 再生中に受け付ける変更
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Change {
    Toggles { single_bar: Option<u8>, repeat: bool },
    JumpBar(u8),
    Bpm(u16),
}

/// 今の小節と次の小節を持つ計画器
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Planner {
    /// 今の小節を鳴らしている状態の Transport
    live: Transport,
    current: BarPlan,
    /// 積んである次の小節と、その小節に入った状態の Transport。None なら今の小節の終わりで止まる
    next: Option<(BarPlan, Transport)>,
}

impl Planner {
    /// 再生開始。`live` は play_session / play_song を済ませた Transport、`events` はその戻り値
    pub fn begin(bank: &Bank, live: Transport, events: BarEvents) -> Option<Planner> {
        let pos = live.position()?;
        let current = BarPlan {
            start_tick: 0,
            meter: live.meter(),
            bpm: live.bpm(),
            pos,
            events,
            stop_at_end: false,
        };
        let mut p = Planner { live, current, next: None };
        p.replan(bank);
        Some(p)
    }

    pub fn current(&self) -> &BarPlan {
        &self.current
    }

    pub fn next(&self) -> Option<&BarPlan> {
        self.next.as_ref().map(|(plan, _)| plan)
    }

    /// 今の小節を鳴らしている状態(トグルや現在テンポの参照用)
    pub fn live(&self) -> &Transport {
        &self.live
    }

    /// song tick が次の小節の頭に達していたら、次の小節へ進める。
    /// 進めたら true(アプリは新しい next を積む)。遅れて複数小節ぶん進んでいても 1 小節ずつ進むので、
    /// 呼び出し側は false になるまで繰り返す
    pub fn advance_to(&mut self, bank: &Bank, song_tick: u32) -> bool {
        match self.next {
            Some((plan, state)) if song_tick >= plan.start_tick => {
                self.live = state;
                self.current = plan;
                self.replan(bank);
                true
            }
            _ => false,
        }
    }

    /// 変更を受け付ける。
    /// - `in_time` = 次の小節の頭まで締め切り(アプリが決める GUARD)以上ある:
    ///   次の小節を作り直す。アプリは次の小節の頭以降を flush して積み直す
    /// - そうでない: 積んである次の小節はそのまま鳴り、その次の小節から効く
    ///
    /// 今の小節の終わりで止まる予定のまま締め切りを過ぎた場合、停止は取り消せない
    pub fn apply(&mut self, bank: &Bank, change: Change, in_time: bool) -> Result<(), Error> {
        if in_time {
            apply_to(&mut self.live, bank, change)?;
            self.replan(bank);
            return Ok(());
        }
        match &mut self.next {
            Some((_, state)) => apply_to(state, bank, change),
            None => apply_to(&mut self.live, bank, change),
        }
    }

    /// 次の小節を live から作り直す
    fn replan(&mut self, bank: &Bank) {
        let mut peek = self.live;
        let ev = peek.advance_bar(bank);
        match peek.position() {
            Some(pos) if !ev.stop => {
                self.current.stop_at_end = false;
                let plan = BarPlan {
                    start_tick: self.current.end_tick(),
                    meter: peek.meter(),
                    bpm: peek.bpm(),
                    pos,
                    events: ev,
                    stop_at_end: false,
                };
                self.next = Some((plan, peek));
            }
            _ => {
                self.current.stop_at_end = true;
                self.next = None;
            }
        }
    }
}

fn apply_to(t: &mut Transport, bank: &Bank, change: Change) -> Result<(), Error> {
    match change {
        Change::Toggles { single_bar, repeat } => t.set_session_toggles(bank, single_bar, repeat),
        Change::JumpBar(bar) => t.queue(bank, QueuedAction::JumpBar(bar)),
        Change::Bpm(bpm) => t.set_bpm(bpm),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::tests::sample_bank;
    use crate::model::Session;
    use std::vec::Vec;

    fn begin_session(bank: &Bank, id: SessionId, single: Option<u8>, repeat: bool) -> Planner {
        let mut t = Transport::new();
        let ev = t.play_session(bank, id, single, repeat).unwrap();
        Planner::begin(bank, t, ev).unwrap()
    }

    /// 自然に止まるか limit 小節に達するまで進め、(小節番号, 開始 tick) の列を返す
    fn run(bank: &Bank, p: &mut Planner, limit: usize) -> Vec<(u8, u32)> {
        let mut seen = std::vec![(p.current().pos.bar, p.current().start_tick)];
        while seen.len() < limit {
            let Some(n) = p.next().copied() else { break };
            assert!(p.advance_to(bank, n.start_tick));
            seen.push((p.current().pos.bar, p.current().start_tick));
        }
        seen
    }

    #[test]
    fn tick_math_per_meter() {
        assert_eq!(ticks_per_bar(TimeSig::FOUR_FOUR), 3840);
        assert_eq!(ticks_per_bar(TimeSig::new(3, 4)), 2880);
        assert_eq!(ticks_per_bar(TimeSig::new(7, 8)), 3360);
        assert_eq!(ticks_per_bar(TimeSig::new(2, 4)), 1920);
        assert_eq!(ticks_per_bar(TimeSig::new(5, 4)), 4800);
        assert_eq!(ticks_per_beat(TimeSig::new(7, 8)), 480);
    }

    #[test]
    fn all_bars_once_ends_after_the_last_bar() {
        let bank = sample_bank(); // Session 2 = 4/4, 2 小節目だけ 7/8
        let mut p = begin_session(&bank, 2, None, false);
        assert_eq!(run(&bank, &mut p, 10), [(0, 0), (1, 3840)]);
        assert!(p.current().stop_at_end);
        assert_eq!(p.current().end_tick(), 3840 + 3360);
        assert_eq!(p.current().meter, TimeSig::new(7, 8));
        assert!(!p.advance_to(&bank, 100_000));
    }

    #[test]
    fn all_bars_repeat_keeps_start_ticks_contiguous() {
        let bank = sample_bank();
        let mut p = begin_session(&bank, 2, None, true);
        assert_eq!(run(&bank, &mut p, 5), [(0, 0), (1, 3840), (0, 7200), (1, 11040), (0, 14400)]);
    }

    #[test]
    fn single_bar_once_and_repeat() {
        let bank = sample_bank();
        let mut once = begin_session(&bank, 2, Some(1), false);
        assert_eq!(run(&bank, &mut once, 10), [(1, 0)]);
        assert!(once.current().stop_at_end);
        let mut rep = begin_session(&bank, 2, Some(1), true);
        assert_eq!(run(&bank, &mut rep, 3), [(1, 0), (1, 3360), (1, 6720)]);
    }

    #[test]
    fn beats_and_events_of_a_bar() {
        let bank = sample_bank();
        let p = begin_session(&bank, 2, None, false);
        assert!(p.current().events.start);
        let n = p.next().unwrap();
        assert_eq!(n.events.meter, Some(TimeSig::new(7, 8)));
        assert_eq!(n.beat_tick(0), 3840);
        assert_eq!(n.beat_tick(6), 3840 + 6 * 480);
    }

    #[test]
    fn change_in_time_takes_effect_at_the_next_boundary() {
        let bank = sample_bank();
        let mut p = begin_session(&bank, 0, None, false); // 4/4 x2、1 回
        let mut peek = p;
        peek.advance_to(&bank, 3840);
        assert!(peek.current().stop_at_end); // 変更しなければ 2 小節目で止まる

        p.advance_to(&bank, 3840); // 2 小節目(最後)
        assert!(p.current().stop_at_end);
        p.apply(&bank, Change::Toggles { single_bar: None, repeat: true }, true).unwrap();
        assert!(!p.current().stop_at_end); // 停止が取り消され、先頭へ戻る計画になった
        assert_eq!(p.next().unwrap().pos.bar, 0);
        assert_eq!(p.next().unwrap().start_tick, 7680);
    }

    #[test]
    fn change_too_late_takes_effect_one_boundary_later() {
        let mut bank = sample_bank();
        bank.set_session(Session::new(3, 13, 4, TimeSig::FOUR_FOUR)).unwrap();
        let mut p = begin_session(&bank, 3, None, true);
        // 小節 0 の途中で締め切りを過ぎてから「小節 2 だけ繰り返し」に変える
        p.apply(&bank, Change::Toggles { single_bar: Some(2), repeat: true }, false).unwrap();
        assert_eq!(p.next().unwrap().pos.bar, 1); // 積んである小節 1 はそのまま
        assert!(p.advance_to(&bank, 3840));
        assert_eq!(p.next().unwrap().pos.bar, 2); // その次から効く
        assert!(p.advance_to(&bank, 7680));
        assert_eq!(p.next().unwrap().pos.bar, 2);
    }

    #[test]
    fn jump_in_time_and_too_late() {
        let mut bank = sample_bank();
        bank.set_session(Session::new(3, 13, 4, TimeSig::FOUR_FOUR)).unwrap();
        let mut p = begin_session(&bank, 3, None, false);
        p.apply(&bank, Change::JumpBar(3), true).unwrap();
        assert_eq!(p.next().unwrap().pos.bar, 3);
        assert!(p.advance_to(&bank, 3840));
        assert!(p.current().stop_at_end); // 最後の小節へ飛んだので、そこで止まる

        let mut q = begin_session(&bank, 3, None, false);
        q.apply(&bank, Change::JumpBar(3), false).unwrap();
        assert_eq!(q.next().unwrap().pos.bar, 1);
        assert!(q.advance_to(&bank, 3840));
        assert_eq!(q.next().unwrap().pos.bar, 3);
    }

    #[test]
    fn bpm_change_in_time_and_too_late() {
        let bank = sample_bank();
        let mut p = begin_session(&bank, 2, None, true);
        assert_eq!(p.current().bpm, 120);
        p.apply(&bank, Change::Bpm(90), true).unwrap();
        assert_eq!(p.next().unwrap().bpm, 90);

        let mut q = begin_session(&bank, 2, None, true);
        q.apply(&bank, Change::Bpm(90), false).unwrap();
        assert_eq!(q.next().unwrap().bpm, 120);
        assert!(q.advance_to(&bank, 3840));
        assert_eq!(q.next().unwrap().bpm, 90);
    }

    #[test]
    fn advance_to_is_a_no_op_before_the_next_bar() {
        let bank = sample_bank();
        let mut p = begin_session(&bank, 2, None, true);
        assert!(!p.advance_to(&bank, 3839));
        assert_eq!(p.current().pos.bar, 0);
    }
}
