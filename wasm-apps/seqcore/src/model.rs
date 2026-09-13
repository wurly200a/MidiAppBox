// データモデル(spec §3)。
//
// 上限定数は Phase 16 のメモリ見積もりで確定する(docs/results/phase16.md)。
// すべて Copy・固定長で、`Bank::new()` は const(static に置ける)。

use crate::fixed::FixedVec;

pub type SessionId = u8; // 0..=MAX_SESSIONS-1
pub type ChapterIdx = u8; // Song 内インデックス

pub const MAX_SESSIONS: usize = 64; // SL MK3 の Session 数(PC 0..=63)
pub const MAX_BARS_PER_SESSION: usize = 16;
/// spec §3 は Chapter の参照列の容量に MAX_BARS_PER_SESSION を使っていたが、
/// 誤記とみなして独立させた(Phase 16 ステップ 0 の D5)
pub const MAX_SESSIONS_PER_CHAPTER: usize = 16;
pub const MAX_CHAPTERS_PER_SONG: usize = 16;
pub const MAX_ARRANGEMENT_LEN: usize = 32;
pub const MAX_TEMPO_TRIGGERS: usize = 16;
pub const MAX_SONGS: usize = 8;
pub const NAME_LEN: usize = 16;

/// 固定長の名前(UTF-8、NUL 詰め)
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Name([u8; NAME_LEN]);

impl Name {
    pub const EMPTY: Name = Name([0; NAME_LEN]);

    /// NAME_LEN バイトを超える分は UTF-8 の文字境界で切り詰める
    pub fn new(s: &str) -> Name {
        let mut end = s.len().min(NAME_LEN);
        while !s.is_char_boundary(end) {
            end -= 1;
        }
        let mut b = [0u8; NAME_LEN];
        b[..end].copy_from_slice(&s.as_bytes()[..end]);
        Name(b)
    }

    pub fn as_str(&self) -> &str {
        let n = self.0.iter().position(|&c| c == 0).unwrap_or(NAME_LEN);
        core::str::from_utf8(&self.0[..n]).unwrap_or("")
    }
}

impl core::fmt::Debug for Name {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{:?}", self.as_str())
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TimeSig {
    pub num: u8,
    pub den: u8,
}

impl TimeSig {
    pub const FOUR_FOUR: TimeSig = TimeSig::new(4, 4);

