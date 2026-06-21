# YMEngine

**ymfm** をコアとした Windows 向け FM 音源エンジン。  
ymfm 16種 + 外部ライブラリ 4種 の計20種のチップに対応し、WASAPI でリアルタイム再生します。

## ファイル構成

```
YMEngine/
├── CMakeLists.txt
├── extern/
│   ├── ymfm/              ← git submodule (aaronsgiles/ymfm)
│   ├── emu2149/           ← git submodule (digital-sound-antiques/emu2149)
│   ├── emu76489/          ← git submodule (digital-sound-antiques/emu76489)
│   ├── emu2212/           ← git submodule (digital-sound-antiques/emu2212)
│   ├── SAASound/          ← git submodule (stripwax/SAASound) ※独立DLL
│   └── nlohmann_json/     ← git submodule (nlohmann/json)
├── src/
│   ├── FmChip.h           ymfm ラッパー・LinearResampler
│   ├── FmEngine.h         複数チップ管理・SPSC キュー・ゲイン
│   ├── ExternalChip.h     外部ライブラリチップ (SSG/DCSG/SCC/SAA)
│   ├── WasapiOutput.h     WASAPI リアルタイム出力・デバイス列挙
│   ├── FmEngineApi.h  ★  DLL 公開用 C ファサード (宣言)
│   ├── FmEngineApi.cpp★  DLL 公開用 C ファサード (実装)
│   ├── FmEngineApi.def★  MSVC エクスポート定義
│   ├── FmEngineApi.rc ★  DLL バージョン情報リソース
│   ├── main.cpp           全チップ全チャンネルテスト (JSON 駆動)
│   └── patches/           チップ別テスト用パッチ定義 (JSON)
├── README.md
└── README_ymfm.md         内部 C++ API リファレンス
```

## セットアップ

```bash
git clone https://github.com/madscient/YMEngine
cd YMEngine
git submodule update --init --recursive
```

サブモジュールを個別に追加する場合:

```bash
git submodule add https://github.com/aaronsgiles/ymfm                    extern/ymfm
git submodule add https://github.com/digital-sound-antiques/emu2149       extern/emu2149
git submodule add https://github.com/digital-sound-antiques/emu76489      extern/emu76489
git submodule add https://github.com/digital-sound-antiques/emu2212       extern/emu2212
git submodule add https://github.com/stripwax/SAASound                    extern/SAASound
git submodule add https://github.com/nlohmann/json                        extern/nlohmann_json
```

## ビルド (Visual Studio 2022)

```bash
# 構成生成 (.sln に FmEngineApi / SAASound / sample_app が追加される)
cmake -B build -G "Visual Studio 17 2022" -A x64

# FmEngineApi.dll + sample_app.exe をビルド
cmake --build build --config Release

# SAASound.dll を別途ビルド (SAA1099 を使う場合)
cmake --build build --target SAASound --config Release

# 成果物
#   build/bin/FmEngineApi.dll      ← DLL 本体
#   build/bin/FmEngineApi.pdb      ← デバッグシンボル
#   build/lib/FmEngineApi.lib      ← インポートライブラリ
#   build/bin/SAASound.dll         ← SAA1099 用 DLL (オプション)
#   build/bin/sample_app.exe       ← 全チップテストアプリ
#   build/bin/patches/             ← テスト用パッチ定義 (JSON)
```

> **Note**: `SAASound.dll` は `FmEngineApi.dll` と同じディレクトリに配置してください。  
> 存在しない場合は SAA1099 チップの追加時にエラーになりますが、他のチップには影響しません。

## DLL の使い方

利用側は **`FmEngineApi.h` だけを include** し、`FmEngineApi.lib` にリンクします。  
`FmEngine.h` / `WasapiOutput.h` / ymfm / emu2149 等のヘッダは不要です。

