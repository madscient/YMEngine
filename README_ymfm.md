# YMEngine — 内部 C++ API リファレンス

DLL を介さず C++ から直接使う場合の API リファレンスです。  
DLL 経由で使う場合は [README.md](README.md) を参照してください。

このエンジンは ymfm がカバーするチップのみをスコープとします。SSG/PSG や PCM 音源など ymfm 以外のチップを組み合わせる場合は、アプリケーション側の責任で別途統合してください。

エンジン自体はオーディオ出力機能を持ちません。`FmEngine::generate()` (または DLL 経由なら `FmEngine_Generate()`) は波形データを返すだけで、実際の再生デバイスへの出力はアプリケーション側で行ってください。

## 構成

```
src/
├── FmChip.h        ymfm ラッパー・LinearResampler (チップ抽象化)
├── FmEngine.h      複数チップ管理 + SPSC キュー + ゲイン
├── FmEngineApi.h   DLL 公開用 C ファサード (宣言)
└── FmEngineApi.cpp DLL 公開用 C ファサード (実装)
```

## セットアップ

```bash
git submodule update --init --recursive
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## 基本的な使い方

```cpp
#include "FmEngine.h"

// ① エンジンを 48000 Hz で作成
FmEngine engine(48000);

// ② チップ追加。クロック (Hz) は必ず指定する。0 なら UINT32_MAX が返る
uint32_t opnaId = engine.addChip(ChipType::OPNA, 7'987'200);
uint32_t opl3Id = engine.addChip(ChipType::OPL3, 14'318'180);

// ③ ゲイン設定 (1.0 = 0 dB)
engine.setGain(opnaId, 1.0f);
engine.setGain(opl3Id, ChipGain::dBToLinear(-6.0f));
// 部位ごとのゲイン。実際のゲインは setGain() との積。持たない部位なら false
engine.setPartGain(opnaId, ChipPart::OPN_SSG, 0.5f, 0.5f);
engine.setPartGain(opl3Id, ChipPart::OPL3_CD, 1.0f, 1.0f);  // C/D は既定で 0

// ④ レジスタ書き込み (任意スレッドから安全)
// write(chip_id, reg, value, port)
//   port=0: bank0 (offset 0/1)、port!=0: bank1 (offset 2/3)
engine.write(opnaId, 0xB4, 0xC0);     // CH0 L/R ON

// ⑤ 波形生成 (アプリケーションが用意するオーディオコールバック内から呼ぶ)
void audioCallback(float* out_l, float* out_r, uint32_t frames) {
    engine.generate(out_l, out_r, frames);
}
```

## write() のセマンティクス

ymfm の `write(offset, data)` はハードウェアのアドレス/データバスを模倣しており、
`FmChipImpl::write()` 内で自動的に 2 ステップ書き込みに変換されます。

```
engine.write(chip_id, reg, value, port)
  →  m_chip.write(addr_offset, reg)    // アドレスポートにレジスタ番号
  →  m_chip.write(data_offset, value)  // データポートに値
