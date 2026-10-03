# CLAUDE.md

AI 向けの作業メモ。人間向けの文書は `README.md`（DLL 利用者）と
`README_ymfm.md`（C++ から直接使う開発者）。

## 文書の置き場所

- `doc/CHANGELOG.md` — 開発経緯。方針の前提、見送った案、確認結果を書く
- 回帰テストと、その実行方法はこのファイルに書く。README からは参照しない

## FmEngineApi のヘッダと仕様

`src/FmEngineApi.h` は写しで、役割は「複数のエンジンとアプリケーションが共有する
C インターフェースの宣言」。正本は https://github.com/madscient/FMEngineTest の
`include/FmEngineApi.h`、仕様書は同じリポジトリの `docs/FmEngineApi.md`。

- 写しは直接編集しない。仕様が改訂されたら正本で丸ごと差し替え、
  `git hash-object src/FmEngineApi.h` が正本の blob（FMEngineTest で
  `git rev-parse HEAD:include/FmEngineApi.h`）と一致することを確かめる
- 改訂のたびに各エンジンが直すことは、FMEngineTest の `docs/CHANGELOG.md` に
  書いてある
- ヘッダのコメントは特定のエンジンに依らない。YMEngine に固有の挙動（ブロックを
  複製しない、書き込みが反映される時点、仕様が定めていない引数の扱い）は
  `README.md` に書く

## 回帰テスト (`_test/`)

CMake には組み込んでいない。`c_api_test` 以外は、ヘッダと ymfm のソースから直接
ビルドする。

| ファイル | 見ていること |
|---|---|
| `keyoff_retrigger_test.cpp` | 同じチャンネルへの KEY OFF → KEY ON が短い間隔で続いても、KEY OFF が観測されること（OPL3・OPNA・OPM・OPLL・OPL4 AWM。batch / crowd / tiny の3条件） |
| `opn_split_test.cpp` | `LinearResampler` が呼び出しをまたいでソースを読み捨てないこと。OPN 系の `detail::*Split` の FM/SSG が上流 `generate()`（FIDELITY_MAX）と全サンプル一致すること（prescale 切り替えを含む。書き込みを抜いた対照で不一致が出ること）。`FmEngine` のネイティブレート・既定の SSG 音量・部位ゲイン |
| `part_gain_test.cpp` | OPLL 系・OPL3・OPL4 の部位ゲインが ymfm のどの出力に掛かるか（ネイティブレートで上流と全サンプル比較。別の出力と比べた対照で不一致が出ること）。既定値で今までの出力（OPLL はメロディ+リズム、OPL3 は A/B、OPL4 は DO2）になること。部位を持たないチップにチップのゲインが掛かること。全チップ × 全部位の受け付けと既定値。部位の列挙と名前からの検索が、仕様の表の名前と一致すること |
| `chip_clock_test.cpp` | 全チップで clock=0 を拒否し、チップが増えないこと（`addChip()` / `addChipByName()` / `createChip()` / `createChipByName()`）。渡したクロックがそのままチップに渡り、ネイティブレートがクロックに比例すること |
| `memory_map_test.cpp` | 外部メモリの割り当て。全チップ × 全種別の受け付け（`mapMemory()` と `setMemory()`。チップが持つ種別だけ）、外部メモリの列挙と名前からの検索が仕様の表の名前と一致すること、範囲の検査と取り外し、`getMemorySize()` が合計を返すこと、`setMemory()` の置き換え。`MemoryYmfmInterface` がアクセス種別と ROM/RAM 選択ビットからどのブロックを読み書きするか。OPNA・Y8950・OPNB の ADPCM-B が選択ビットに応じた側だけを読むこと（反対側に割り当てても出力が1サンプルも変わらないこと）。OPNA のリズムが `RHYTHM` を、OPNB の ADPCM-A が `ADPCM_A` を読むこと。RAM のブロックを複製しないこと。レジスタ経由の転送が `generate()` の中で RAM のブロックに入り、ROM には入らないこと。KEY の衝突で保留している間は転送も持ち越されること |
| `c_api_test.cpp` | C API を、ビルドした DLL を実行時にロードして呼ぶ。ヘッダが宣言する関数がすべてエクスポートされ、番号で指定する形の関数（`FmEngine_GetPartMask` / `FmEngine_GetMemorySize`）が残っていないこと。全チップで、列挙した部位と外部メモリの名前、部位の既定値が仕様の表と一致すること。名前での設定と読み戻し、チップが持たない名前・NULL・未知の chip_id・NULL のハンドルの拒否。列挙した外部メモリの名前がどれも `FmEngine_SetMemory` と `FmEngine_SetMemoryEx` に渡せること。名前で指定したゲインとメモリが、その名前の出力とメモリに効くこと（OPNA の `SSG` / `FM`、`RHYTHM`、`ADPCM_B_ROMMODE`、Y8950 の `FM_ACCESS_RAM` / `FM_ACCESS_ROM`） |