```c
#include "FmEngineApi.h"
#pragma comment(lib, "FmEngineApi.lib")

// エンジン作成 (48000 Hz)
FmEngineHandle eng = FmEngine_Create(48000);

// ymfm チップ追加 (clock=0 で標準クロック自動選択)
uint32_t opnaId;
FmEngine_AddChip(eng, FM_CHIP_OPNA, 0, &opnaId);

// 外部ライブラリチップ追加
uint32_t ssgId;
FmEngine_AddExtChip(eng, FM_CHIP_EXT_SSG, 0, &ssgId);

// ゲイン設定 (1.0 = 0 dB、L/R 独立指定可能)
FmEngine_SetGain(eng, opnaId, 1.0f, 1.0f);
FmEngine_SetGain(eng, ssgId,  0.9f, 0.7f);  // 例: L=0.9, R=0.7 でパン寄せ

// 設定値の取得
float gl, gr;
FmEngine_GetGain(eng, opnaId, &gl, &gr);

// WASAPI 再生 (デフォルトデバイス、Shared mode)
WasapiHandle wasapi = Wasapi_Create(eng, 0);
Wasapi_Start(wasapi);

// レジスタ書き込み (任意スレッドから安全)
// ymfm 系: write(chip_id, reg, value, port)
FmEngine_Write(eng, opnaId, 0xB4, 0xC0, 0);
// SSG 系:  write(chip_id, reg_num, value, port=0)
FmEngine_Write(eng, ssgId,  0x08, 0x0F, 0);

// 停止・解放
Wasapi_Stop(wasapi);
Wasapi_Destroy(wasapi);
FmEngine_Destroy(eng);
```

### ゲイン設定

`FmEngine_SetGain` / `FmEngine_GetGain` は **L/R 独立** したゲインです。チップ単位 (チップ追加直後の `generateNative` 出力に対して) 適用され、チャンネル単位の調整はできません。

```c
FmResult FmEngine_SetGain(FmEngineHandle engine, uint32_t chip_id, float gain_l, float gain_r);
FmResult FmEngine_GetGain(FmEngineHandle engine, uint32_t chip_id, float* out_gain_l, float* out_gain_r);
```

### オーディオデバイスの明示指定

```c
// デバイス列挙
uint32_t count = Wasapi_GetDeviceCount();
for (uint32_t i = 0; i < count; ++i) {
    wchar_t id[256] = {}, name[256] = {};
    Wasapi_GetDeviceId(i, id, 256);
    Wasapi_GetDeviceName(i, name, 256);
    int isDefault = Wasapi_IsDefaultDevice(i);
}

// デバイスIDを指定して作成
WasapiHandle wasapi = Wasapi_CreateWithDevice(eng, 0, id);

// デバイスのサンプルレートを取得
uint32_t devRate = Wasapi_GetSampleRate(wasapi);
```

### C# (P/Invoke) からの使用例

```csharp
using System.Runtime.InteropServices;
using System.Text;

static class FmEngineApi {
    const string DLL = "FmEngineApi";

    [DllImport(DLL)] public static extern IntPtr  FmEngine_Create(uint sampleRate);
    [DllImport(DLL)] public static extern void    FmEngine_Destroy(IntPtr engine);
    [DllImport(DLL)] public static extern int     FmEngine_AddChip(
        IntPtr engine, int chipType, uint clock, out uint chipId);
    [DllImport(DLL)] public static extern int     FmEngine_AddExtChip(
        IntPtr engine, int extChipType, uint clock, out uint chipId);
    [DllImport(DLL)] public static extern int     FmEngine_Write(
        IntPtr engine, uint chipId, byte reg, byte value, uint port);
    [DllImport(DLL)] public static extern int     FmEngine_SetGain(
        IntPtr engine, uint chipId, float gainL, float gainR);
    [DllImport(DLL)] public static extern int     FmEngine_GetGain(
        IntPtr engine, uint chipId, out float gainL, out float gainR);
    [DllImport(DLL)] public static extern int     FmEngine_SetMemory(
        IntPtr engine, uint chipId, int memType,
        byte[] data, uint size);
    [DllImport(DLL)] public static extern uint    FmEngine_GetMemorySize(
        IntPtr engine, uint chipId, int memType);
    [DllImport(DLL)] public static extern uint    Wasapi_GetDeviceCount();
    [DllImport(DLL)] public static extern int     Wasapi_GetDeviceId(
        uint index, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder buf, uint bufLen);
    [DllImport(DLL)] public static extern int     Wasapi_GetDeviceName(
        uint index, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder buf, uint bufLen);
    [DllImport(DLL)] public static extern int     Wasapi_IsDefaultDevice(uint index);
    [DllImport(DLL)] public static extern IntPtr  Wasapi_Create(IntPtr engine, int exclusive);
    [DllImport(DLL)] public static extern IntPtr  Wasapi_CreateWithDevice(
        IntPtr engine, int exclusive,
        [MarshalAs(UnmanagedType.LPWStr)] string deviceId);
    [DllImport(DLL)] public static extern uint    Wasapi_GetSampleRate(IntPtr wasapi);
    [DllImport(DLL)] public static extern void    Wasapi_Destroy(IntPtr wasapi);
    [DllImport(DLL)] public static extern int     Wasapi_Start(IntPtr wasapi);
    [DllImport(DLL)] public static extern int     Wasapi_Stop(IntPtr wasapi);
}
```