```

`port=0` → `addr_offset=0 / data_offset=1` (bank0)  
`port!=0` → `addr_offset=2 / data_offset=3` (bank1、OPN2/OPNA 等の bank 選択。OPL4 は port2 が AWM/PCM 側レジスタ)

## スレッドモデル

```
[アプリの任意スレッド]     engine.write()    →  SPSC キュー (lock-free)
[アプリのオーディオスレッド] engine.generate() ←  キュー消化 → リサンプル → ゲイン → ミックス
```

`write()` と `generate()` はロックフリーキューで完全に分離されており、レジスタ書き込みがオーディオスレッドをブロックすることはありません。`setGain()` と `setPartGain()` は `std::atomic<float>` を使用しているため任意スレッドから安全に呼べます。`generate()` はオーディオコールバックスレッドなど、アプリケーションが波形を消費するスレッドから呼び出してください。

### キーオン/オフの連続書き込み

同じチャンネルのキー状態を、間に波形生成を挟まずに2回以上変えた場合 (例: KEY OFF の直後に KEY ON)、2回目以降の書き込みは前の状態を約2ms 生成してから適用されます。途中の状態 (KEY OFF のリリースや打撃の立ち上がり) を読み飛ばさないためです。別チャンネルどうしの変化はまとめて適用されるため、和音や同時に鳴る打楽器は遅れません。

この遅れは `generate()` の呼び出しをまたいで持ち越され、後続の書き込みも順序を保って待ちます。衝突が N 回重なると、それ以降の書き込みは最大で N × 約2ms 遅れます。

## OPN 系の FM と SSG

OPN, OPNA, OPNB, OPNBB は、ymfm のチップクラスを継承した `detail::Ym2203Split` / `Ym2608Split` / `Ym2610Split<>` を使います。ymfm の `generate()` は FM と SSG を1本の列にまとめるため同じ値を繰り返して速い方のレートに揃えますが、これらのクラスは FM と SSG をそれぞれ本来のレートで1サンプルずつ取り出します。`FmChipImpl` は FM 用と SSG 用に `LinearResampler` を1本ずつ持ち、出力レートに変換してから部位ごとのゲインを掛けて足します。

| チップ | FM のレート | SSG のレート |
|---|---|---|
| OPN  | clk / (prescale × 12) | clk / 16, 8, 4 (prescale 6, 3, 2) |
| OPNA | clk / (prescale × 24) | clk / 32, 16, 8 (prescale 6, 3, 2) |
| OPNB, OPNBB | clk / 144 | clk / 32 |

prescale は `0x2D`〜`0x2F` への書き込みで切り替わり、そのたびに両方のリサンプラを設定し直します。`nativeRate()` は FM のレートを返します。

## 出力の部位

`ChipPart` はチップが別々の端子から出す出力で、番号はチップをまたいで重なりません。`FmChip::hasPart()` はそのチップの部位にだけ true を返し、出力が1本のチップ (OPL, OPL2, Y8950, OPN2, OPM, OPZ) はどの部位にも false を返します。`FmEngine::getPartMask()` は、チップが持つ部位をビットマスク (bit n = `ChipPart` の n 番) で返します。部位ごとの意味と既定値は README.md の「部位ごとのゲイン」を参照してください。既定値は `defaultPartGain()` が返します。

OPN 系以外で部位を持つチップは、ymfm の出力を部位ごとに別々に出力レートへ変換し、ゲインを掛けてから L/R に混ぜます。リサンプラのチャンネル数は OPLL 系が2 (メロディ、リズム)、OPL3 が4 (A/B/C/D)、OPL4 が6 (DO2、DO0、DO1 の各 L/R) です。

`FmEngine` は `FmChip::generate()` に `PartGains` を渡します。`l[]`/`r[]` はチップのゲインと部位のゲインの積、`chip_l`/`chip_r` はチップのゲインだけで、部位を持たないチップが使います。

C API の `FmPart` は `ChipPart` にキャストして渡すので、番号を揃えてあります。`FmEngineApi.cpp` の `static_assert` で照合しています。

## ymfm チップのコンストラクタ特殊化

ymfm の全チップは `(ymfm_interface&, uint32_t clock)` を取らないため、`FmChipImpl` の完全特殊化で吸収しています。

| コンストラクタパターン | 対象チップ |
|---|---|
| `(interface&)` のみ | Y8950, OPL, OPL2, OPL3, OPL4, OPN, OPNA, OPNBB, OPN2, OPZ (OPN 系と Y8950 は継承したクラス経由) |
| `(interface&, const uint8_t*)` | OPLL, OPLLX, OPLLP, VRC7 |
| `(interface&, opm_variant)` | OPM ※`protected` のため public コンストラクタを使用 |
| `(interface&, uint8_t channel_mask)` | OPNB |

## 対応チップ一覧

| 列挙値 (ChipType) | チップ | クロックの例 | 主な用途 |
|---|---|---|---|
| `ChipType::Y8950`  | Y8950   | 3.58 MHz  | MSX-Audio |
| `ChipType::OPL`    | YM3526  | 3.58 MHz  | 初期 AdLib カード |
| `ChipType::OPL2`   | YM3812  | 3.58 MHz  | AdLib, Sound Blaster |
| `ChipType::OPL3`   | YMF262  | 14.32 MHz | Sound Blaster 16 |
| `ChipType::OPL4`   | YMF278B | 33.87 MHz | OPL4 (ROM/RAM PCM 付き) |
| `ChipType::OPN`    | YM2203  | 3.99 MHz  | PC-8801, PC-9801 |
| `ChipType::OPNA`   | YM2608  | 7.99 MHz  | PC-8801mkIISR, PC-9801 |
| `ChipType::OPNB`   | YM2610  | 8.00 MHz  | NEO GEO |
| `ChipType::OPNBB`  | YM2610B | 8.00 MHz  | TAITO アーケード |
| `ChipType::OPN2`   | YM2612  | 7.67 MHz  | Mega Drive, FM TOWNS |
| `ChipType::OPM`    | YM2151  | 3.58 MHz  | SFG-01/05, アーケード |
| `ChipType::OPLL`   | YM2413  | 3.58 MHz  | MSX2+, Sega Master System |
| `ChipType::OPLLP`  | YMF281  | 3.58 MHz  | パチンコ・パチスロ |
| `ChipType::OPLLX`  | YM2423  | 3.58 MHz  | FM Melody Maker, PMC100 |
| `ChipType::OPZ`    | YM2414  | 3.58 MHz  | TX81Z |
| `ChipType::VRC7`   | DS1001  | 3.58 MHz  | Lagrange Point (FC) |

クロックは第2引数で必ず指定します。エンジンは既定のクロックを持ちません。`addChip()` / `addChipByName()` は clock が 0 なら `UINT32_MAX` を、`createChip()` / `createChipByName()` は `nullptr` を返します。表のクロックは例です。

```cpp
uint32_t id = engine.addChip(ChipType::OPN2, 7'600'489u); // PAL Mega Drive
```

## 外部メモリ

ADPCM・PCM を持つチップは、ymfm の `ymfm_external_read()` / `ymfm_external_write()` で外部メモリを読み書きします。`FmChipImpl` の `m_iface` (`MemoryYmfmInterface`) が、呼び出し元のブロックを番地の範囲に割り当ててこれに応えます。チップごとのメモリの意味と挙動は README.md の「外部メモリ」と同じです。

### メモリ種別

`ChipMemoryType` / `ChipMemoryAccess` は C API の `FmMemoryType` / `FmMemoryAccess` と番号を揃えてあり、`FmEngineApi.cpp` の `static_assert` で照合しています。

| `ChipMemoryType` | `FmMemoryType` | ymfm のアクセス | 対象チップ |
|---|---|---|---|
| `ADPCM_A`         | `FM_MEM_ADPCM_A`         | `ACCESS_ADPCM_A` | OPNA, OPNB, OPNBB |
| `ADPCM_B`         | `FM_MEM_ADPCM_B`         | `ACCESS_ADPCM_B` (OPNA/Y8950 は RAM モードのとき) | OPNA, OPNB, OPNBB, Y8950 |
| `PCM`             | `FM_MEM_PCM`             | `ACCESS_PCM` | OPL4 |
| `ADPCM_B_ROMMODE` | `FM_MEM_ADPCM_B_ROMMODE` | `ACCESS_ADPCM_B` (OPNA/Y8950 が ROM モードのとき) | OPNA, Y8950 |

`FmChip::hasMemory()` は、そのチップが持つ種別にだけ true を返します。

### C++ API

```cpp
// C API の FmEngine_SetMemoryEx と同じ。data が nullptr なら範囲と重なるブロックを外す
engine.mapMemory(y8950Id, ChipMemoryType::ADPCM_B, 0, ram, 32768, ChipMemoryAccess::RAM);
engine.mapMemory(y8950Id, ChipMemoryType::ADPCM_B_ROMMODE, 0, rom, romSize, ChipMemoryAccess::ROM);

