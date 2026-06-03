# ymfm_engine

**ymfm** をコアとした Windows 向け FM 音源エンジン。  
複数チップ (OPL2/OPL3/OPN2/OPM) を同時に扱い、WASAPI でリアルタイム再生します。

## ファイル構成

```
ymfm_engine/
├── CMakeLists.txt
├── extern/
│   └── ymfm/                   ← git submodule
└── src/
    ├── FmChip.h                ymfm ラッパー・LinearResampler
    ├── FmEngine.h              複数チップ管理・SPSC キュー・ゲイン
    ├── WasapiOutput.h          WASAPI リアルタイム出力
    ├── FmEngineApi.h      ★   DLL 公開用 C ファサード (宣言)
    ├── FmEngineApi.cpp    ★   DLL 公開用 C ファサード (実装)
    ├── FmEngineApi.def    ★   MSVC エクスポート定義
    ├── FmEngineApi.rc     ★   DLL バージョン情報リソース
    └── main.cpp                使用例 (FmEngineApi.h のみ使用)
```

## セットアップ

```bash
git init ymfm_engine && cd ymfm_engine
git submodule add https://github.com/aaronsgiles/ymfm extern/ymfm
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
#pragma comment(lib, "FmEngineApi.lib")  // または CMake で target_link_libraries

// エンジン作成
FmEngineHandle eng = FmEngine_Create(48000);

// チップ追加 (clock=0 で標準クロック自動選択)
uint32_t opl3Id;
FmEngine_AddChip(eng, FM_CHIP_OPL3, 0, &opl3Id);

// ゲイン設定 (1.0 = 0 dB)
FmEngine_SetGain(eng, opl3Id, 1.0f, 1.0f);

// WASAPI 再生
WasapiHandle wasapi = Wasapi_Create(eng, 0 /*Shared mode*/);
Wasapi_Start(wasapi);

// レジスタ書き込み (任意スレッドから安全)
FmEngine_Write(eng, opl3Id, 0xB0, 0x34, 0); // Key-on

// 停止・解放
Wasapi_Stop(wasapi);
Wasapi_Destroy(wasapi);
FmEngine_Destroy(eng);
```

### C# (P/Invoke) からの使用例

```csharp
using System.Runtime.InteropServices;

static class FmEngineApi {
    const string DLL = "FmEngineApi";

    [DllImport(DLL)] public static extern IntPtr FmEngine_Create(uint sampleRate);
    [DllImport(DLL)] public static extern void   FmEngine_Destroy(IntPtr engine);
    [DllImport(DLL)] public static extern int    FmEngine_AddChip(
        IntPtr engine, int chipType, uint clock, out uint chipId);
    [DllImport(DLL)] public static extern int    FmEngine_Write(
        IntPtr engine, uint chipId, byte reg, byte value, uint port);
    [DllImport(DLL)] public static extern int    FmEngine_SetGain(
        IntPtr engine, uint chipId, float gainL, float gainR);
    [DllImport(DLL)] public static extern IntPtr Wasapi_Create(IntPtr engine, int exclusive);
    [DllImport(DLL)] public static extern void   Wasapi_Destroy(IntPtr wasapi);
    [DllImport(DLL)] public static extern int    Wasapi_Start(IntPtr wasapi);
    [DllImport(DLL)] public static extern int    Wasapi_Stop(IntPtr wasapi);
}

var eng    = FmEngineApi.FmEngine_Create(48000);
FmEngineApi.FmEngine_AddChip(eng, 1 /*OPL3*/, 0, out uint id);
var wasapi = FmEngineApi.Wasapi_Create(eng, 0);
FmEngineApi.Wasapi_Start(wasapi);
```

## 対応チップ

| 定数 | チップ | 標準クロック |
|---|---|---|
| `FM_CHIP_OPL2` | YM3812 | 3.58 MHz |
| `FM_CHIP_OPL3` | YMF262 | 14.3 MHz |
| `FM_CHIP_OPN2` | YM2612 | 7.67 MHz |
| `FM_CHIP_OPM`  | YM2151 | 3.58 MHz |

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **このエンジンコード**: MIT
