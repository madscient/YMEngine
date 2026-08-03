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

// ② チップ追加 (clock=0 で標準クロック自動選択)
uint32_t opnaId = engine.addChip(ChipType::OPNA);
uint32_t opl3Id = engine.addChip(ChipType::OPL3);

// ③ ゲイン設定 (1.0 = 0 dB)
engine.setGain(opnaId, 1.0f);
engine.setGain(opl3Id, ChipGain::dBToLinear(-6.0f));

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

`write()` と `generate()` はロックフリーキューで完全に分離されており、レジスタ書き込みがオーディオスレッドをブロックすることはありません。`setGain()` は `std::atomic<float>` を使用しているため任意スレッドから安全に呼べます。`generate()` はオーディオコールバックスレッドなど、アプリケーションが波形を消費するスレッドから呼び出してください。

## ymfm チップのコンストラクタ特殊化

ymfm の全チップは `(ymfm_interface&, uint32_t clock)` を取らないため、`FmChipImpl` の完全特殊化で吸収しています。

| コンストラクタパターン | 対象チップ |
|---|---|
| `(interface&)` のみ | Y8950, OPL, OPL2, OPL3, OPL4, OPN, OPNA, OPNBB, OPN2, OPZ |
| `(interface&, const uint8_t*)` | OPLL, OPLLX, OPLLP, VRC7 |
| `(interface&, opm_variant)` | OPM ※`protected` のため public コンストラクタを使用 |
| `(interface&, uint8_t channel_mask)` | OPNB |

## 対応チップ一覧

| 列挙値 (ChipType) | チップ | 標準クロック | 主な用途 |
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

クロックは第2引数で上書き可能 (0 で標準値):

```cpp
uint32_t id = engine.addChip(ChipType::OPN2, 7'600'489u); // PAL Mega Drive
```

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **このエンジンコード**: MIT

## 外部メモリ (ADPCM/PCM ROM)

ADPCM・PCM を内蔵するチップは ymfm の `ymfm_external_read()` コールバック経由で外部メモリを参照します。`FmChipImpl` の `m_iface` は `MemoryYmfmInterface` として実装されており、3 種のメモリ領域を保持します。

### メモリ種別

| `ymfm::access_class` | `FmMemoryType` (C API) | 対象チップ |
|---|---|---|
| `ACCESS_ADPCM_A` | `FM_MEM_ADPCM_A` | OPNB (YM2610), OPNBB (YM2610B) |
| `ACCESS_ADPCM_B` | `FM_MEM_ADPCM_B` | OPNA (YM2608), OPNB, OPNBB, Y8950 |
| `ACCESS_PCM`     | `FM_MEM_PCM`     | OPL4 (YMF278B) |

### C++ API

```cpp
// ROM データを設定 (オーディオコールバック開始前に呼ぶこと)
engine.setMemory(opnaId, ymfm::ACCESS_ADPCM_B, romData, romSize);

// 設定済みサイズの確認
uint32_t sz = engine.memorySize(opnaId, ymfm::ACCESS_ADPCM_B);
```

### 内部実装 (`MemoryYmfmInterface`)

`MemoryYmfmInterface` は `BasicYmfmInterface` の代わりに `FmChipImpl::m_iface` として使われます。

```cpp
// 外部 ROM ポインタを渡す (寿命は呼び出し元管理)
m_iface.setMemory(ymfm::ACCESS_ADPCM_B, romPtr, romSize);

// 書き込み可能 RAM を内部確保 (ADPCM-B の RAM 録音用)
m_iface.allocMemory(ymfm::ACCESS_ADPCM_B, 512 * 1024);
```

`setMemory()` は読み取り専用 ROM を想定しています。書き込み可能 RAM が必要な場合 (ADPCM-B の RAM モード等) は `allocMemory()` を使って内部バッファを確保してください。

### 注意事項

`setMemory()` はスレッドセーフではありません。オーディオコールバックの開始 (`generate()` の呼び出し開始) より前に設定してください。設定した `data` ポインタが指すバッファは再生終了まで解放しないでください。