// C API の FmEngine_SetMemory と同じ。type のメモリを [0, size) の data だけにする
engine.setMemory(opnbId, ChipMemoryType::ADPCM_A, romA, romASize);

// 割り当てたブロックの大きさの合計
uint32_t sz = engine.getMemorySize(opnbId, ChipMemoryType::ADPCM_A);
```

`mapMemory()` と `setMemory()` は、引数が誤っていれば false を返します (C API の `FM_ERR_INVALID_ARG`)。どちらもスレッドセーフではないので、オーディオスレッドで `generate()` を始める前に呼んでください。

### 内部実装 (`MemoryYmfmInterface`)

- 種別ごとにブロックの一覧を持ち、ymfm のアクセスのたびに番地を含むブロックを探します。割り当ての無い番地は 0 を読み、書き込みは捨てます。`ACCESS_IO` (SSG の I/O ポートなど) はどのメモリにも当てません
- ROM のブロックは書き込み先を持たないので、チップの書き込みは捨てます
- OPNA と Y8950 は、構築時に `bindAdpcmBRegs()` で ADPCM-B のレジスタを結び付けます。`ACCESS_ADPCM_B` のたびに ROM/RAM 選択ビット (`rom_ram()`) を見て、`ADPCM_B_ROMMODE` と `ADPCM_B` のどちらかを選びます。ymfm はこのビットでアドレスの刻みを変えるだけで、メモリは1つの空間として扱うためです。上流の `m_adpcm_b` は protected なので、`detail::Ym2608Split` と `detail::Y8950Mem` が `adpcmBRegs()` で見せています

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **このエンジンコード**: MIT