    pub const fn new(num: u8, den: u8) -> TimeSig {
        TimeSig { num, den }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Session {
    pub id: SessionId,
    pub name: Name,
    /// 送信する PC 番号 = SL MK3 の Session 番号(0..=63)。
    /// キュー(パターン末尾で切替)にするときは送信時に PC_CUE_OFFSET を足す
    pub program: u8,
    pub bars: u8, // 1..=MAX_BARS_PER_SESSION
    pub meter: TimeSig,
    pub bar_meter: [Option<TimeSig>; MAX_BARS_PER_SESSION],
}

impl Session {
    pub const fn new(id: SessionId, program: u8, bars: u8, meter: TimeSig) -> Session {
        Session {
            id,
            name: Name::EMPTY,
            program,
            bars,
            meter,
            bar_meter: [None; MAX_BARS_PER_SESSION],
        }
    }

    pub fn with_name(mut self, name: &str) -> Session {
        self.name = Name::new(name);
        self
    }

    /// 範囲外の小節は無視する
    pub fn with_bar_meter(mut self, bar: u8, meter: TimeSig) -> Session {
        if let Some(m) = self.bar_meter.get_mut(bar as usize) {
            *m = Some(meter);
        }
        self
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Chapter {
    pub name: Name,
    pub sessions: FixedVec<SessionId, MAX_SESSIONS_PER_CHAPTER>, // 参照列
}

impl Chapter {
    pub const EMPTY: Chapter = Chapter {
        name: Name::EMPTY,
        sessions: FixedVec::new(0),
    };

    /// 容量を超える参照は切り捨てる
    pub fn new(name: &str, sessions: &[SessionId]) -> Chapter {
        let mut c = Chapter::EMPTY;
        c.name = Name::new(name);
        for &id in sessions {
            if c.sessions.push(id).is_err() {
                break;
            }
        }
        c
    }
}

/// Song 内の絶対位置(arrangement 上の位置)。導出順 = 曲の進行順
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct SongPos {
    pub arr_idx: u8,  // arrangement 内インデックス
    pub sess_idx: u8, // その Chapter 内の Session インデックス
    pub bar: u8,      // その Session 内の小節
}

impl SongPos {
    pub const START: SongPos = SongPos::new(0, 0, 0);

    pub const fn new(arr_idx: u8, sess_idx: u8, bar: u8) -> SongPos {
        SongPos { arr_idx, sess_idx, bar }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TempoTrigger {
    pub at: SongPos,
    pub bpm: u16,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Song {
    pub name: Name,
    pub default_bpm: u16,
    pub chapters: FixedVec<Chapter, MAX_CHAPTERS_PER_SONG>,
    pub arrangement: FixedVec<ChapterIdx, MAX_ARRANGEMENT_LEN>,
    pub tempo_triggers: FixedVec<TempoTrigger, MAX_TEMPO_TRIGGERS>, // at 昇順
}

impl Song {
    /// 全ビット 0(FixedVec の埋め値に使う)。そのまま再生できる値ではない
    pub const EMPTY: Song = Song {
        name: Name::EMPTY,
        default_bpm: 0,
        chapters: FixedVec::new(Chapter::EMPTY),
        arrangement: FixedVec::new(0),
        tempo_triggers: FixedVec::new(TempoTrigger {
            at: SongPos::START,
            bpm: 0,
        }),
    };

    pub fn new(name: &str, default_bpm: u16) -> Song {
        let mut s = Song::EMPTY;
        s.name = Name::new(name);
        s.default_bpm = default_bpm;
        s
    }

    /// pos が指す Session。arrangement / Chapter / Bank のどこかで参照が切れているか、
    /// bar が Session の小節数を超えていれば None
    pub fn session_at<'b>(&self, bank: &'b Bank, pos: SongPos) -> Option<&'b Session> {
        let s = self.slot(bank, pos.arr_idx, pos.sess_idx)?;
        (pos.bar < s.bars).then_some(s)
    }

    /// 最初に再生できる位置(参照切れ・0 小節の Session は飛ばす)
    pub fn first_pos(&self, bank: &Bank) -> Option<SongPos> {
        self.next_slot(bank, 0, 0)
    }

    /// pos の次の小節の位置。曲の終端なら None
    pub fn next_pos(&self, bank: &Bank, pos: SongPos) -> Option<SongPos> {
        if let Some(s) = self.session_at(bank, pos) {
            if pos.bar + 1 < s.bars {
                return Some(SongPos { bar: pos.bar + 1, ..pos });
            }
        }
        if pos.sess_idx < u8::MAX {
            self.next_slot(bank, pos.arr_idx, pos.sess_idx + 1)
        } else {
            self.next_slot(bank, pos.arr_idx.saturating_add(1), 0)
        }
    }

    fn slot<'b>(&self, bank: &'b Bank, arr_idx: u8, sess_idx: u8) -> Option<&'b Session> {
        let ch = *self.arrangement.get(arr_idx as usize)?;
        let id = *self.chapters.get(ch as usize)?.sessions.get(sess_idx as usize)?;
        bank.session(id).filter(|s| s.bars > 0)
    }

    /// (arr_idx, sess_idx) 以降で最初に再生できる Session の先頭小節
    fn next_slot(&self, bank: &Bank, arr_idx: u8, sess_idx: u8) -> Option<SongPos> {
        let mut sess = sess_idx as usize;
        for arr in arr_idx as usize..self.arrangement.len() {
            let ch = self.arrangement[arr] as usize;
            let n = self.chapters.get(ch).map_or(0, |c| c.sessions.len());
            while sess < n {
                if self.slot(bank, arr as u8, sess as u8).is_some() {
                    return Some(SongPos::new(arr as u8, sess as u8, 0));
                }
                sess += 1;
            }
            sess = 0;
        }
        None
    }
}

/// 装置全体の保持データ。Session は id を添字にして置く。
///
/// spec §3 は `[Option<Session>; MAX_SESSIONS]` だが、**`bars == 0` を空きとする**
/// 配列にした。`Option<Session>` は niche 最適化で None が非ゼロのビット列になり
/// (実測: 64 スロットで 64 バイト)、`static` の Bank が .bss ではなく .data に
/// 載って .wasm が約 9.8KB 太るため(docs/results/phase16.md「仕様からの逸脱」)。
/// 読み出しは `session()` が Option で返すので、呼び出し側の見え方は同じ。
pub struct Bank {
    pub sessions: [Session; MAX_SESSIONS],
    pub songs: FixedVec<Song, MAX_SONGS>,
}

impl Bank {
    pub const fn new() -> Bank {
        Bank {
            sessions: [Session::new(0, 0, 0, TimeSig::new(0, 0)); MAX_SESSIONS],
            songs: FixedVec::new(Song::EMPTY),
        }
    }

    /// 空き(bars == 0)なら None
    pub fn session(&self, id: SessionId) -> Option<&Session> {
        self.sessions.get(id as usize).filter(|s| s.bars > 0)
    }

    /// id が範囲外なら Session をそのまま返す。bars == 0 を渡すと空きに戻る
    pub fn set_session(&mut self, s: Session) -> Result<(), Session> {
        match self.sessions.get_mut(s.id as usize) {
            Some(slot) => {
                *slot = s;
                Ok(())
            }
            None => Err(s),
        }
    }
}

impl Default for Bank {
    fn default() -> Bank {
        Bank::new()
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use std::boxed::Box;

    /// テスト用の Bank。Session 0 = 4/4 x2 小節、1 = 3/4 x1 小節、2 = 4/4 x2 小節
    /// (2 小節目だけ 7/8)。Song 0 = Chapter A[0,1] / B[2]、arrangement A B A、100bpm
    pub fn sample_bank() -> Box<Bank> {
        let mut bank = Box::new(Bank::new());
        bank.set_session(Session::new(0, 10, 2, TimeSig::FOUR_FOUR)).unwrap();
        bank.set_session(Session::new(1, 11, 1, TimeSig::new(3, 4))).unwrap();
        bank.set_session(
            Session::new(2, 12, 2, TimeSig::FOUR_FOUR).with_bar_meter(1, TimeSig::new(7, 8)),
        )
        .unwrap();
        let mut song = Song::new("Hello", 100);
        song.chapters.push(Chapter::new("A", &[0, 1])).unwrap();
        song.chapters.push(Chapter::new("B", &[2])).unwrap();
        song.arrangement.extend_from_slice(&[0, 1, 0]).unwrap();
        bank.songs.push(song).unwrap();
        bank
    }

    #[test]
    fn name_truncates_at_a_char_boundary() {
        assert_eq!(Name::new("Intro").as_str(), "Intro");
        // "サビ" x3 = 18 バイト。16 バイトに収まる 5 文字(15 バイト)で切る
        assert_eq!(Name::new("サビサビサビ").as_str(), "サビサビサ");
    }

    #[test]
    fn session_lookup_by_id() {
        let bank = sample_bank();
        assert_eq!(bank.session(1).map(|s| s.program), Some(11));
        assert!(bank.session(3).is_none());
        assert!(bank.session(200).is_none());
        let mut b = Bank::new();
        assert!(b.set_session(Session::new(64, 0, 1, TimeSig::FOUR_FOUR)).is_err());
    }

    #[test]
    fn song_walks_every_bar_in_arrangement_order() {
        let bank = sample_bank();
        let song = &bank.songs[0];
        let mut pos = song.first_pos(&bank);
        let mut seen = std::vec::Vec::new();
        while let Some(p) = pos {
            seen.push((p.arr_idx, p.sess_idx, p.bar));
            pos = song.next_pos(&bank, p);
        }
        assert_eq!(
            seen,
            [(0, 0, 0), (0, 0, 1), (0, 1, 0), (1, 0, 0), (1, 0, 1), (2, 0, 0), (2, 0, 1), (2, 1, 0)]
        );
    }

    #[test]
    fn broken_references_are_skipped() {
        let mut bank = sample_bank();
        // Session 9 は Bank に無い / Chapter 5 は Song に無い
        bank.songs[0].chapters[0] = Chapter::new("A", &[9, 1]);
        bank.songs[0].arrangement.push(5).unwrap();
        let song = &bank.songs[0];
        assert_eq!(song.first_pos(&bank), Some(SongPos::new(0, 1, 0)));
        assert_eq!(song.next_pos(&bank, SongPos::new(2, 1, 0)), None);
    }

    #[test]
    fn layout_sizes_for_the_memory_estimate() {
        use core::mem::size_of;
        std::println!(
            "size_of: Name={} TimeSig={} Option<TimeSig>={} Session={} Option<Session>={} \
             Chapter={} TempoTrigger={} Song={} Bank={}",
            size_of::<Name>(),
            size_of::<TimeSig>(),
            size_of::<Option<TimeSig>>(),
            size_of::<Session>(),
            size_of::<Option<Session>>(),
            size_of::<Chapter>(),
            size_of::<TempoTrigger>(),
            size_of::<Song>(),
            size_of::<Bank>(),
        );
        // 定数をうっかり増やして Bank が膨らんだら気づけるようにする
        // (見積もりと判断は docs/results/phase16.md「メモリ見積もり」)
        assert!(size_of::<Bank>() <= 16 * 1024);
    }

    #[test]
    fn empty_bank_is_all_zero_bits() {
        // static に置いたとき .bss に落ちるか(.wasm の .data を太らせないか)
        let bank = Bank::new();
        let bytes = unsafe {
            core::slice::from_raw_parts(&bank as *const Bank as *const u8, core::mem::size_of::<Bank>())
        };
        let nonzero = bytes.iter().filter(|&&b| b != 0).count();
        std::println!("Bank::new(): {} non-zero bytes of {}", nonzero, bytes.len());
        assert_eq!(nonzero, 0);
    }
}
