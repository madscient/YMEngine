# CLAUDE.md

AI 向けの作業メモ。人間向けの文書は `README.md`（DLL 利用者）と
`README_ymfm.md`（C++ から直接使う開発者）。

## 文書の置き場所

- `doc/CHANGELOG.md` — 開発経緯。方針の前提、見送った案、確認結果を書く
- 回帰テストと、その実行方法はこのファイルに書く。README からは参照しない

## 回帰テスト (`_test/`)

CMake には組み込んでいない。ヘッダと ymfm のソースから直接ビルドする。

| ファイル | 見ていること |
|---|---|
| `keyoff_retrigger_test.cpp` | 同じチャンネルへの KEY OFF → KEY ON が短い間隔で続いても、KEY OFF が観測されること（OPL3・OPNA・OPM・OPLL・OPL4 AWM。batch / crowd / tiny の3条件） |
| `opn_split_test.cpp` | `LinearResampler` が呼び出しをまたいでソースを読み捨てないこと。OPN 系の `detail::*Split` の FM/SSG が上流 `generate()`（FIDELITY_MAX）と全サンプル一致すること（prescale 切り替えを含む。書き込みを抜いた対照で不一致が出ること）。`FmEngine` のネイティブレート・既定の SSG 音量・部位ゲイン |
| `part_gain_test.cpp` | OPLL 系・OPL3・OPL4 の部位ゲインが ymfm のどの出力に掛かるか（ネイティブレートで上流と全サンプル比較。別の出力と比べた対照で不一致が出ること）。既定値で今までの出力（OPLL はメロディ+リズム、OPL3 は A/B、OPL4 は DO2）になること。部位を持たないチップにチップのゲインが掛かること。全チップ × 全部位の受け付けと既定値、`getPartMask()` |
| `chip_clock_test.cpp` | 全チップで clock=0 を拒否し、チップが増えないこと（`addChip()` / `addChipByName()` / `createChip()` / `createChipByName()`）。渡したクロックがそのままチップに渡り、ネイティブレートがクロックに比例すること |
| `memory_map_test.cpp` | 外部メモリの割り当て。全チップ × 全種別の受け付け、範囲の検査と取り外し、`getMemorySize()` が合計を返すこと、`setMemory()` の置き換え。`MemoryYmfmInterface` がアクセス種別と ROM/RAM 選択ビットからどのブロックを読み書きするか。OPNA・Y8950・OPNB の ADPCM-B が選択ビットに応じた側だけを読むこと（反対側に割り当てても出力が1サンプルも変わらないこと）。RAM のブロックを複製しないこと。レジスタ経由の転送が `generate()` の中で RAM のブロックに入り、ROM には入らないこと。KEY の衝突で保留している間は転送も持ち越されること |

どれも全件通れば終了コード 0 を返す。

エンジンは既定のクロックを持たないので、テストは `_test/test_clocks.h` の
`testClock()` でクロックを渡す。ネイティブレートなどの期待値はこの値を前提に
している。チップを足したら `testClock()` にも足す。

Windows（vcvars64.bat を通した環境、リポジトリ直下で。`<name>` はテスト名）：
```cmd
cl /std:c++20 /EHsc /O2 /utf-8 /I src /I extern\ymfm\src _test\<name>.cpp extern\ymfm\src\ymfm_*.cpp
<name>.exe
```

Linux / macOS（`keyoff_retrigger_test` は g++ 10.2 と 11.3、`opn_split_test`・`part_gain_test`・`memory_map_test`・`chip_clock_test` は g++ 11.3 で確認。macOS は未検証）：
```bash
g++ -std=c++20 -O2 -I src -I extern/ymfm/src _test/<name>.cpp extern/ymfm/src/ymfm_*.cpp -o <name>
./<name>
```

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
の表を足す。

`MemoryYmfmInterface`、`ChipMemoryType`、`FmChip::hasMemory()`、`FmEngine` の
`mapMemory()` / `setMemory()` / `getMemorySize()` を変えたり ymfm を更新したり
したら `memory_map_test` を走らせる。再生の比較は、何も割り当てない場合
（0 を読む）との差で見ている。ROM モードは 8KB を回るうちに両方のアキュムレータが
上限に張り付いて一致するので、変わるサンプル数の閾値を全体の1割にしてある。
種別を足したら `testAccept()` の表を足す。

`createChip()` / `createChipByName()`、`chipTable()`、`FmChipImpl` のコンストラクタ、
`FmEngine::addChip()` / `addChipByName()` を変えたら `chip_clock_test` を走らせる。
チップの一覧は `chipTable()` から取るので、チップを足すと自動で対象に入る
（`testClock()` に足していなければ落ちる）。
