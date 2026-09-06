# Phase 12 実施記録 — 基盤整備(パーティション拡張・アプリ整理・実機テスト自動化・PSRAM 可否)

対応する指示書: `docs/prompts/phase12.md`
前提の実測値: `docs/results/phase11.md`(最終回帰)、`docs/results/phase10.md`(P10-4)

生データ: `captures/phase12/`(.gitignore 対象)

---

## 作業 1: パーティション拡張(2026-09-06)

### 変更内容

| ファイル | 変更 |
|---|---|
| `src/sdkconfig.defaults` | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` / `CONFIG_ESPTOOLPY_FLASHSIZE="16MB"` / `CONFIG_PARTITION_TABLE_CUSTOM=y` / `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"` を追記 |
| `src/partitions.csv` | **新規**(下表) |

`src/sdkconfig` は .gitignore 対象の生成物なので、削除して
`sdkconfig.defaults` から再生成した。再生成前後の差分は上記 4 項目
(`FLASHSIZE_2MB`→`FLASHSIZE_16MB`、`PARTITION_TABLE_SINGLE_APP`→`CUSTOM`、
`PARTITION_TABLE_FILENAME`)**のみ**で、他の設定はドリフトしていないことを
`diff` で確認済み。

### パーティション表(前後)

| | 変更前(`partitions_singleapp.csv` / 2MB) | 変更後(`partitions.csv` / 16MB) |
|---|---|---|
| nvs | data/nvs `0x9000` `0x6000`(24K) | **同一** |
| phy_init | data/phy `0xf000` `0x1000`(4K) | **同一** |
| factory | app/factory `0x10000` **`0x100000`(1M)** | app/factory `0x10000` **`0x400000`(4M)** |

**オフセットは 3 パーティションとも変更前と同一**で、`factory` のサイズだけが
1MB → 4MB に増えている。したがって **NVS の内容は失われない**(そもそも
NVS を使っているのは `app_main.cpp` の `nvs_flash_init()` だけで、アプリ設定等の
永続化データは現状ゼロ。`grep -rn "nvs_"` で確認済み)。

OTA は不要(無線スタックなし)なので factory 1 本のみ。

### 残容量(`idf.py size` / `check_sizes.py`)

| 項目 | 変更前 | 変更後 |
|---|---|---|
| バイナリサイズ | 0xfeff0 = 1,044,464 B | 0xfeff0 = 1,044,464 B(コード変更なし) |
| アプリパーティション | 0x100000 = 1,048,576 B | **0x400000 = 4,194,304 B** |
| 残り | **0x1010 = 4,112 B(0%)** | **0x301010 = 3,149,840 B(75%)** |

`idf.py size` の内訳(変更後):

| 区分 | サイズ |
|---|---|
| Flash Code `.text` | 634,818 |
| Flash Data `.rodata` | 306,676 |
| DIRAM(.bss/.text/.data) | 231,287(67.68%、残 110,473) |
| IRAM | 16,384(100%) |
| Total image | 1,044,345 |

**指示書の前提の訂正**: 「ファームウェア領域の逼迫は `.wasm` 埋め込みが主因」は
実測と合わない。埋め込み総量は `.wasm` 10 本 = **21,201 B**、MP3 アセット 3 本 =
**145,739 B**、合計 166,940 B で、これは `.rodata` の 54% ではあるが**イメージ全体の
16%** にすぎない。主因は Flash Code `.text` 634,818 B(ESP-IDF + LVGL + WAMR)である。
つまり**アプリ整理(作業 2)はフラッシュ容量対策としてはほぼ効かない**
(削除候補 4 本で 1,724 B)。作業 2 の価値は容量ではなく「回帰対象の削減」にある。

### 予約データ領域を切るかどうか — **切らない(実測に基づく判断)**

当初 `appdata, data, fat, 0x410000, 0x400000` を将来用に予約した表で 1 度
ビルド・フラッシュし、起動時の free heap を比較した:

| 構成 | `Audio_Init` 前 | `Audio_Init` 後 | `runtime ready` |
|---|---|---|---|
| Phase 11 最終(2MB / single app) | 151,428 | 104,224 | 97,800 |
| 16MB / factory 4M + **appdata 予約あり** | 151,372 | 104,168 | 97,744 |
| 16MB / factory 4M(**予約なし・採用**) | **151,428** | **104,224** | **97,800** |

**パーティションエントリ 1 個につき internal heap が 56 B 恒久的に減る**
(`esp_partition` のランタイムリストがヒープ上に作られるため)。予約は現時点で
消費者がおらず(アプリは SD 上に置く方針)、`factory` の後ろは空き番地なので
**後から追加してもどのオフセットも動かない**。よって予約せず、
**free heap を Phase 11 基準とビット一致させる**方を採った。

### 起動確認(実機)

生データ: `captures/phase12/monitor-work1.log`

- ブートローダの Partition Table ログが nvs / phy_init / factory の 3 本を表示。
  `esptool.py read_flash 0x8000` で実フラッシュからも同じ表を読み出して確認
  (`captures/phase12/pt_readback.bin`)。
- **`idf.py erase-flash` は不要だった。** 既存 3 パーティションのオフセットが
  同一なので、通常の `idf.py flash`(bootloader + partition-table + app)だけで
  移行できる。SD カードにも一切影響しない。
- 起動時 free heap は **Phase 11 最終回帰と完全一致**(上表)。
- **既知の起動警告が 1 件消えた**:
  `spi_flash: Detected size(16384k) larger than the size in the binary image
  header(2048k)` はイメージヘッダのフラッシュサイズが 16MB になったため出なくなった。
  以後の回帰ではこの行を「既知の許容パターン」から外してよい。
- WARN/ERROR 0 件、SD マウント正常、`menu: 8 app(s) listed`。