## WASAPI 動作モード

| モード | 説明 |
|---|---|
| Shared mode (`exclusive=0`) | デフォルト。`GetMixFormat` で取得したデバイスのネイティブフォーマットで初期化し、float32 出力を自前変換して書き込む。`AUTOCONVERTPCM` 非依存。 |
| Exclusive mode (`exclusive=1`) | デバイスを占有し最小レイテンシで動作。`IsFormatSupported` で非対応と判定された場合は Shared mode に自動降格する。 |

対応フォーマット: Float32 / Int16 / Int24 / Int32 (デバイスに応じて自動選択)

## 全チップテスト (sample_app)

`patches/` 以下に チップごとの JSON パッチファイルを用意しています。  
`sample_app.exe` はこれらを読み込み、各チャンネルを順番に発音します。

```bash
# デフォルト: patches/all.json (全チップ) を使用
sample_app.exe

# チップを指定
sample_app.exe patches/opna.json

# 複数ファイルを順番に処理
sample_app.exe patches/opl2.json patches/opna.json

# サンプルレートとデバイスを指定
sample_app.exe -r 44100 -d "Realtek" patches/all.json
```

オプション:

| オプション | 説明 |
|---|---|
| `-r <rate>` | サンプルレートを Hz で指定 (省略時 48000) |
| `-d <name>` | デバイス名を部分一致で指定 (省略時デフォルトデバイス) |

### JSON パッチフォーマット

```json
{
  "sample_rate": 48000,
  "global": { "note_ms": 800, "rest_ms": 200 },
  "chips": {
    "OPNA": {
      "gain_l": 0.9,
      "gain_r": 1.0,
      "init": [ {"reg": "0x29", "val": "0x80"} ],
      "channels": [
        {
          "ch": 0, "port": 0,
          "note_ms": 800,
          "init":    [ {"reg": "0xA0", "val": "0x8A", "port": 0} ],
          "key_on":  [ {"reg": "0x28", "val": "0xF0", "port": 0} ],
          "key_off": [ {"reg": "0x28", "val": "0x00", "port": 0} ]
        }
      ]
    },
    "OPM": { "$ref": "opm.json" }
  }
}
```

レジスタエントリの `"port"` は省略可能で、省略時はチャンネルレベルの `"port"` 値が使われます。  
OPL3/OPL4 は bank0 が `port=0`、bank1 が `port=1` です。

**ゲイン設定**: `"gain"` は L/R 共通値 (省略時 1.0)。`"gain_l"` / `"gain_r"` を指定すると左右別々のゲインになり、`"gain"` より優先されます。

**`"$ref"` による参照**: チップ定義の代わりに `{"$ref": "他のファイル名.json"}` を書くと、参照先ファイル内の `chips.<同名チップ>` の定義を読み込んで使用します。パスは参照元ファイルからの相対パスです。多重参照 (`$ref` の参照先がさらに `$ref` を持つ) や循環参照の検出にも対応しています。これにより `all.json` は各チップ用 JSON への参照だけで構成できます。

