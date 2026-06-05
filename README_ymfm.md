# YMEngine — 内部 C++ API リファレンス

DLL を介さず C++ から直接使う場合の API リファレンスです。  
DLL 経由で使う場合は [README.md](README.md) を参照してください。

## 構成

```
YMEngine/
├── FmChip.h       ymfm ラッパー・LinearResampler (チップ抽象化)
├── FmEngine.h     複数チップ管理 + SPSC キュー + ゲイン
└── WasapiOutput.h WASAPI リアルタイム出力
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
#include "WasapiOutput.h"

// ① エンジンを 48000 Hz で作成
FmEngine engine(48000);

// ② チップ追加 (clock=0 で標準クロック自動選択)
uint32_t opnaId = engine.addChip(ChipType::OPNA);
uint32_t opl3Id = engine.addChip(ChipType::OPL3);

// ③ ゲイン設定 (1.0 = 0 dB)
engine.setGain(opnaId, 1.0f);
engine.setGain(opl3Id, ChipGain::dBToLinear(-6.0f)); // -6 dB

// ④ WASAPI 出力を開く (false = Shared mode)
WasapiOutput output(engine, false);
output.start();

// ⑤ レジスタ書き込み (任意スレッドから安全)
engine.write(opnaId, 0xB4, 0xC0); // CH0 L/R ON

// ⑥ 停止
output.stop();
```

## スレッドモデル

```
[ゲームスレッド]   engine.write()    →  SPSC キュー
[WASAPIスレッド]   engine.generate() ←  キュー消化 → リサンプル → ゲイン → ミックス → WASAPI
```

`write()` と `generate()` はロックフリーキューで完全に分離されており、
レジスタ書き込みがオーディオスレッドをブロックすることはありません。

## 対応チップ

| 列挙値 (ChipType) | チップ | 標準クロック | 主な用途 |
|---|---|---|---|
| `ChipType::Y8950`  | Y8950   | 3.58 MHz  | MSX-Audio |
| `ChipType::OPL`    | YM3526  | 3.58 MHz  | 初期 AdLib カード |
| `ChipType::OPL2`   | YM3812  | 3.58 MHz  | AdLib, Sound Blaster |
| `ChipType::OPL3`   | YMF262  | 14.3 MHz  | Sound Blaster 16 |
| `ChipType::OPL4`   | YMF278B | 16.93 MHz | OPL4 (ROM/RAM PCM 付き) |
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

クロックは `addChip()` の第2引数で上書き可能です（0 で標準値）。

```cpp
// PAL クロックの Mega Drive を再現
uint32_t id = engine.addChip(ChipType::OPN2, 7'600'489u);
```

## Exclusive mode

```cpp
WasapiOutput output(engine, /*exclusive=*/true);
```

排他モードではドライバが最小レイテンシ (1〜3 ms 程度) で動作します。  
対応デバイスの確認は「サウンドの詳細プロパティ → 排他モードを許可する」で行ってください。

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **このエンジンコード**: MIT
