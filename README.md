# YMEngine

**ymfm** をコアとした Windows 向け FM 音源エンジン。  
16種類のチップに対応し、WASAPI でリアルタイム再生します。

## ファイル構成

```
YMEngine/
├── CMakeLists.txt
├── extern/
│   └── ymfm/              ← git submodule (aaronsgiles/ymfm)
├── src/
│   ├── FmChip.h           ymfm ラッパー・LinearResampler
│   ├── FmEngine.h         複数チップ管理・SPSC キュー・ゲイン
│   ├── WasapiOutput.h     WASAPI リアルタイム出力・デバイス列挙
│   ├── FmEngineApi.h  ★  DLL 公開用 C ファサード (宣言)
│   ├── FmEngineApi.cpp★  DLL 公開用 C ファサード (実装)
│   ├── FmEngineApi.def★  MSVC エクスポート定義
│   ├── FmEngineApi.rc ★  DLL バージョン情報リソース
│   └── main.cpp           使用例 (FmEngineApi.h のみ使用)
├── README.md
└── README_ymfm.md         内部 C++ API リファレンス
```

## セットアップ

```bash
git clone https://github.com/madscient/YMEngine
cd YMEngine
git submodule update --init --recursive
```

## ビルド (Visual Studio 2022)

```bash
# 構成生成
cmake -B build -G "Visual Studio 17 2022" -A x64

# DLL + EXE をビルド
cmake --build build --config Release

# 成果物
#   build/bin/FmEngineApi.dll   ← DLL 本体
#   build/bin/FmEngineApi.pdb   ← デバッグシンボル
#   build/lib/FmEngineApi.lib   ← インポートライブラリ
#   build/bin/sample_app.exe    ← 使用例
```

## DLL の使い方

利用側は **`FmEngineApi.h` だけを include** し、`FmEngineApi.lib` にリンクします。  
`FmEngine.h` / `WasapiOutput.h` / ymfm ヘッダは不要です。

```c
#include "FmEngineApi.h"
#pragma comment(lib, "FmEngineApi.lib")

// エンジン作成 (48000 Hz)
FmEngineHandle eng = FmEngine_Create(48000);

// チップ追加 (clock=0 で標準クロック自動選択)
uint32_t opnaId;
FmEngine_AddChip(eng, FM_CHIP_OPNA, 0, &opnaId);

// ゲイン設定 (1.0 = 0 dB)
FmEngine_SetGain(eng, opnaId, 1.0f, 1.0f);

// WASAPI 再生 (デフォルトデバイス、Shared mode)
WasapiHandle wasapi = Wasapi_Create(eng, 0);
Wasapi_Start(wasapi);

// レジスタ書き込み (任意スレッドから安全)
FmEngine_Write(eng, opnaId, 0xB4, 0xC0, 0); // CH0 L/R ON

// 停止・解放
Wasapi_Stop(wasapi);
Wasapi_Destroy(wasapi);
FmEngine_Destroy(eng);
```

### オーディオデバイスの明示指定

複数のオーディオデバイスがある場合、デバイスを列挙して明示的に指定できます。

```c
// デバイス列挙 (Wasapi_GetDeviceCount 呼び出しでリストが更新される)
uint32_t count = Wasapi_GetDeviceCount();
for (uint32_t i = 0; i < count; ++i) {
    wchar_t id[256] = {}, name[256] = {};
    Wasapi_GetDeviceId(i, id, 256);
    Wasapi_GetDeviceName(i, name, 256);
    int isDefault = Wasapi_IsDefaultDevice(i);
    // name, id, isDefault を使って選択 UI などに表示する
}

// デバイスIDを指定して作成
WasapiHandle wasapi = Wasapi_CreateWithDevice(eng, 0, id);
```

### C# (P/Invoke) からの使用例

