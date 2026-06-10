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
│   ├── ExternalChip.h     外部ライブラリチップ (PSG/SN76489/SCC/SAA1099)
│   ├── WasapiOutput.h     WASAPI リアルタイム出力・デバイス列挙
│   ├── FmEngineApi.h  ★  DLL 公開用 C ファサード (宣言)
│   ├── FmEngineApi.cpp★  DLL 公開用 C ファサード (実装)
│   ├── FmEngineApi.def★  MSVC エクスポート定義
│   ├── FmEngineApi.rc ★  DLL バージョン情報リソース
│   ├── main.cpp           全チップ全チャンネルテスト (JSON 駆動)
│   └── test_patches.json  テスト用レジスタパッチ定義
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
#   build/bin/FmEngineApi.dll   ← DLL 本体
#   build/bin/FmEngineApi.pdb   ← デバッグシンボル
#   build/lib/FmEngineApi.lib   ← インポートライブラリ
#   build/bin/SAASound.dll      ← SAA1099 用 DLL (オプション)
#   build/bin/sample_app.exe    ← 全チップテストアプリ
#   build/bin/test_patches.json ← テスト用パッチ定義
```

> **Note**: SAASound.dll は `FmEngineApi.dll` と同じディレクトリに配置してください。  
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
uint32_t psgId;
FmEngine_AddExtChip(eng, FM_CHIP_EXT_PSG, 0, &psgId);

// ゲイン設定 (1.0 = 0 dB)
FmEngine_SetGain(eng, opnaId, 1.0f, 1.0f);
FmEngine_SetGain(eng, psgId,  0.7f, 0.7f);

// WASAPI 再生 (デフォルトデバイス、Shared mode)
WasapiHandle wasapi = Wasapi_Create(eng, 0);
Wasapi_Start(wasapi);

// レジスタ書き込み (任意スレッドから安全)
// ymfm 系: write(chip_id, reg, value, port)
FmEngine_Write(eng, opnaId, 0xB4, 0xC0, 0);
// PSG 系:  write(chip_id, reg_num, value, port=0)
FmEngine_Write(eng, psgId,  0x08, 0x0F, 0);

// 停止・解放
Wasapi_Stop(wasapi);
Wasapi_Destroy(wasapi);
FmEngine_Destroy(eng);
```

### オーディオデバイスの明示指定

複数のオーディオデバイスがある場合、デバイスを列挙して明示的に指定できます。

```c
// デバイス列挙 (呼び出しのたびにリストが更新される)
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

`test_patches.json` にレジスタパッチを記述し、全チップ全チャンネルを順番に鳴らします。

```bash
# デフォルト設定で実行
sample_app.exe

# パッチファイルを指定
sample_app.exe my_patches.json
```

`test_patches.json` の構造:

```json
{
  "sample_rate": 48000,
  "global": { "note_ms": 800, "rest_ms": 200 },
  "chips": {
    "OPNA": {
      "gain": 1.0,
      "init": [ {"reg": "0x22", "val": "0x00"} ],
      "channels": [
        {
          "ch": 0, "port": 0,
          "note_ms": 800,
          "init":    [ {"reg": "0xA0", "val": "0x8A"} ],
          "key_on":  [ {"reg": "0x28", "val": "0xF0"} ],
          "key_off": [ {"reg": "0x28", "val": "0x00"} ]
        }
      ]
    },
    "PSG": { ... }
  }
}
```

チップ名キー (`"OPNA"`, `"PSG"` 等) は以下の識別子と対応します。

## 対応チップ

### ymfm チップ (`FmEngine_AddChip`)

| C 定数 | チップ | 標準クロック | 主な用途 |
|---|---|---|---|
| `FM_CHIP_Y8950`  | Y8950   | 3.58 MHz  | MSX-Audio |
| `FM_CHIP_OPL`    | YM3526  | 3.58 MHz  | 初期 AdLib カード |
| `FM_CHIP_OPL2`   | YM3812  | 3.58 MHz  | AdLib, Sound Blaster |
| `FM_CHIP_OPL3`   | YMF262  | 14.3 MHz  | Sound Blaster 16 |
| `FM_CHIP_OPL4`   | YMF278B | 16.93 MHz | OPL4 (ROM/RAM PCM 付き) |
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
| `FM_CHIP_EXT_SSG`     | YM2149 (SSG) | 3.58 MHz | emu2149   | `"PSG"` |
| `FM_CHIP_EXT_DCSG`    | SN76489      | 3.58 MHz | emu76489  | `"SN76489"` |
| `FM_CHIP_EXT_SCC`     | SCC/K051649  | 3.58 MHz | emu2212   | `"SCC"` |
| `FM_CHIP_EXT_SAA`     | SAA1099      | 8.00 MHz | SAASound.dll (動的ロード) | `"SAA1099"` |

> **SAA1099 の注意**: `SAASound.dll` が実行時に `LoadLibrary` でロードされます。  
> `FmEngineApi.dll` と同じディレクトリに配置してください。

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **emu2149 / emu76489 / emu2212**: MIT (digital-sound-antiques)
- **SAASound**: GPL-2.0 (stripwax) — 配布時はライセンス条件を確認してください
- **nlohmann/json**: MIT (nlohmann)
- **このエンジンコード**: MIT

## 外部メモリ (ADPCM/PCM ROM)

ADPCM や PCM を内蔵するチップは外部 ROM/RAM からサンプルデータを読み取ります。  
`FmEngine_SetMemory` で ROM データを設定してください。

| チップ | 必要なメモリ種別 | 用途 |
|---|---|---|
| OPNA (YM2608)  | `FM_MEM_ADPCM_B` | ADPCM-B サンプル |
| OPNB (YM2610)  | `FM_MEM_ADPCM_A`, `FM_MEM_ADPCM_B` | ADPCM-A / ADPCM-B サンプル |
| OPNBB (YM2610B)| `FM_MEM_ADPCM_A`, `FM_MEM_ADPCM_B` | ADPCM-A / ADPCM-B サンプル |
| Y8950          | `FM_MEM_ADPCM_B` | ADPCM-B サンプル |
| OPL4 (YMF278B) | `FM_MEM_PCM`     | PCM サンプル ROM |

```c
// ROM ファイルを読み込む
FILE* f = fopen("adpcm_rom.bin", "rb");
fseek(f, 0, SEEK_END);
uint32_t romSize = (uint32_t)ftell(f);
rewind(f);
uint8_t* romData = (uint8_t*)malloc(romSize);
fread(romData, 1, romSize, f);
fclose(f);

// チップ追加後、Wasapi_Start() より前に設定する
uint32_t opnaId;
FmEngine_AddChip(eng, FM_CHIP_OPNA, 0, &opnaId);
FmEngine_SetMemory(eng, opnaId, FM_MEM_ADPCM_B, romData, romSize);

// 設定済みサイズの確認
uint32_t sz = FmEngine_GetMemorySize(eng, opnaId, FM_MEM_ADPCM_B);
printf("ADPCM-B ROM: %u bytes\n", sz);

// 再生開始
WasapiHandle wasapi = Wasapi_Create(eng, 0);
Wasapi_Start(wasapi);

// romData は Wasapi_Stop() まで解放しないこと
Wasapi_Stop(wasapi);
free(romData);
```

> **注意**: `FmEngine_SetMemory` はスレッドセーフではありません。`Wasapi_Start()` より前に呼んでください。
