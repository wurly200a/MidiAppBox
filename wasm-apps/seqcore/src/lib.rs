// Sequencer app のコア。Host API に依存しない(Phase 16)。
//
// - 仕様: docs/apps/sequencer/spec.md
// - 記録: docs/results/phase16.md
//
// 画面・MIDI 送信・Host API 呼び出しは持たない。曲構造の解決と Transport の
// 状態遷移だけを行い、「境界で何をすべきか」を値として返す。
#![cfg_attr(not(any(test, feature = "std")), no_std)]

pub mod fixed;
pub mod model;
pub mod resolve;
pub mod transport;

pub use fixed::FixedVec;
pub use model::*;
pub use resolve::{effective_meter, effective_tempo};
pub use transport::*;
