// 固定容量の可変長列(spec §3 の heapless::Vec の代わり)。
//
// 依存 crate を足さないために自前で持つ(Phase 16 ステップ 0 で承認)。
// 長さを u8 で持つので、ホスト(x86_64)と wasm32 でレイアウトが一致し、
// メモリ見積もりをホストのテストで取れる。

use core::ops::{Deref, DerefMut};

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct FixedVec<T: Copy, const N: usize> {
    buf: [T; N],
    len: u8,
}

impl<T: Copy, const N: usize> FixedVec<T, N> {
    /// `fill` は未使用領域の埋め値。static に置いたとき .bss に落ちるよう、
    /// 全ビット 0 になる値を渡すこと(非ゼロだと .wasm の .data が太る)。
    pub const fn new(fill: T) -> Self {
        assert!(N <= u8::MAX as usize);
        Self { buf: [fill; N], len: 0 }
    }

    pub const fn capacity(&self) -> usize {
        N
    }

    /// 満杯なら値をそのまま返す
    pub fn push(&mut self, v: T) -> Result<(), T> {
        let i = self.len as usize;
        if i >= N {
            return Err(v);
        }
        self.buf[i] = v;
        self.len += 1;
        Ok(())
    }

    /// 入り切らなければ何も足さずに Err
    pub fn extend_from_slice(&mut self, items: &[T]) -> Result<(), ()> {
        if self.len as usize + items.len() > N {
            return Err(());
        }
        for &v in items {
            let _ = self.push(v);
        }
        Ok(())
    }

    pub fn pop(&mut self) -> Option<T> {
        if self.len == 0 {
            return None;
        }
        self.len -= 1;
        Some(self.buf[self.len as usize])
    }

    pub fn clear(&mut self) {
        self.len = 0;
    }
}

impl<T: Copy, const N: usize> Deref for FixedVec<T, N> {
    type Target = [T];
    fn deref(&self) -> &[T] {
        &self.buf[..self.len as usize]
    }
}

impl<T: Copy, const N: usize> DerefMut for FixedVec<T, N> {
    fn deref_mut(&mut self) -> &mut [T] {
        let n = self.len as usize;
        &mut self.buf[..n]
    }
}

impl<T: Copy + core::fmt::Debug, const N: usize> core::fmt::Debug for FixedVec<T, N> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_list().entries(self.iter()).finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn push_until_full_then_rejects_the_value() {
        let mut v: FixedVec<u8, 3> = FixedVec::new(0);
        assert!(v.is_empty());
        for i in 1..=3 {
            assert_eq!(v.push(i), Ok(()));
        }
        assert_eq!(v.push(9), Err(9));
        assert_eq!(&v[..], &[1, 2, 3]);
    }

    #[test]
    fn extend_is_all_or_nothing() {
        let mut v: FixedVec<u8, 3> = FixedVec::new(0);
        v.extend_from_slice(&[1, 2]).unwrap();
        assert_eq!(v.extend_from_slice(&[3, 4]), Err(()));
        assert_eq!(&v[..], &[1, 2]);
    }

    #[test]
    fn pop_and_clear() {
        let mut v: FixedVec<u8, 4> = FixedVec::new(0);
        v.extend_from_slice(&[1, 2]).unwrap();
        assert_eq!(v.pop(), Some(2));
        v.clear();
        assert_eq!(v.pop(), None);
    }

    #[test]
    fn layout_is_the_same_on_every_target() {
        // len が u8 なので usize の幅(host 8 / wasm32 4)に左右されない
        assert_eq!(core::mem::size_of::<FixedVec<u8, 16>>(), 17);
    }
}
