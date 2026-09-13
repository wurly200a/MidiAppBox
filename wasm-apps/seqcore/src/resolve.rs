// 解決規則(spec §3.2)。
//
// Song の文脈が無い再生(Session 画面からの単体再生)のテンポは
// Transport の現在テンポを使う。それは transport 側で扱う。

use crate::model::{Session, Song, SongPos, TimeSig};

/// 有効テンポ = pos 以前(pos 自身を含む)で最後の TempoTrigger の bpm。
/// 無ければ `Song.default_bpm`。
///
/// tempo_triggers は at 昇順が前提(spec §3)だが、並びに依存しないよう全件を見て
/// 「at <= pos のうち at が最大」を選ぶ。同じ at が複数あれば後ろにある方を採る。
/// SongPos の順序 = 曲の進行順なので、途中の Session から始めても
/// 「Song 先頭からその位置までを走査」した結果と一致する。
pub fn effective_tempo(song: &Song, pos: SongPos) -> u16 {
    let mut best: Option<(SongPos, u16)> = None;
    for t in song.tempo_triggers.iter() {
        if t.at <= pos && best.map_or(true, |(at, _)| t.at >= at) {
            best = Some((t.at, t.bpm));
        }
    }
    best.map_or(song.default_bpm, |(_, bpm)| bpm)
}

/// 有効拍子 = `Session.bar_meter[bar]` があればそれ、無ければ `Session.meter`。
/// 範囲外の小節は `Session.meter`。
pub fn effective_meter(session: &Session, bar: u8) -> TimeSig {
    session
        .bar_meter
        .get(bar as usize)
        .copied()
        .flatten()
        .unwrap_or(session.meter)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::tests::sample_bank;
    use crate::model::TempoTrigger;

    fn trig(arr: u8, sess: u8, bar: u8, bpm: u16) -> TempoTrigger {
        TempoTrigger { at: SongPos::new(arr, sess, bar), bpm }
    }

    #[test]
    fn tempo_without_triggers_is_the_song_default() {
        let bank = sample_bank();
        let song = &bank.songs[0];
        assert_eq!(effective_tempo(song, SongPos::START), 100);
        assert_eq!(effective_tempo(song, SongPos::new(2, 1, 0)), 100);
    }

    #[test]
    fn tempo_trigger_at_the_song_start_replaces_the_default() {
        let mut bank = sample_bank();
        bank.songs[0].tempo_triggers.push(trig(0, 0, 0, 132)).unwrap();
        let song = &bank.songs[0];
        assert_eq!(effective_tempo(song, SongPos::START), 132);
        assert_eq!(effective_tempo(song, SongPos::new(2, 1, 0)), 132);
    }

    #[test]
    fn tempo_trigger_midway_applies_from_its_own_bar() {
        let mut bank = sample_bank();
        // Chapter B(arr 1)の 2 小節目から 140
        bank.songs[0].tempo_triggers.push(trig(1, 0, 1, 140)).unwrap();
        let song = &bank.songs[0];
        assert_eq!(effective_tempo(song, SongPos::new(1, 0, 0)), 100);
        assert_eq!(effective_tempo(song, SongPos::new(1, 0, 1)), 140);
        assert_eq!(effective_tempo(song, SongPos::new(2, 0, 0)), 140);
    }

    #[test]
    fn tempo_from_a_middle_session_sees_every_earlier_trigger() {
        let mut bank = sample_bank();
        bank.songs[0]
            .tempo_triggers
            .extend_from_slice(&[trig(0, 1, 0, 90), trig(1, 0, 0, 120), trig(2, 1, 0, 150)])
            .unwrap();
        let song = &bank.songs[0];
        // 途中(2 回目の A の先頭)から始めても、手前の 2 つを越えた値になる
        assert_eq!(effective_tempo(song, SongPos::new(2, 0, 0)), 120);
        assert_eq!(effective_tempo(song, SongPos::new(0, 1, 0)), 90);
        assert_eq!(effective_tempo(song, SongPos::new(0, 0, 1)), 100);
        assert_eq!(effective_tempo(song, SongPos::new(2, 1, 0)), 150);
    }

    #[test]
    fn tempo_does_not_depend_on_trigger_order() {
        let mut bank = sample_bank();
        bank.songs[0]
            .tempo_triggers
            .extend_from_slice(&[trig(1, 0, 0, 120), trig(0, 1, 0, 90)])
            .unwrap();
        let song = &bank.songs[0];
        assert_eq!(effective_tempo(song, SongPos::new(2, 0, 0)), 120);
        assert_eq!(effective_tempo(song, SongPos::new(0, 1, 0)), 90);
    }

    #[test]
    fn meter_defaults_to_the_session_meter() {
        let bank = sample_bank();
        let s1 = bank.session(1).unwrap();
        assert_eq!(effective_meter(s1, 0), TimeSig::new(3, 4));
    }

    #[test]
    fn meter_bar_override_applies_only_to_that_bar() {
        let bank = sample_bank();
        let s2 = bank.session(2).unwrap(); // 4/4、2 小節目だけ 7/8
        assert_eq!(effective_meter(s2, 0), TimeSig::FOUR_FOUR);
        assert_eq!(effective_meter(s2, 1), TimeSig::new(7, 8));
        assert_eq!(effective_meter(s2, 200), TimeSig::FOUR_FOUR);
    }
}