### patches/ ファイル一覧

| ファイル | チップ | FM ch | SSG ch | リズム ch | 備考 |
|---|---|---|---|---|---|
| `y8950.json` | Y8950 | 9+5\* | — | — | \*drums.sb リズム5ch |
| `opl.json`   | OPL (YM3526) | 9 | — | — | |
| `opl2.json`  | OPL2 (YM3812) | 9 | — | 5 | drums.sb |
| `opl3.json`  | OPL3 (YMF262) | 12 | — | 5 | bank0/1各6ch(4OP×3+2OP×3)、drums.sb |
| `opl4.json`  | OPL4 (YMF278B) | 12 | — | 5 | bank0/1各6ch(4OP×3+2OP×3)、drums.sb |
| `opll.json`  | OPLL (YM2413) | 9 | — | 5 | 内蔵リズム |
| `opllp.json` | OPLLP (YMF281) | 9 | — | 5 | 内蔵リズム |
| `opllx.json` | OPLLX (YM2423) | 9 | — | 5 | 内蔵リズム |
| `opn.json`   | OPN (YM2203) | 3 | 3 | — | |
| `opn2.json`  | OPN2 (YM2612) | 6 | — | — | |
| `opna.json`  | OPNA (YM2608) | 6 | 3 | 6 | ADPCM-A (要 ym2608.rom)、gain_l/gain_r 設定例 |
| `opnb.json`  | OPNB (YM2610) | 4 | 3 | — | CH0/3無効(仕様) |
| `opnbb.json` | OPNBB (YM2610B) | 6 | 3 | — | |
| `opm.json`   | OPM (YM2151) | 8 | — | — | |
| `opz.json`   | OPZ (YM2414) | 8 | — | — | |
| `vrc7.json`  | VRC7 (DS1001) | 6 | — | — | |
| `ssg.json`   | SSG (YM2149) | — | 3 | — | |
| `dcsg.json`  | DCSG (SN76489) | — | 3+1\* | — | \*3tone+1noise |
| `scc.json`   | SCC/K051649 | — | 5 | — | |
| `saa.json`   | SAA1099 | — | 6 | — | |
| `all.json`   | 全20チップ | — | — | — | 各チップ JSON への `$ref` 参照のみで構成 |

OPL3/OPL4 の FM 12ch は C4〜B4 の半音スケールです:

```
bank0: CH0+CH3 (4OP C4) → CH1+CH4 (4OP C#4) → CH2+CH5 (4OP D4)
       CH6 (2OP D#4) → CH7 (2OP E4) → CH8 (2OP F4)
bank1: CH9+CH12 (4OP F#4) → CH10+CH13 (4OP G4) → CH11+CH14 (4OP G#4)
       CH15 (2OP A4) → CH16 (2OP A#4) → CH17 (2OP B4)
```

## 対応チップ

### ymfm チップ (`FmEngine_AddChip`)

| C 定数 | チップ | 標準クロック | 主な用途 |
|---|---|---|---|
| `FM_CHIP_Y8950`  | Y8950   | 3.58 MHz  | MSX-Audio |
| `FM_CHIP_OPL`    | YM3526  | 3.58 MHz  | 初期 AdLib カード |
| `FM_CHIP_OPL2`   | YM3812  | 3.58 MHz  | AdLib, Sound Blaster |
| `FM_CHIP_OPL3`   | YMF262  | 14.32 MHz | Sound Blaster 16 |
| `FM_CHIP_OPL4`   | YMF278B | 33.87 MHz | OPL4 (ROM/RAM PCM 付き) |
| `FM_CHIP_OPN`    | YM2203  | 3.99 MHz  | PC-8801, PC-9801 |
| `FM_CHIP_OPNA`   | YM2608  | 7.99 MHz  | PC-8801mkIISR, PC-9801 |
| `FM_CHIP_OPNB`   | YM2610  | 8.00 MHz  | NEO GEO |
| `FM_CHIP_OPNBB`  | YM2610B | 8.00 MHz  | TAITO アーケード |
| `FM_CHIP_OPN2`   | YM2612  | 7.67 MHz  | Mega Drive, FM TOWNS |
| `FM_CHIP_OPM`    | YM2151  | 3.58 MHz  | SFG-01/05, アーケード |
| `FM_CHIP_OPLL`   | YM2413  | 3.58 MHz  | MSX2+, Sega Master System |
| `FM_CHIP_OPLLP`  | YMF281  | 3.58 MHz  | パチンコ・パチスロ |
| `FM_CHIP_OPLLX`  | YM2423  | 3.58 MHz  | FM Melody Maker, PMC100 |
| `FM_CHIP_OPZ`    | YM2414  | 3.58 MHz  | TX81Z |
| `FM_CHIP_VRC7`   | DS1001  | 3.58 MHz  | Lagrange Point (FC) |

