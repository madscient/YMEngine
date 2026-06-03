# ymfm_engine

**ymfm** をコアとした Windows 向け FM 音源エンジン。  
複数チップ (OPL2/OPL3/OPN2/OPM) を同時に扱い、WASAPI でリアルタイム再生します。

## 構成

```
ymfm_engine/
├── CMakeLists.txt
├── extern/
│   └── ymfm/          ← git submodule
└── src/
    ├── FmChip.h       ymfm ラッパー (チップ抽象化)
    ├── FmEngine.h     複数チップ管理 + SPSC キュー
    ├── WasapiOutput.h WASAPI リアルタイム出力
    └── main.cpp       使用例
```

## セットアップ

### 1. リポジトリとサブモジュール取得

```bash
git init ymfm_engine && cd ymfm_engine
git submodule add https://github.com/aaronsgiles/ymfm extern/ymfm
```

### 2. ビルド (Visual Studio 2022 / ninja)

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

または Ninja:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 3. 実行

```bash
./build/Release/ymfm_engine.exe
```

## 基本的な使い方

```cpp
// ① エンジンを 44100 Hz で作成
FmEngine engine(44100);

// ② 使いたいチップを追加
uint32_t oplId = engine.addChip(ChipType::OPL3);
uint32_t opnId = engine.addChip(ChipType::OPN2);

// ③ WASAPI 出力を開く (false = Shared mode)
WasapiOutput output(engine, false);
output.start();

// ④ レジスタを書き込む (任意スレッドから呼べる)
engine.write(oplId, 0xB0, 0x34); // Key-on

// ⑤ 停止
output.stop();
```

## スレッドモデル

```
[ゲームスレッド]   engine.write()  →  SPSC キュー
[WASAPIスレッド]   engine.generate()  ←  SPSC キューを消化 → チップ合算 → WASAPI
```

`write()` と `generate()` はロックフリーキューで分離されているため、
レジスタ書き込みはオーディオスレッドをブロックしません。

## 対応チップ

| 列挙値 | チップ | クロック | 用途 |
|---|---|---|---|
| `ChipType::OPL2` | YM3812 | 3.58 MHz | PC-88, SoundBlaster |
| `ChipType::OPL3` | YMF262 | 14.3 MHz | SoundBlaster 16 |
| `ChipType::OPN2` | YM2612 | 7.67 MHz | Mega Drive |
| `ChipType::OPM`  | YM2151 | 3.58 MHz | アーケード |

## Exclusive mode

```cpp
WasapiOutput output(engine, /*exclusive=*/true);
```

排他モードではドライバが最小レイテンシ (1〜3 ms 程度) で動作します。  
対応デバイスの確認は「サウンドの詳細プロパティ → 排他モードを許可する」で行ってください。

## ライセンス

- **ymfm**: BSD 3-Clause (Aaron Giles)
- **このエンジンコード**: MIT
