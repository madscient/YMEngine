# YMEngine — 内部 C++ API リファレンス

DLL を介さず C++ から直接使う場合の API リファレンスです。  
DLL 経由で使う場合は [README.md](README.md) を参照してください。

## 構成

```
src/
├── FmChip.h        ymfm ラッパー・LinearResampler (チップ抽象化)
├── FmEngine.h      複数チップ管理 + SPSC キュー + ゲイン
├── ExternalChip.h  外部ライブラリチップラッパー (PSG/SN76489/SCC/SAA1099)
└── WasapiOutput.h  WASAPI リアルタイム出力・デバイス列挙
```

## セットアップ

```bash
git submodule update --init --recursive
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --build build --target SAASound --config Release  # SAA1099 を使う場合
```

## 基本的な使い方

```cpp
#include "FmEngine.h"
#include "ExternalChip.h"   // 外部チップを使う場合
#include "WasapiOutput.h"

// ① エンジンを 48000 Hz で作成
FmEngine engine(48000);

// ② ymfm チップ追加 (clock=0 で標準クロック自動選択)
uint32_t opnaId = engine.addChip(ChipType::OPNA);
uint32_t opl3Id = engine.addChip(ChipType::OPL3);

// ③ 外部ライブラリチップ追加
uint32_t psgId = engine.addExtChip(ChipTypeExt::PSG);
uint32_t sngId = engine.addExtChip(ChipTypeExt::SN76489);
uint32_t sccId = engine.addExtChip(ChipTypeExt::SCC);
uint32_t saaId = engine.addExtChip(ChipTypeExt::SAA1099); // SAASound.dll が必要

// ④ ゲイン設定 (1.0 = 0 dB)
engine.setGain(opnaId, 1.0f);
engine.setGain(opl3Id, ChipGain::dBToLinear(-6.0f));
engine.setGain(psgId,  0.7f);

// ⑤ デフォルトデバイスで WASAPI 出力を開く (false = Shared mode)
WasapiOutput output(engine, false);
output.start();

// ⑥ レジスタ書き込み (任意スレッドから安全)
// ymfm 系: write(chip_id, reg, value, port)
//   port=0: bank0 (offset 0/1)、port!=0: bank1 (offset 2/3)
engine.write(opnaId, 0xB4, 0xC0);     // CH0 L/R ON
// PSG 系: write(chip_id, reg_num, value, port=0)
engine.write(psgId, 0x08, 0x0F);      // CH A volume=15

// ⑦ 停止
output.stop();
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
`port!=0` → `addr_offset=2 / data_offset=3` (bank1、OPN2/OPNA 等の bank 選択)

外部チップ (PSG/SN76489/SCC/SAA1099) はライブラリごとに異なるインターフェースを持ちますが、`ExternalChip.h` 内のラッパーがそれぞれ適切に変換します。

## デバイスの明示指定

```cpp
#include "WasapiOutput.h"

// デバイス列挙
// enumerateWasapiDevices() は COM を内部で自己管理するため
// CoInitialize 済みかどうかを問わず呼べる
std::vector<WasapiDeviceInfo> devices = enumerateWasapiDevices();
for (const auto& d : devices) {
    // d.id        : デバイスID (wstring)
    // d.name      : 表示名 (wstring)
    // d.isDefault : デフォルトデバイスか
}

// デバイスIDを指定して初期化
WasapiOutput output(engine, false, devices[1].id);
```

## WASAPI 動作モード

| モード | 説明 |
|---|---|
| Shared mode (`exclusive=false`) | `GetMixFormat` で取得したネイティブフォーマットで `Initialize`。float32 → デバイスフォーマットへの変換は自前実装 (`AUTOCONVERTPCM` 非依存)。 |
| Exclusive mode (`exclusive=true`) | `IsFormatSupported` で確認後に初期化。非対応の場合は Shared mode に自動降格。 |

対応デバイスフォーマット: Float32 / Int16 / Int24 / Int32

デバイスとエンジンのサンプルレートが異なる場合、`renderLoop` 内で線形補間リサンプリングを行います。

## スレッドモデル

```
[アプリスレッド]   engine.write()    →  SPSC キュー (lock-free)
[WASAPIスレッド]   engine.generate() ←  キュー消化 → リサンプル → ゲイン → ミックス → WASAPI
```

`write()` と `generate()` はロックフリーキューで完全に分離されており、レジスタ書き込みがオーディオスレッドをブロックすることはありません。`setGain()` は `std::atomic<float>` を使用しているため任意スレッドから安全に呼べます。

## SAASound (SAA1099) の動的ロード

SAA1099 チップは名前衝突 (`BYTE` マクロ等) の問題により `FmEngineApi.dll` に静的リンクできないため、`SAASound.dll` を実行時に `LoadLibrary` でロードします。

```
SAASound.dll が見つかる場合  → SAA1099 チップが正常に動作
SAASound.dll が見つからない → addExtChip(ChipTypeExt::SAA1099) で例外
                              → FmEngine_AddExtChip が FM_ERR_EXCEPTION を返す
```

`SAASound.dll` は `FmEngineApi.dll` と同じディレクトリに配置してください。

## ymfm チップのコンストラクタ特殊化

ymfm の全チップは `(ymfm_interface&, uint32_t clock)` を取らないため、`FmChipImpl` の完全特殊化で吸収しています。

| コンストラクタパターン | 対象チップ |
|---|---|
| `(interface&)` のみ | Y8950, OPL, OPL2, OPL3, OPL4, OPN, OPNA, OPNBB, OPN2, OPZ |
| `(interface&, const uint8_t*)` | OPLL, OPLLX, OPLLP, VRC7 |
| `(interface&, opm_variant)` | OPM ※`protected` のため public コンストラクタを使用 |
| `(interface&, uint8_t channel_mask)` | OPNB |

## 対応チップ一覧

### ymfm チップ

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

クロックは第2引数で上書き可能 (0 で標準値):

```cpp
uint32_t id = engine.addChip(ChipType::OPN2, 7'600'489u); // PAL Mega Drive
```

### 外部ライブラリチップ

| 列挙値 (ChipTypeExt) | チップ | 標準クロック | ライブラリ |
|---|---|---|---|
| `ChipTypeExt::PSG`     | YM2149 (PSG) | 3.58 MHz | emu2149 (静的リンク) |
| `ChipTypeExt::SN76489` | SN76489      | 3.58 MHz | emu76489 (静的リンク) |
| `ChipTypeExt::SCC`     | SCC/K051649  | 3.58 MHz | emu2212 (静的リンク) |
| `ChipTypeExt::SAA1099` | SAA1099      | 8.00 MHz | SAASound.dll (動的ロード) |

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **emu2149 / emu76489 / emu2212**: MIT (digital-sound-antiques)
- **SAASound**: GPL-2.0 (stripwax) — 配布時はライセンス条件を確認してください
- **nlohmann/json**: MIT (nlohmann)
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
// ROM データを設定 (Wasapi 起動前に呼ぶこと)
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

`setMemory()` はスレッドセーフではありません。`WasapiOutput::start()` より前に設定してください。設定した `data` ポインタが指すバッファは再生終了まで解放しないでください。