### 外部ライブラリチップ (`FmEngine_AddExtChip`)

| C 定数 | チップ | 標準クロック | ライブラリ | JSON キー |
|---|---|---|---|---|
| `FM_CHIP_EXT_SSG`  | YM2149 (SSG) | 3.58 MHz | emu2149          | `"SSG"`  |
| `FM_CHIP_EXT_DCSG` | SN76489      | 3.58 MHz | emu76489         | `"DCSG"` |
| `FM_CHIP_EXT_SCC`  | SCC/K051649  | 3.58 MHz | emu2212          | `"SCC"`  |
| `FM_CHIP_EXT_SAA`  | SAA1099      | 8.00 MHz | SAASound.dll (動的) | `"SAA"` |

> **SAA1099**: `SAASound.dll` が実行時に `LoadLibrary` でロードされます。  
> `FmEngineApi.dll` と同じディレクトリに配置してください。

## 外部メモリ (ADPCM/PCM ROM)

ADPCM や PCM を内蔵するチップは外部 ROM/RAM からサンプルデータを読み取ります。

| チップ | 必要なメモリ種別 | 定数 |
|---|---|---|
| OPNA (YM2608)   | ADPCM-A リズム ROM | `FM_MEM_ADPCM_A` |
| OPNA (YM2608)   | ADPCM-B サンプル RAM/ROM | `FM_MEM_ADPCM_B` |
| OPNB (YM2610)   | ADPCM-A ROM, ADPCM-B ROM | `FM_MEM_ADPCM_A`, `FM_MEM_ADPCM_B` |
| OPNBB (YM2610B) | ADPCM-A ROM, ADPCM-B ROM | `FM_MEM_ADPCM_A`, `FM_MEM_ADPCM_B` |
| Y8950           | ADPCM-B RAM/ROM | `FM_MEM_ADPCM_B` |
| OPL4 (YMF278B)  | PCM サンプル ROM | `FM_MEM_PCM` |

```c
// ROM ファイルを読み込んでチップに設定する例
FILE* f = fopen("ym2608.rom", "rb");
fseek(f, 0, SEEK_END);  uint32_t sz = (uint32_t)ftell(f);  rewind(f);
uint8_t* buf = (uint8_t*)malloc(sz);
fread(buf, 1, sz, f);  fclose(f);

uint32_t opnaId;
FmEngine_AddChip(eng, FM_CHIP_OPNA, 0, &opnaId);
// Wasapi_Start() より前に設定すること
FmEngine_SetMemory(eng, opnaId, FM_MEM_ADPCM_A, buf, sz);

WasapiHandle wasapi = Wasapi_Create(eng, 0);
Wasapi_Start(wasapi);
// buf は Wasapi_Stop() まで解放しないこと
Wasapi_Stop(wasapi);
free(buf);
```

> **注意**: `FmEngine_SetMemory` はスレッドセーフではありません。`Wasapi_Start()` より前に呼んでください。

### sample_app での ROM 自動ロード

`sample_app.exe` は **実行ファイルと同じフォルダ** に以下のファイルが存在すれば自動的にロードします。  
ファイルが存在しない場合は ADPCM チャンネルが無音になるだけで、FM/SSG チャンネルは影響を受けません。

