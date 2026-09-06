// メニュー(ランチャー)表示中のスクリーンセーバー / バックライト消灯。
//
// 目的は焼き付き(残像)防止。一定時間タッチが無ければ画面を黒地+ゆっくり動く
// ドットに切り替え、さらに時間が経てばバックライトを落とす。復帰はタッチ
// (または電源キー短押し)。復帰時のタップは全画面オーバーレイが食うので、
// メニューのボタンに誤ヒットしてアプリが起動することはない。
//
// WASM アプリ実行中は一切介入しない(アクティブスクリーンがメニューのときだけ
// タイマが働く)。
#pragma once

#include "lvgl.h"

namespace wasmrt {

// メニュー画面に監視タイマを取り付ける(2 回目以降の呼び出しは画面の差し替え)。
// lvgl_port_lock を保持した状態で呼ぶこと。
void screensaver_attach(lv_obj_t* menu_screen);

// 即時復帰(オーバーレイ破棄+バックライト ON+無操作タイマのリセット)。
// lvgl_port_lock を保持していない文脈から呼ぶ版。
void screensaver_wake();

// 同上。lvgl_port_lock を保持している文脈(LVGL タスク、launcher_show 内など)用。
void screensaver_wake_locked();

// 復帰を「要求」するだけ。LVGL に触らないので、電源キータスクのような
// 小スタック・非 LVGL 文脈から呼んでよい(次の tick で反映される)。
void screensaver_request_wake();

} // namespace wasmrt
