# ESP-IDF 6.0.2 移行調査メモ (2026-07-19)

## 背景

Docker ビルドコンテナを `esp-idf-v5.5` から `esp-idf-v6.0.2` に切り替えて
ビルドを試みた(ユーザーによる試験的切り替え)。段階的にエラーを解消したが、
最終的に WAMR 2.4.0(registry 固定バージョン)側の非互換で行き詰まったため、
本試行は中断し `esp-idf-v5.5` に復帰した。本メモはその調査記録であり、
将来 WAMR コンポーネントが IDF 6 対応版を出した際の再挑戦に備える。

**現状(このメモの時点)**: `src/components/*` への変更はすべて revert 済み、
`src/managed_components/` はローカルパッチごと削除して IDF 5.5.1 で再取得済み。
つまり本メモに記載する修正はどれもリポジトリには残っていない
(managed_components はそもそも .gitignore 対象)。

## 発生順に見た問題と修正の一覧(7 箇所)

| # | 箇所 | 症状 | 原因 | 対応 |
|---|---|---|---|---|
| 1 | `build/bootloader/CMakeCache.txt`(ビルドキャッシュ) | `CMAKE_C_COMPILER` が存在しない `esp-14.2.0_20241119` を指す | `ninja clean` はビルド成果物のみ削除しCMakeキャッシュを残す。IDF 5.5→6.0.2 切替前の構成が bootloader サブプロジェクトのキャッシュに残存 | `idf.py fullclean`(`build/` ごと削除)してから再構成 |
| 2 | `components/board/CMakeLists.txt` | `driver/gpio.h: No such file or directory` | IDF 6.0 で `driver` コンポーネントが分割され、`esp_driver_gpio` 等は `driver` の `PRIV_REQUIRES`(非公開)になり透過的に見えなくなった | `REQUIRES` に `esp_driver_gpio esp_driver_i2c` を追加 |
| 3 | `components/display/CMakeLists.txt` | 同上(`driver/spi_master.h`, `driver/gpio.h`) | 同上 | `REQUIRES` に `esp_driver_spi esp_driver_gpio` を追加 |
| 4 | `components/audio/CMakeLists.txt` | 同上(`driver/i2s_std.h`, `driver/gpio.h`) | 同上 | `REQUIRES` に `esp_driver_i2s esp_driver_gpio` を追加 |
| 5 | `components/power_key/CMakeLists.txt` | 同上(`driver/gpio.h`) | 同上 | `REQUIRES` に `esp_driver_gpio` を追加 |
| 6 | `components/storage/CMakeLists.txt` | 同上(`driver/spi_master.h`, `driver/gpio.h`, `driver/sdspi_host.h`, `driver/sdmmc_host.h`) | 同上 | `REQUIRES` に `esp_driver_spi esp_driver_gpio esp_driver_sdmmc esp_driver_sdspi` を追加 |
| 7 | `components/display/display.cpp` | `esp_lcd_panel_dev_config_t` に `color_space` メンバがない | IDF 6.0 で `esp_lcd_panel_dev_config_t.color_space`(`ESP_LCD_COLOR_SPACE_RGB`)が `rgb_ele_order`(`lcd_rgb_element_order_t`, 値 `LCD_RGB_ELEMENT_ORDER_RGB`)に置換された(`esp_lcd_panel_dev.h`) | フィールド名・型・enum 値を新 API に合わせて書き換え |
| 8 | `components/display/display.cpp` | `lvgl_port_cfg_t` の `-Werror=missing-field-initializers` | `lvgl_port_cfg_t` に `task_max_sleep_ms` / `task_stack_caps` フィールドが追加された(esp_lvgl_port)。デザイン初期化子リストで一部フィールドを省略すると GCC 15 では error 昇格 | `ESP_LVGL_PORT_INIT_CONFIG()` の既定値(`task_max_sleep_ms=500`, `task_stack_caps=MALLOC_CAP_INTERNAL\|MALLOC_CAP_DEFAULT`)を明示 |
| 9 | `managed_components/chmorgan__esp-audio-player/CMakeLists.txt` | `driver/i2s_std.h: No such file or directory`(audio_player.h 経由) | 同じ driver 分割問題がベンダー依存コンポーネントにも波及 | `REQUIRES` に `esp_driver_i2s` を追加(ローカルパッチ。managed_components は .gitignore 対象で再取得のたび消える) |
| 10 | `managed_components/chmorgan__esp-audio-player/audio_player.cpp:568` | `error: type qualifiers ignored on cast result type`(`-Werror=ignored-qualifiers`) | `(TaskHandle_t * const) NULL` という無意味な top-level const 修飾つきキャスト。GCC 15 でエラー昇格 | `(TaskHandle_t *) NULL` に修正(同上、ローカルパッチ) |
| 11 | `managed_components/espressif__wasm-micro-runtime/.../espidf_platform.c` | `fstatat` の `struct stat *buf` 引数で "declared inside parameter list" | `<sys/stat.h>` が(旧 IDF では推移的に入っていたが)未 include | ファイル先頭に `#include <sys/stat.h>` を追加(同上) |
| 12 | `managed_components/espressif__wasm-micro-runtime/.../espidf_file.c` | `struct stat` 未定義、`fstat`/`fstatat` 暗黙宣言(多数箇所) | 同上 | `#include <sys/stat.h>` を追加(同上)。ただし `renameat` は本ファイルの `<sys/stat.h>` だけでは解決せず(別ヘッダ由来、newlib では通常 `<stdio.h>`)、**未解決のまま中断** |
| 13 | `managed_components/espressif__wasm-micro-runtime/.../espidf_memmap.c` | `MALLOC_CAP_EXEC` undeclared | 下記「MALLOC_CAP_EXEC 分析」参照。単純な `#include "esp_heap_caps.h"` では解決しない | **未解決のまま中断**(詳細下記) |