```csharp
using System.Runtime.InteropServices;

static class FmEngineApi {
    const string DLL = "FmEngineApi";

    [DllImport(DLL)] public static extern IntPtr  FmEngine_Create(uint sampleRate);
    [DllImport(DLL)] public static extern void    FmEngine_Destroy(IntPtr engine);
    [DllImport(DLL)] public static extern int     FmEngine_AddChip(
        IntPtr engine, int chipType, uint clock, out uint chipId);
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
    [DllImport(DLL)] public static extern void    Wasapi_Destroy(IntPtr wasapi);
    [DllImport(DLL)] public static extern int     Wasapi_Start(IntPtr wasapi);
    [DllImport(DLL)] public static extern int     Wasapi_Stop(IntPtr wasapi);
}

// デフォルトデバイスで再生
var eng    = FmEngineApi.FmEngine_Create(48000);
FmEngineApi.FmEngine_AddChip(eng, 6 /*FM_CHIP_OPNA*/, 0, out uint id);
var wasapi = FmEngineApi.Wasapi_Create(eng, 0);
FmEngineApi.Wasapi_Start(wasapi);
```

## WASAPI 動作モード

| モード | 説明 |
|---|---|
| Shared mode (`exclusive=0`) | デフォルト。他アプリと音声を共有。デバイスのミックスフォーマットで初期化し、float32 出力を自前変換して書き込む。 |
| Exclusive mode (`exclusive=1`) | デバイスを占有し最小レイテンシで動作。フォーマット非対応時は自動的に Shared mode に降格する。 |

Shared / Exclusive どちらのモードも `GetMixFormat` で取得したデバイスのネイティブフォーマット (Float32 / Int16 / Int24 / Int32) に自前変換して書き込むため、`AUTOCONVERTPCM` に依存しません。

## 対応チップ

| C 定数 (FmChipType) | C++ 列挙 (ChipType) | チップ | 標準クロック | 主な用途 |
|---|---|---|---|---|
| `FM_CHIP_Y8950`  | `ChipType::Y8950`  | Y8950    | 3.58 MHz  | MSX-Audio |
| `FM_CHIP_OPL`    | `ChipType::OPL`    | YM3526   | 3.58 MHz  | 初期 AdLib カード |
| `FM_CHIP_OPL2`   | `ChipType::OPL2`   | YM3812   | 3.58 MHz  | AdLib, Sound Blaster |
| `FM_CHIP_OPL3`   | `ChipType::OPL3`   | YMF262   | 14.3 MHz  | Sound Blaster 16 |
| `FM_CHIP_OPL4`   | `ChipType::OPL4`   | YMF278B  | 16.93 MHz | OPL4 (ROM/RAM PCM 付き) |
| `FM_CHIP_OPN`    | `ChipType::OPN`    | YM2203   | 3.99 MHz  | PC-8801, PC-9801 |
| `FM_CHIP_OPNA`   | `ChipType::OPNA`   | YM2608   | 7.99 MHz  | PC-8801mkIISR, PC-9801 |
| `FM_CHIP_OPNB`   | `ChipType::OPNB`   | YM2610   | 8.00 MHz  | NEO GEO |
| `FM_CHIP_OPNBB`  | `ChipType::OPNBB`  | YM2610B  | 8.00 MHz  | TAITO アーケード |
| `FM_CHIP_OPN2`   | `ChipType::OPN2`   | YM2612   | 7.67 MHz  | Mega Drive, FM TOWNS |
| `FM_CHIP_OPM`    | `ChipType::OPM`    | YM2151   | 3.58 MHz  | SFG-01/05, アーケード |
| `FM_CHIP_OPLL`   | `ChipType::OPLL`   | YM2413   | 3.58 MHz  | MSX2+, Sega Master System |
| `FM_CHIP_OPLLP`  | `ChipType::OPLLP`  | YMF281   | 3.58 MHz  | パチンコ・パチスロ |
| `FM_CHIP_OPLLX`  | `ChipType::OPLLX`  | YM2423   | 3.58 MHz  | FM Melody Maker, PMC100 |
| `FM_CHIP_OPZ`    | `ChipType::OPZ`    | YM2414   | 3.58 MHz  | TX81Z |
| `FM_CHIP_VRC7`   | `ChipType::VRC7`   | DS1001   | 3.58 MHz  | Lagrange Point (FC) |

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **このエンジンコード**: MIT