| ファイル名 | 対象チップ | 用途 |
|---|---|---|
| `ym2608.rom` | OPNA | ADPCM-A リズム ROM |
| `ym2610.rom` | OPNB, OPNBB | ADPCM-A ROM |
| `ym2610b.rom` | OPNB, OPNBB | ADPCM-B ROM |

ROMファイルの入手はエンドユーザーの責任で行ってください。

## 出力チャンネルの構成

`FmChip.h` の `generateNative` でチップごとに以下のミックス処理を行います。

| 分類 | 対象チップ | L 出力 | R 出力 |
|---|---|---|---|
| TrueStereo | OPM, OPN2, OPL3, OPL4 | data[0] (FM-L) | data[1] (FM-R) |
| OpnaStereo | OPNA, OPNB, OPNBB | data[0]+data[2] (FM-L+SSG) | data[1]+data[2] (FM-R+SSG) |
| OpnMono | OPN | data[0]+data[1]+data[2]+data[3] (FM+SSG×3) | 同左 |
| MixMono | OPL/OPL2/Y8950/OPLL系 | data[0]+data[1] (melody+rhythm) | 同左 |
| Mono | その他 | data[0] | 同左 |

OPL3/OPL4 のリズムチャンネルは仕様上 data[1] (R ch) にのみ出力されます。

## fnum 計算メモ

チップごとの FM 周波数番号 (fnum) 計算式:

### OPL 系 (OPL/OPL2/OPL3/Y8950)

```
fm_sr  = clk / (DEFAULT_PRESCALE * OPERATORS)
       = clk / 72   (OPL/OPL2/Y8950: prescale=4, ops=18)
       = clk / 288  (OPL3: prescale=8, ops=36)
       = clk / 684  (OPL4: prescale=19, ops=36)
fnum = freq × 2^(20−block) / fm_sr
```

### OPLL 系 (OPLL/OPLLP/OPLLX/VRC7)

```
fm_sr = 3579545 / 72 ≈ 49716 Hz
fnum = freq × 2^(19−block) / fm_sr   ← 指数が OPL と 1 異なる
fnum の最大値は 511 (9 bit)
```

### OPN 系 (OPN/OPN2/OPNA/OPNB/OPNBB/OPZ)

```
fm_sr  = clk / (prescale × OPERATORS)
       = clk / 72   (OPN:  prescale=6, CHANNELS=3, ops=12)
       = clk / 144  (OPNA/OPNB/OPNBB/OPN2: prescale=6, CHANNELS=6, ops=24)
fnum = freq × 2^(21−block) / fm_sr   ← 指数が OPL と 1 異なる
```

### OPM (YM2151)

OPM は fnum ではなく **key_code (KC)** 方式です。  
`0x28+ch` に key_code (`KC = (octave<<4)|note_code`)、`0x30+ch` に key_fraction (通常 0) を書きます。

note_code は12平均律と素直に対応しておらず、`s_phase_step` テーブル (`ymfm_fm.ipp`) を逆算しないと正確な周波数が得られません。実測した代表値:

| 音名 | KC (reg 0x28) | 実周波数 |
|---|---|---|
| C4 | `0x3E` | 261.6 Hz |
| D4 | `0x41` | 293.6 Hz |
| E4 | `0x44` | 329.6 Hz |
| F4 | `0x45` | 349.3 Hz |
| G4 | `0x48` | 391.9 Hz |
| A4 | `0x4A` | 439.9 Hz |
| B4 | `0x4D` | 493.9 Hz |
| C5 | `0x4E` | 523.2 Hz |

他の音程が必要な場合は `s_phase_step` テーブルを参照して `(octave, note_code, key_fraction)` を逆算すること。単純に `(octave<<4)|0,1,2,4,5,6,8,9,A,C,D,E` (半音ごとの理論値) を使うと最大で半音以上ずれる。

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **emu2149 / emu76489 / emu2212**: MIT (digital-sound-antiques)
- **SAASound**: GPL-2.0 (stripwax) — 配布時はライセンス条件を確認してください
- **nlohmann/json**: MIT (nlohmann)
- **このエンジンコード**: MIT