#2〜#6 は同一パターン(IDF 6.0 での `driver` コンポーネント分割)。
`driver` は現在 I2C(レガシー)/TWAI/touch_sensor のみを `REQUIRES`(公開)し、
GPIO/SPI/I2S/SDMMC/SDSPI は `esp_driver_gpio` 等へ分離され `driver` からは
`PRIV_REQUIRES`(非公開)扱いになったため、`driver/*.h` を直接 include する
既存コンポーネントは軒並み `esp_driver_*` を明示 `REQUIRES` する必要が生じた。

#9〜#13 は managed_components 配下(registry 経由で取得される依存)で、
上流(chmorgan/esp-audio-player, espressif/wasm-micro-runtime)がまだ
IDF 6.0 / GCC 15 に追随していないために起きた問題。

## MALLOC_CAP_EXEC / CONFIG_HEAP_HAS_EXEC_HEAP 分析(中断の決め手)

`espidf_memmap.c` の `os_mmap()`:

```c
void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    if (prot & MMAP_PROT_EXEC) {
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
        uint32_t mem_caps = MALLOC_CAP_SPIRAM;
#else
        uint32_t mem_caps = MALLOC_CAP_EXEC;   // ← ここが未定義
#endif
        ...
    }
    ...
}
```

`MALLOC_CAP_EXEC` は IDF 6.0 の `esp_heap_caps.h` で

```c
#if CONFIG_HEAP_HAS_EXEC_HEAP
#define MALLOC_CAP_EXEC  (1<<0)
#endif
```

とガードされており、`CONFIG_HEAP_HAS_EXEC_HEAP` は本プロジェクトの
sdkconfig で有効な `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=y` /
`CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=y`(W^X 相当のメモリ保護。esp32s3 向け
IDF 6.0 のデフォルトと見られる)と両立しないため未定義になる
(ビルドログの HINT がこれを明示していた)。つまり単純なヘッダ不足ではなく、
**IDF 6.0 の esp32s3 では実行可能ヒープ確保がデフォルトで提供されなくなった**
というアーキテクチャ上の変更が根本原因。

**実行時に到達しない経路である根拠**: `os_mmap` のこの分岐は
`prot & MMAP_PROT_EXEC` の場合のみ実行される。`MMAP_PROT_EXEC` を要求する
呼び出しは WAMR の AOT/JIT 実行時(コンパイル済みコードを実行可能メモリに
配置する場合)にのみ発生する。本プロジェクトの WAMR 構成は
(Phase 1 実施記録より)`WAMR AOT disabled` / `Fast interpreter enabled`
であり、AOT モジュールをロードすることはない。したがって
`os_mmap(..., prot=MMAP_PROT_EXEC, ...)` が実際に呼ばれることはなく、
この分岐はビルド時にしか存在しない死コードのはずである
(実機・Linux 双方の既存 phase 1-7 実施記録で AOT 経路が使われたことはない)。

**修正の方向性(未実施)**: `CONFIG_HEAP_HAS_EXEC_HEAP` が定義されない場合の
フォールバック(例: `MALLOC_CAP_EXEC` を `MALLOC_CAP_DEFAULT` にマクロ的に
読み替える)を `espidf_memmap.c` 側にローカルパッチとして追加すれば
恐らくビルドは通る。ただしこれは managed_components 配下の上流コードへの
機能的パッチであり、AOT を将来有効化した場合に実行可能メモリが取得できず
サイレントに失敗する(または `heap_caps_malloc` が要求外のケーパビリティで
非実行可能メモリを返し、実行時に PMP フォールトになる)リスクを伴う。
このプロジェクトでは AOT を使う計画は当面ないため実害は低いと判断できるが、
安全側に倒すなら上流の正式な IDF 6 対応を待つのが妥当と判断し、
ここで試行を中断した。

`renameat` 等の未解決の暗黙宣言(#12)も同様に上流ヘッダ整理待ちの延長線上の
問題であり、深追いすれば都度 include 追加で倒せる可能性が高いが、
根本原因(MALLOC_CAP_EXEC)が解決しない限りビルドは通らないため中断。

## 再挑戦の条件

- `espressif/wasm-micro-runtime`(component registry)が **IDF 6 系対応版**
  (`MALLOC_CAP_EXEC` 分岐や `<sys/stat.h>` 等の include 漏れが upstream で
  修正されたバージョン)を公開したら、`idf_component.yml` のバージョン制約を
  更新して再挑戦する。
- それまでは `esp-idf-v5.5` を使い続ける(ビルド環境は README のコマンドで
  固定)。
- 再挑戦時は本メモの #1〜#8(`driver` 分割対応、`esp_lcd_panel_dev_config_t`
  リネーム、`lvgl_port_cfg_t` 新フィールド)は再度同じ修正が必要になる
  見込み(WAMR 側とは独立した IDF 6.0 自体の破壊的変更のため)。
