# YMEngine

**ymfm** をコアとした FM 音源エンジン DLL。  
**FmEngineApi** インターフェースに準拠した ymfm のラッパー実装です。

ymfm が対応する16種のチップをサポートし、チップ名文字列 (`"OPNA"`, `"OPL2"` 等) でインスタンスを作成できます。DLL はオーディオ出力機能を持ちません。アプリケーションのオーディオコールバックから `FmEngine_Generate()` を呼び出すことで波形データを取得します。

テストツールおよび API のドキュメントは **[FmEngineApiTest](https://github.com/your-org/FmEngineApiTest)** を参照してください。

## ファイル構成

```
YMEngine/
├── CMakeLists.txt
├── extern/
│   └── ymfm/              ← git submodule (aaronsgiles/ymfm)
├── src/
│   ├── FmChip.h           ymfm ラッパー・LinearResampler・ChipEntry テーブル
│   ├── FmEngine.h         複数チップ管理・SPSC キュー・ゲイン
│   ├── FmEngineApi.h  ★  DLL 公開用 C ファサード (宣言)
│   ├── FmEngineApi.cpp★  DLL 公開用 C ファサード (実装)
│   ├── FmEngineApi.def★  MSVC エクスポート定義
│   └── FmEngineApi.rc ★  DLL バージョン情報リソース
└── README.md
```

`★` は DLL のビルドに直接関係するファイルです。  
`FmEngineApi.h` だけを include すれば利用できます。

## セットアップ

```bash
git clone https://github.com/madscient/YMEngine
cd YMEngine
git submodule update --init --recursive
```

## ビルド

### Windows (Visual Studio 2022)

```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

成果物:
```
build/bin/FmEngineApi.dll   ← DLL 本体
build/bin/FmEngineApi.pdb   ← デバッグシンボル
build/lib/FmEngineApi.lib   ← インポートライブラリ
```

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 対応チップ

`FmEngine_Inquiry` / `FmEngine_GetSupportedChip` で実行時に取得できます。

| チップ名 | 実チップ | 標準クロック | 主な用途 |
|---|---|---|---|
| `Y8950`  | Y8950   | 3.58 MHz  | MSX-Audio |
| `OPL`    | YM3526  | 3.58 MHz  | 初期 AdLib |
| `OPL2`   | YM3812  | 3.58 MHz  | AdLib, Sound Blaster |
| `OPL3`   | YMF262  | 14.32 MHz | Sound Blaster 16 |
| `OPL4`   | YMF278B | 33.87 MHz | OPL4 |
| `OPN`    | YM2203  | 3.99 MHz  | PC-8801, PC-9801 |
| `OPNA`   | YM2608  | 7.99 MHz  | PC-8801mkIISR |
| `OPNB`   | YM2610  | 8.00 MHz  | NEO GEO |
| `OPNBB`  | YM2610B | 8.00 MHz  | TAITO アーケード |
| `OPN2`   | YM2612  | 7.67 MHz  | Mega Drive |
| `OPM`    | YM2151  | 3.58 MHz  | SFG-01/05, アーケード |
| `OPLL`   | YM2413  | 3.58 MHz  | MSX2+, Sega Master System |
| `OPLLP`  | YMF281  | 3.58 MHz  | パチンコ・パチスロ |
| `OPLLX`  | YM2423  | 3.58 MHz  | FM Melody Maker |
| `OPZ`    | YM2414  | 3.58 MHz  | TX81Z |
| `VRC7`   | DS1001  | 3.58 MHz  | Lagrange Point (FC) |

## 出力チャンネルの構成

`FmEngine_Generate` が返す L/R の出力は以下のようにミックスされます:

| チップ | L 出力 | R 出力 |
|---|---|---|
| OPM, OPN2, OPL3, OPL4 | FM-L | FM-R |
| OPNA, OPNB, OPNBB | FM-L + SSG | FM-R + SSG |
| OPN | FM + SSG (モノラル) | 同左 |
| OPL/OPL2/Y8950/OPLL系 | melody + rhythm | 同左 |
| その他 | data[0] | 同左 |

OPL3/OPL4 のリズムチャンネルは R チャンネルのみに出力されます (仕様)。

## fnum / key_code 計算メモ

### OPL 系

```
fm_sr = clk / (prescale × OPERATORS)
      = clk / 72   (OPL/OPL2/Y8950: prescale=4, ops=18)
      = clk / 288  (OPL3: prescale=8, ops=36)
      = clk / 684  (OPL4: prescale=19, ops=36)
fnum = freq × 2^(20−block) / fm_sr
```

### OPLL 系

```
fm_sr = 3579545 / 72 ≈ 49716 Hz
fnum = freq × 2^(19−block) / fm_sr   (指数が OPL と 1 異なる、最大511)
```

### OPN 系

```
fm_sr = clk / 72   (OPN: CHANNELS=3, OPERATORS=12)
      = clk / 144  (OPNA/OPNB/OPNBB/OPN2: CHANNELS=6, OPERATORS=24)
fnum = freq × 2^(21−block) / fm_sr   (指数が OPL と 1 異なる)
```

### OPM

KC レジスタ (`0x28+ch`) 方式。クロック 3.579545 MHz のとき KF=0 で平均律になるよう設計。  
NOTE の並びは C# 始まり・C 終わり (0=C#, 1=D, 2=D#, 4=E, 5=F, 6=F#, 8=G, 9=G#, 10=A, 12=A#, 13=B, 14=C)。

C4〜C5 の代表的な KC 値 (クロック 3.579545 MHz, KF=0):

| 音名 | KC |
|---|---|
| C4 | `0x3E` |
| D4 | `0x41` |
| E4 | `0x44` |
| F4 | `0x45` |
| G4 | `0x48` |
| A4 | `0x4A` |
| B4 | `0x4D` |
| C5 | `0x4E` |

クロックが 3.579545 MHz 以外の場合は KF で補正が必要。詳細は YM2151 アプリケーションマニュアル参照。

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **このエンジンコード**: MIT