どれも全件通れば終了コード 0 を返す。

エンジンは既定のクロックを持たないので、テストは `_test/test_clocks.h` の
`testClock()` でクロックを渡す。ネイティブレートなどの期待値はこの値を前提に
している。チップを足したら `testClock()` にも足す。`c_api_test` はクロックに依る
期待値を持たないので、全チップに同じクロックを渡す。

Windows（vcvars64.bat を通した環境、リポジトリ直下で。`<name>` はテスト名）：
```cmd
cl /std:c++20 /EHsc /O2 /utf-8 /I src /I extern\ymfm\src _test\<name>.cpp extern\ymfm\src\ymfm_*.cpp
<name>.exe
```

Linux / macOS：
```bash
g++ -std=c++20 -O2 -I src -I extern/ymfm/src _test/<name>.cpp extern/ymfm/src/ymfm_*.cpp -o <name>
./<name>
```

`c_api_test` のビルドに ymfm のソースは要らない。先に DLL をビルドし、そのパスを渡す
（CMake の出力先は、Visual Studio なら `build\bin\Release\YMFMEngine.dll`、NMake なら
`build\bin\YMFMEngine.dll`。Linux の出力先は**未確認**：`CMakeLists.txt` は MSVC
以外で出力先を指定していないので、候補は `build/libYMFMEngine.so`。Linux で CMake を
走らせれば決まる）：
```cmd
cl /std:c++20 /EHsc /O2 /utf-8 /I src _test\c_api_test.cpp
c_api_test.exe <DLL のパス>
```
```bash
g++ -std=c++20 -O2 -I src _test/c_api_test.cpp -o c_api_test -ldl
./c_api_test <共有ライブラリのパス>
```

g++ は 10.2 で6本とも確認した。g++ 11.3 で確認したのは、部位と外部メモリを名前で
指定する形にする前の5本（`c_api_test` 以外）で、その後は走らせていない。macOS は
未検証。

`FmEngine::generate()` のキュー消化や `FmChip::keyOnTransitionMask()` を
変えたら `keyoff_retrigger_test` を走らせる。テストで使う音色は、リリースの
速いもの（RR 最大）にする。リリースが遅いと、約2ms の KEY OFF では谷が出ず
判定できない。

`LinearResampler`、`detail::*Split`、部位ゲインを変えたり ymfm を更新したり
したら `opn_split_test` を走らせる。一致の比較は、区間の長さを FM と SSG の
繰り返し数（18,9,6 と 4,2,1）の公倍数にして、区間の頭を揃えている。

`ChipPart`、`FmChipImpl::generateNative()` / `mixParts()`、`PartGains` を
変えたり ymfm を更新したりしたら `part_gain_test` を走らせる。比較は浮動小数の
完全一致で見ている。部位ごとのゲインを 0.5 / 0.25（2の冪）にして、掛け算でも
丸めが出ないようにしてある。部位を足したら、そのチップの経路と `testAccept()`
の表を足す。`testAccept()` の表の名前は、`chipPartName()` から作らずに仕様の表を
書き写してある（実装と同じ関数から期待値を作ると、名前を取り違えても通る）。

`MemoryYmfmInterface`、`ChipMemoryType`、`chipMemoryName()`、`FmChip::hasMemory()`、
`FmEngine` の `mapMemory()` / `setMemory()` / `getMemorySize()` を変えたり ymfm を
更新したりしたら `memory_map_test` を走らせる。再生の比較は、何も割り当てない場合
（0 を読む）との差で見ている。ROM モードは 8KB を回るうちに両方のアキュムレータが
上限に張り付いて一致するので、変わるサンプル数の閾値を全体の1割にしてある。
種別を足したら `testAccept()` の表を足す。

`createChip()` / `createChipByName()`、`chipTable()`、`FmChipImpl` のコンストラクタ、
`FmEngine::addChip()` / `addChipByName()` を変えたら `chip_clock_test` を走らせる。
チップの一覧は `chipTable()` から取るので、チップを足すと自動で対象に入る
（`testClock()` に足していなければ落ちる）。

`FmEngineApi.cpp`、`FmEngineApi.def`、`chipPartName()` / `chipMemoryName()`、
`FmChip::hasPart()` / `hasMemory()`、`defaultPartGain()` を変えたり、
`src/FmEngineApi.h` を差し替えたりしたら、DLL をビルドし直して `c_api_test` を
走らせる。期待値は `chipSpecs()` の表で、FmEngineApi の仕様書の表を書き写したもの。
チップ・部位・外部メモリを足したら、この表にも足す（足していなければ chips か
names の行が落ちる）。ヘッダに関数が増えたら `FM_API_FUNCTIONS` に足す。
`FmEngine` の中で二重に検査している項目（チップが持たない名前の拒否）は、片方を
外しても `c_api_test` は通る。その検査は `part_gain_test` と `memory_map_test` の
find の行が見ている。
