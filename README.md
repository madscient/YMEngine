# YMEngine

**ymfm** をコアとした FM 音源エンジン DLL。  
**FmEngineApi** インターフェースに準拠した ymfm のラッパー実装です。

ymfm が対応する16種のチップをサポートし、チップ名文字列 (`"OPNA"`, `"OPL2"` 等) とクロックを指定してインスタンスを作成できます。DLL はオーディオ出力機能を持ちません。アプリケーションのオーディオコールバックから `FmEngine_Generate()` を呼び出すことで波形データを取得します。

SSG/PSG や PCM 音源など ymfm がカバーしないチップはスコープ外です。それらを組み合わせる場合はアプリケーション側の責任で別途統合してください。

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
├── README.md
└── README_ymfm.md         DLL を介さず C++ から直接使う場合の API リファレンス
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
build/bin/YMFMEngine.dll   ← DLL 本体
build/bin/YMFMEngine.pdb   ← デバッグシンボル
build/lib/YMFMEngine.lib   ← インポートライブラリ
```

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 対応チップ

`FmEngine_Inquiry` / `FmEngine_GetSupportedChip` で実行時に取得できます。

`FmEngine_AddChip` には、チップのマスタークロック (Hz) を必ず指定します。0 を渡すと `FM_ERR_INVALID_ARG` を返します。同じチップでも機種によってクロックが違い、F-Number などのレジスタ値はクロックを前提に計算するため、エンジンは既定のクロックを持ちません。

```c
uint32_t opna_id;
FmEngine_AddChip(engine, "OPNA", 7987200, &opna_id);
```

下の表のクロックは例です。

| チップ名 | 実チップ | クロックの例 | 主な用途 |
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
| OPM, OPN2 | FM-L | FM-R |
| OPL3 | A + C | B + D |
| OPL4 | DO2-L + DO0-L + DO1-L | DO2-R + DO0-R + DO1-R |
| OPNA, OPNB, OPNBB | FM-L + SSG | FM-R + SSG |
| OPN | FM + SSG (モノラル) | 同左 |
| OPLL系 (VRC7 を含む) | メロディ + リズム | 同左 |
| OPL/OPL2/Y8950 | FM (リズムを含む) + ADPCM (Y8950 のみ)。モノラル | 同左 |
| その他 | data[0] | 同左 |

各出力には部位ごとのゲインが掛かります (「部位ごとのゲイン」を参照)。OPL3 の C/D と OPL4 の DO0/DO1 は、ゲインの既定値が 0 なので、既定では混ざりません。

OPL3 (YMF262) の FM は、チャンネルごとに出力先 A/B/C/D をレジスタ `0xC0`〜 の bit4-7 で選びます (port1 の `0x05` の NEW を立てたとき)。

OPL4 (YMF278B) は3系統のステレオ出力を持ちます。

- DO0: FM の C/D
- DO1: AWM の C/D
- DO2: FM の A/B と AWM の A/B をチップ内でミックスしたもの。FM と AWM の比率はチップのレジスタ (port2 の `0xF8`/`0xF9`) で決まります

AWM は、チャンネルごとにレジスタ `0x68`〜 (port2) の bit4 で A/B (0) と C/D (1) のどちらに出すかを選びます。

OPN, OPNA, OPNB, OPNBB の FM と SSG は、それぞれ本来のサンプルレートで生成してから出力レートに変換し、足し合わせます。FM には ADPCM とリズムが含まれます。SSG は3チャンネルの和で、OPNA/OPNB/OPNBB では和に 2/3 を掛けます。

## 部位ごとのゲイン

チップによっては、音を複数の端子から別々に出します。実機ではそれらをボード上の回路でミックスしたり、一部の端子だけを配線したりするため、音量バランスは機種によって異なります。`FmEngine_SetPartGain` で部位ごとにゲインを設定できます。

```c
FmEngine_SetPartGain(engine, opna_id, FM_PART_OPN_SSG, 0.5f, 0.5f);  // SSG を -6 dB
FmEngine_SetPartGain(engine, opl3_id, FM_PART_OPL3_CD, 1.0f, 1.0f);  // C/D も鳴らす
```

| 部位 | 対象チップ | 内容 | 既定値 |
|---|---|---|---|
| `FM_PART_OPN_FM`      | OPN, OPNA, OPNB, OPNBB | FM 部 (ADPCM・リズムを含む) | 1.0 |
| `FM_PART_OPN_SSG`     | OPN, OPNA, OPNB, OPNBB | SSG 部 | 1.0 |
| `FM_PART_OPLL_MELODY` | OPLL, OPLLP, OPLLX, VRC7 | メロディ | 1.0 |
| `FM_PART_OPLL_RHYTHM` | OPLL, OPLLP, OPLLX, VRC7 | リズム | 1.0 |
| `FM_PART_OPL3_AB`     | OPL3 | 出力 A (L) / B (R) | 1.0 |
| `FM_PART_OPL3_CD`     | OPL3 | 出力 C (L) / D (R) | 0 |
| `FM_PART_OPL4_DO0`    | OPL4 | DO0 (FM の C/D) | 0 |
| `FM_PART_OPL4_DO1`    | OPL4 | DO1 (AWM の C/D) | 0 |
| `FM_PART_OPL4_DO2`    | OPL4 | DO2 (FM の A/B と AWM の A/B のミックス) | 1.0 |

実際に掛かるゲインは、`FmEngine_SetGain` で設定したチップ全体のゲインと部位のゲインの積です。チップが持たない部位を指定すると `FM_ERR_INVALID_ARG` を返します。出力が1系統のチップ (OPL, OPL2, Y8950, OPN2, OPM, OPZ) は部位を持たないので、`FmEngine_SetGain` を使ってください。

チップが持つ部位は `FmEngine_GetPartMask` で調べられます。bit n が `FmPart` の n 番に当たり、部位を持たないチップでは 0 です。

```c
uint32_t mask = 0;
FmEngine_GetPartMask(engine, opna_id, &mask);
if (mask & (1u << FM_PART_OPN_SSG)) {
    // SSG のゲインを設定できる
}
```

C/D 側 (`FM_PART_OPL3_CD`, `FM_PART_OPL4_DO0`, `FM_PART_OPL4_DO1`) の既定値が 0 なのは、FM の出力先を A/B/C/D 全部にしたチャンネルが A/B と C/D に同じ音を出し、混ぜると二重に足されるためです。

## ネイティブサンプルレート

`FmEngine_GetNativeRate` は FM 部のサンプルレート (Hz、端数切り捨て) を返します。OPN と OPNA では prescale レジスタ (`0x2D`〜`0x2F`) の書き込みで変わります。

| チップ | prescale 6 (リセット時) | 3 | 2 |
|---|---|---|---|
| OPN  | clk / 72  | clk / 36 | clk / 24 |
| OPNA | clk / 144 | clk / 72 | clk / 48 |
| OPNB, OPNBB | clk / 144 (prescale なし) | | |

## 外部メモリ

ADPCM と PCM を持つチップは、アプリケーションが用意したメモリのブロックを読み書きします。ブロックは複製せずに参照するので、割り当てを外すか `FmEngine_Destroy` が戻るまで解放しないでください。割り当てはオーディオストリームを始める前に行ってください (スレッドセーフではありません)。

| `FmMemoryType` | チップ | 内容 |
|---|---|---|
| `FM_MEM_ADPCM_A`         | OPNA | リズム音の内蔵 ROM の内容 |
| `FM_MEM_ADPCM_A`         | OPNB, OPNBB | ADPCM-A のメモリ |
| `FM_MEM_ADPCM_B`         | OPNA, Y8950 | ADPCM-B の ROM/RAM 選択ビットが RAM のときにアクセスするメモリ |
| `FM_MEM_ADPCM_B`         | OPNB, OPNBB | ADPCM-B のメモリ |
| `FM_MEM_ADPCM_B_ROMMODE` | OPNA, Y8950 | ADPCM-B の ROM/RAM 選択ビットが ROM のときにアクセスするメモリ |
| `FM_MEM_PCM`             | OPL4 | PCM のメモリ |

OPNA と Y8950 は、ROM/RAM 選択ビット (OPNA は port1 の `0x01` の bit0、Y8950 は `0x08` の bit0) で、ROM モードと RAM モードの別々のメモリにアクセスします。ROM モードで鳴らすデータは `FM_MEM_ADPCM_B_ROMMODE` に割り当ててください。`FM_MEM_ADPCM_B` に割り当てたデータは RAM モードでだけ読まれます。

### FmEngine_SetMemoryEx

```c
// Y8950: RAM モードのメモリに 32KB の RAM、ROM モードのメモリに ROM イメージ
FmEngine_SetMemoryEx(engine, y8950_id, FM_MEM_ADPCM_B, 0, ram, 32768, FM_ACCESS_RAM);
FmEngine_SetMemoryEx(engine, y8950_id, FM_MEM_ADPCM_B_ROMMODE, 0, rom, rom_size, FM_ACCESS_ROM);
```

- `[base, base + size)` に `data` を割り当てます。番地 `base + i` のバイトが `data[i]` です。範囲が重ならなければ、1つのメモリに複数のブロックを並べられます (OPL4 の ROM と SRAM など)。
- 割り当ての無い番地を読むと 0 で、書き込みは捨てます。
- `data` に `NULL` を渡すと、`[base, base + size)` と重なるブロックをすべて外します。
- `FM_ACCESS_RAM` のブロックには、チップの書き込み (レジスタ経由の転送) をその場で書きます。`FM_ACCESS_ROM` のブロックへの書き込みは捨てます。
- ROM/RAM 選択ビットが ROM の間にレジスタ経由で転送したデータは、`FM_MEM_ADPCM_B_ROMMODE` に書き込みます。そこに割り当てたブロックが `FM_ACCESS_RAM` なら、ブロックに入ります。
- 番地はチップが出すアドレスで、1番地が1バイトです。Y8950 の RAM のビット単位の並び (8個の D-RAM への振り分け) は再現せず、チップが読み書きするバイトを番地の順に並べます。
- 未知の chip_id、チップが持たない `mem_type`、`size` が 0、`base + size` が 2^32 を越える、既存のブロックと範囲が重なる、未知の `access` のときは `FM_ERR_INVALID_ARG` を返します。

### FmEngine_SetMemory / FmEngine_GetMemorySize

`FmEngine_SetMemory` は、`mem_type` のメモリを `[0, size)` の `data` だけにします (それまでのブロックは外れます)。チップからの書き込みは捨てます。`FM_MEM_ADPCM_B_ROMMODE` は受け付けません。

`FmEngine_GetMemorySize` は、割り当てたブロックの大きさの合計を返します。

### 書き込みが反映される時点

`FmEngine_Write` の書き込みは、次の `FmEngine_Generate` の中でチップに反映します。チップがレジスタ経由の転送でメモリに書いた値は、その書き込みを反映した `FmEngine_Generate` が戻った時点で、`FM_ACCESS_RAM` のブロックに入っています。

同じチャンネルのキー状態を、間に `FmEngine_Generate` を挟まずに2回以上変えると、2回目以降の書き込みは前の状態を約2ms 生成してから反映します。その間は後ろに並んだ書き込みも待つので、転送の反映が次以降の `FmEngine_Generate` に持ち越されることがあります。

エンジンがブロックを読み書きするのは `FmEngine_Generate` の実行中だけです。ブロックを別のデバイスと共有する場合は、`FmEngine_Generate` の実行中に別のスレッドからブロックに触らないようにしてください。

## チップ固有のレジスタの扱い

- **OPL2**: 波形選択 (`0xE0`〜) は、`0x01` の bit5 (WSE) を立てたときだけ有効です。立てていなければ全オペレータが正弦波になります。OPL3/OPL4 では常に有効です。
- **OPN 系**: F-Number/Block は、上位 (`0xA4`〜) を書くとラッチされ、下位 (`0xA0`〜) を書いた時点でラッチ中の上位と一緒に反映されます。ラッチは直前の上位の値を保持し続けるため、下位だけを書き換えることもできます。OPNA などの2バンクのチップでは、ラッチは両バンクで共有です。上位と下位は同じチャンネルに続けて書いてください。

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
fm_sr = clk / 72   (clk = 3,579,545 Hz なら ≈ 49716 Hz)
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
