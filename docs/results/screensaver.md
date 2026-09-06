# メニューのスクリーンセーバー / バックライト消灯(phase 外作業)

対応する指示書は無い(2026-09-06、ユーザー依頼の phase 外作業)。
きっかけは「実機をつけっぱなしにするとメニューが焼き付いて跡が残る」という現象。

## 決めた仕様(着手前にユーザー確認)

- **適用範囲はメニュー(ランチャー)表示中のみ**。WASM アプリ実行中は一切介入しない。
- 無操作 **60 秒**でスクリーンセーバー(黒地+ゆっくり漂う小さな図形)。
- 無操作 **180 秒**でバックライト消灯(同じ無操作タイマ基準)。
- 復帰はタッチ。**復帰のタップはアプリを起動しない**。電源キー短押しでも復帰する。

## 実装

| ファイル | 変更 |
|---|---|
| `src/components/display/display.{hpp,cpp}` | `display_backlight_set/is_on()` を追加(GPIO5 = `PIN_LCD_BL` を直接叩くだけ。`Display::init` 自身もこれを使う) |
| `src/components/wasm_runtime/screensaver.{hpp,cpp}` | 新規。状態機械(Active / Saver / Blank)と LVGL タイマ(200ms) |
| `src/components/wasm_runtime/launcher.cpp` | メニュー生成時に `screensaver_attach`、`launcher_show` で復帰、`launcher_launch_by_name`(シリアル `run`)で起動前に復帰 |
| `src/main/app_main.cpp` | 電源キー短押しで `screensaver_request_wake()` |
| `src/main/Kconfig.projbuild` | `MIDIBOX_SCREENSAVER`(既定 y)/ `MIDIBOX_SCREENSAVER_SEC`(60)/ `MIDIBOX_BACKLIGHT_OFF_SEC`(180) |

### 設計判断

- **無操作時間は LVGL の `lv_display_get_inactive_time()` を使う**。`touch.cpp` の
  indev は普通の pointer indev として登録済みで、押下のたびに LVGL が
  `last_activity_time` を更新する(`lv_indev.c`)。タッチ側にフックを増やさずに済む。
- **復帰タップの誤爆防止に全画面オーバーレイ**。黒地の全画面オブジェクトを
  `LV_OBJ_FLAG_CLICKABLE` でメニュー画面の最前面に置くので、復帰のタップは
  オーバーレイが食い、下のアプリ一覧のボタンには届かない。
- **LVGL のイベント CB 内でオブジェクトを消さない**。オーバーレイの
  `LV_EVENT_PRESSED` では点灯(GPIO)と atomic フラグの立て上げだけを行い、
  実際の破棄は次の tick(最大 200ms 後)に LVGL タスクで行う。
- **アプリ実行中は無効**。tick が `lv_screen_active() != メニュー画面` を見て、
  その場合は状態を Active に戻して何もしない。
- **バックライトは GPIO の ON/OFF のみ**でパネルは `disp_on` のまま。復帰時に
  再初期化が要らず、直前の描画がそのまま出る。
- **電源キー短押しは atomic フラグのみ**(小スタックの power_key タスクから
  LVGL を触らない、という既存の約束をそのまま守る)。
- 消灯段階では図形のアニメを削除して非表示にする(消灯中に無駄な再描画をしない)。

## 実機での確認(2026-09-06)

ビルド exit 0(警告なし)、フラッシュ exit 0。バイナリ 0xffcf0(app パーティション 4MB の 75% 空き)。

### 段階遷移(シリアルログ)

```
I (  1561) WASM/SAVER: armed: saver 60s, backlight off 180s
I ( 61771) WASM/SAVER: screensaver on (idle 60 s)
I (181701) WASM/SAVER: backlight off (idle 180 s)
I (192301) WASM/SAVER: wake                      ← 消灯中のタップ
I (194651) WASM/LAUNCH: launch: /sdcard/apps/mp3player.wasm  ← 2 回目のタップ
```

- 復帰のタップ(192.3s)では `WASM/LAUNCH: launch:` が出ていない。**オーバーレイが
  タップを食い、アプリを誤起動しない**ことをログとユーザーの目視の双方で確認。
- その 2.3 秒後の 2 回目のタップでは通常どおり起動する。
- 目視確認(ユーザー): 60 秒で黒地+漂う丸、180 秒で完全消灯、タップで即復帰。

### アプリ実行中に働かないこと

`run touch_demo` のまま 92 秒(3.5s → 95.2s)保持しても `screensaver on` は出ない。

### メモリ

| 状態 | free heap | largest block |
|---|---|---|
| 起動直後のメニュー | 66112 | 31744 |
| スクリーンセーバー表示中(63 秒経過時点) | 66112 | 31744 |
| 復帰後のメニュー | 65948 | 31744 |

- **スクリーンセーバーは ESP のヒープを消費しない**(LVGL オブジェクトは LVGL 側の
  プールから取られるため)。表示中でも 66112 のまま。
- 復帰後の 65948(-164 B)は**スクリーンセーバーとは無関係の既存挙動**。
  切り分け: 起動 4 秒後(セーバー発動前)に `run touch_demo` → `stop` した場合も
  66112 → 65948 になり、2 回目のサイクル後も 65948 のまま(累積しない一度きりの差)。
- 自動回帰 `scripts/device-regress.sh --task screensaver-regress`: **PASS**
  (6 本すべて free heap 差分 +0、largest block 31744、許容外 WARN/ERROR 0 件)。

## 残っている検討事項

- 消灯中も LVGL タスクとタッチのポーリングは動き続ける(復帰に必要)。
  消費電力の実測はしていない。バッテリ運用で効かせたいなら、消灯段階で
  タッチのポーリング周期を落とす/パネルを `disp_off` にする等の追加検討が要る。
- 時間の既定値は Kconfig(`MIDIBOX_SCREENSAVER_SEC` / `MIDIBOX_BACKLIGHT_OFF_SEC`)
  で変えられる。UI から設定する手段は用意していない。
