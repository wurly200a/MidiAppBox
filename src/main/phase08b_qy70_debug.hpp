#pragma once
// Phase 8b 追記(2026-08-15): QY70 での MIDI Clock 同期不成立の切り分け用
// 一時検証コード(使い捨て)。docs/prompts/phase08b_midi_clock_api.md 参照。
// Clock を一切送らず、Note On/Off の単発送信のみを行う。既存の
// hostapi_midi_send の実体(midi::Midi_Send)をそのまま使うだけで、
// Host API/ABI は変更しない。確認後にこのファイルごと削除すること。

void phase08b_qy70_debug_start();
