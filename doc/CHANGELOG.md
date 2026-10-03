# CHANGELOG

開発経緯の記録。現在の仕様は `README.md` と `README_ymfm.md` を参照。

## 部位と外部メモリを名前で指定する

FmEngineApi の仕様の改訂（FMEngineTest `20c4923`）に追従した。部位と外部メモリは
名前の文字列で指定し、チップが持つものはエンジンに問い合わせて列挙する。
`FmPart`、`FmMemoryType`、`FmEngine_GetPartMask`、`FmEngine_GetMemorySize` は
無くなった。ヘッダの正本は FMEngineTest の `include/FmEngineApi.h` に移り、
`src/FmEngineApi.h` はその写しになった。

前提（仕様と同じ）：番号で指定する形でビルドした呼び出し側を、この DLL と
組み合わせて使わないこと。`FmEngine_SetPartGain` / `FmEngine_GetPartGain` /
`FmEngine_SetMemory` / `FmEngine_SetMemoryEx` は、名前が同じまま第3引数が番号から
文字列に変わった。組み合わせると、DLL は番号をポインタとして読む。

### 変更

- `src/FmEngineApi.h` を正本の写しに差し替えた。**確認済み**：
  `git hash-object src/FmEngineApi.h` が、FMEngineTest の
  `git rev-parse 20c4923:include/FmEngineApi.h` と同じ `206f723` を返す
- C API に `FmEngine_GetPartCount` / `FmEngine_GetPartName` /
  `FmEngine_GetMemoryCount` / `FmEngine_GetMemoryName` を足した。
  `FmEngine_SetPartGain` / `FmEngine_GetPartGain` / `FmEngine_SetMemory` /
  `FmEngine_SetMemoryEx` は名前を受け取る。`FmEngine_GetPartMask` と
  `FmEngine_GetMemorySize` は定義ごと消した（`FMENGINE_API` が付いた定義は、
  `.def` から外してもエクスポートされるため）
- OPNA のリズム音の内蔵 ROM は `RHYTHM`。C++ 側に `ChipMemoryType::RHYTHM` を
  足し、OPNA は `ADPCM_A` を持たなくなった。ymfm は OPNA のリズムを OPNB の
  ADPCM-A と同じ `ACCESS_ADPCM_A` で読むので、`MemoryYmfmInterface` に
  振り向け先（`setAdpcmAMemory()`）を持たせ、OPNA だけ `RHYTHM` に向けた
- C++ 側に、列挙型から名前を返す `chipPartName()` / `chipMemoryName()` と、
  チップが持つものの列挙・検索（`FmChip` と `FmEngine` の `partCount()` /
  `partName()` / `findPart()` / `memoryCount()` / `memoryName()` /
  `findMemory()`）を足した。C API は、入口で名前を列挙型に直してから今までの
  関数を呼ぶ
- `FmEngine::getPartMask()` を消した
- `ChipMemoryType` を 0 始まりの連番に振り直した（`RHYTHM`=0、`ADPCM_A`=1、
  `ADPCM_B`=2、`ADPCM_B_ROMMODE`=3、`PCM`=4。`kChipMemoryTypeEnd` は
  `kChipMemoryTypeCount` になった）。番号を C の定数に揃える理由が無くなった
- README の、仕様書とテストツールへのリンクを直した。今までは実在しない
  リポジトリを指していた

### 呼び出し側から見える挙動の変化

仕様に合わせた結果として変わるもの。

- OPNA のリズムの ROM を `ADPCM_A` で渡すと `FM_ERR_INVALID_ARG`。`RHYTHM` で渡す
- `FmEngine_SetMemory` は、チップが持たないメモリの名前を `FM_ERR_INVALID_ARG` で
  拒否する。今までは、チップが持たない種別も受け付けていた（チップは読まない）
- `FmEngine_SetMemory` に `ADPCM_B_ROMMODE` を渡せる。今までは
  `FmEngine_SetMemoryEx` 専用だった。仕様は「列挙した名前は、どれも
  `FmEngine_SetMemory` に渡せる」とする
- 割り当てたブロックの大きさの合計を、C API からは取れなくなった

ROM を渡さない使い方では、出力は変わらない（下の確認）。

### 仕様に書かれておらず、こちらで決めたこと

どれも変えるときは、関数の条件1か所と、テストの該当行、README の該当行で済む
（C++ の名前を変える場合は、ヘッダ2本とテスト、README_ymfm に及ぶ）。

- 列挙の順序は `ChipPart` / `ChipMemoryType` の番号順（OPNA の外部メモリは
  `RHYTHM`、`ADPCM_B`、`ADPCM_B_ROMMODE`）。仕様は順序を定めず、同じ chip_id に
  同じ順序で返すことだけを求める
- `FmEngine_SetMemory` の `data` が NULL のときと `size` が 0 のときは、今までどおり
  `FM_ERR_INVALID_ARG`。仕様は今回も定めていない
- `FmEngine_SetMemoryEx` の、`base + size` が 2^32 を越える場合と未知の `access` は、
  今までどおり `FM_ERR_INVALID_ARG`。`FM_ERR_UNAVAILABLE` は返さない。正本の
  ヘッダの戻り値の一覧には載らないので、README に書いた
- NULL のハンドルには、数は 0、名前は NULL、設定と取得は `FM_ERR_INVALID_ARG`
- C++ の API は列挙型のまま残した。`generate()` が部位のゲインを配列の添字で
  引くので、列挙型は内部に要る
- `FmEngine::getMemorySize()` は C++ に残した。`memory_map_test` が、取り外しで
  ブロックが外れたことを見るのに使う
- `FmEngine::setMemory()` も、チップが持たない種別を拒否する（C API と揃えた）
- DLL のバージョン（2.0.0）は変えていない

前提：部位と外部メモリの名前が、チップの中で一意であること。同じ名前を2つ持つ
チップを足すと、`findPart()` / `findMemory()` は番号の若い方だけを返す。
`part_gain_test` / `memory_map_test` / `c_api_test` の names の行は、列挙した
名前が重なると落ちる。

### 見送った案

- C++ の API も名前で受け取る（`setPartGain(id, "SSG", ...)`）。理由：C++ の
  呼び出し側は列挙型で書け、綴りの誤りをコンパイル時に見つけられる。名前から
  引きたい場合は `findPart()` / `findMemory()` がある
- OPNA のリズムを、C++ では `ChipMemoryType::ADPCM_A` のままにして、名前だけ
  `RHYTHM` にする。理由：同じメモリの呼び名が C と C++ で食い違い、OPNA の
  `hasMemory(ADPCM_A)` が true のままになる
- `c_api_test` を、DLL を作らずに `FmEngineApi.cpp` と一緒にビルドして走らせる。
  理由：正本のヘッダは、Windows では `dllexport` か `dllimport` のどちらかにしか
  ならない。実行ファイルに入れると、実行ファイルがインポートライブラリを吐く。
  `.def` とエクスポートも見られない
- `.def` とヘッダの食い違いを CMake で検査する。理由：`.def` にあって定義が
  無ければリンクが落ち、定義があれば `.def` に無くてもエクスポートされる。残る
  穴（消したはずの関数の定義が残り、エクスポートされ続ける）は `c_api_test` の
  symbols が見る

### 仕様側の記録と食い違うところ

FMEngineTest `20c4923` の `docs/CHANGELOG.md` は、変更前の状態として「チップが
そのメモリを持たないときの `FmEngine_SetMemory` の戻り値は、YMEngine と
FMgenEngine が `FM_ERR_INVALID_ARG`」とする。YMEngine `7d8ed2d` の
`FmEngine_SetMemory` は、この場合に `FM_OK` を返していた（コードで確認：
`FmEngine::setMemory()` はチップが持つかを見ず、`memory_map_test` に
「setMemory accepts a type the chip does not have」の行があった。
`FM_ERR_INVALID_ARG` を返していたのは `FmEngine_SetMemoryEx`）。今回の変更で
拒否するようになった。FMEngineTest の文書には手を入れていない。

### 確認

`_test/c_api_test.cpp` を追加した。ビルドした DLL を実行時にロードし、C API だけを
呼ぶ。`part_gain_test` と `memory_map_test` には、列挙と検索、`RHYTHM` の経路、
`setMemory()` の受け付けを足した。期待する名前は、3本とも仕様書の表を書き写して
ある（`chipPartName()` / `chipMemoryName()` からは作っていない）。

**確認済み**（MSVC 19.51 と g++ 10.2 (Debian 11) でビルドして実行、6本とも
全件通過）：

- `c_api_test`（MSVC は CMake でビルドした DLL、g++ は `g++ -shared` で直接
  ビルドした共有ライブラリに対して）：
  - ヘッダが宣言する 20 関数がすべてエクスポートされ、`FmEngine_GetPartMask` と
    `FmEngine_GetMemorySize` は無い
  - 全16チップで、列挙した部位の名前と既定値、外部メモリの名前が、仕様書の表と
    一致する。名前で設定したゲインを読み戻せる。チップが持たない名前（ほかの
    チップの部位と外部メモリの名前、大文字小文字の違う名前、空文字列など）、
    NULL、未知の chip_id、NULL のハンドルを拒否し、拒否した呼び出しはほかの
    部位の値を変えない
  - 列挙した外部メモリの名前は、どれも `FmEngine_SetMemory` と
    `FmEngine_SetMemoryEx` に渡せる。重なり、隣接、取り外し（`access` を見ない）、
    size 0、2^32 越え、未知の `access`、`data` が NULL
  - OPNA の SSG のトーンは、`FM` を 0 にしても変わらず、`SSG` を L=0.5 / R=0 に
    すると L が約半分・R が 0 になる
  - OPNA のリズムは、`RHYTHM` に 0x77 の 8KB を渡すと、何も渡さない場合と
    4,630 サンプル（4,800 中）食い違い、`ADPCM_B` に渡しても 0。ROM モードの
    ADPCM-B は、`ADPCM_B_ROMMODE` に `FmEngine_SetMemory` で渡すと 1,888 サンプル
    食い違い、`ADPCM_B` では 0
  - Y8950 のレジスタ経由の転送は、`FM_ACCESS_RAM` のブロックに入り、
    `FM_ACCESS_ROM` のブロックには入らない
- `memory_map_test`：`MemoryYmfmInterface` 単体で、`ACCESS_ADPCM_A` は振り向け
  なければ `ADPCM_A` を、`RHYTHM` に振り向ければ `RHYTHM` を読む。OPNA のリズムは
  `RHYTHM` に割り当てると 4,628 サンプル、OPNB の ADPCM-A は `ADPCM_A` に割り当てると
  4,798 サンプル食い違う（どちらも `ADPCM_B` に割り当てても 0）
- `keyoff_retrigger_test` / `opn_split_test` / `chip_clock_test` は変えていない

試験が効いていることの確認（**確認済み**、MSVC）：`src/` の写しを1か所ずつ壊して
ビルドし、落ちることを見た。

| 壊し方 | `part_gain_test` | `memory_map_test` | `c_api_test` |
|---|---|---|---|
| `FM` と `SSG` の名前を入れ替える | 4 | 0 | 1 |
| OPNA で `ACCESS_ADPCM_A` を振り向けない | 0 | 1 | 1 |
| OPNA が `ADPCM_A` を持ったままにする | 0 | 3 | 2 |
| `setMemory()` がチップの持たない種別を受け付ける | 0 | 16 | 0 |
| `ADPCM_B_ROMMODE` の綴りを変える | 0 | 4 | 6 |
| C API の `SetPartGain` で L と R を入れ替える | 0 | 0 | 11 |
| `FmEngine_GetMemorySize` の定義を残す | 0 | 0 | 1 |
| `findPart()` が `hasPart()` を見ない | 16 | 0 | 0 |
| `findMemory()` が `hasMemory()` を見ない | 0 | 16 | 0 |
| C API の `SetMemoryEx` が `access` を無視する | 0 | 0 | 6 |
| C API の `SetMemory` が名前を無視して `ADPCM_B` に割り当てる | 0 | 0 | 7 |
| 取り外しでも `access` を検査する | 0 | 0 | 5 |

数字は落ちた行の数。`c_api_test` が 0 の3行は、C API の入口と `FmEngine` の中で
同じことを二重に検査している項目で、片方を外しても C API の結果は変わらない。
壊していない写しでは3本とも全件通る。変更前のソースからビルドした DLL
（リポジトリの `build/` に残っていたもの。どのコミットかは確かめていない）に
`c_api_test` を掛けると、列挙の4関数が無く、消した2関数が残っているとして落ちる。

DLL：CMake（NMake Makefiles、MSVC 19.51、Release）でビルドが通り、dumpbin で
エクスポートが仕様の 20 個であることを見た（**確認済み**）。警告 C4005 と C4324 は、
今回変えていない行から出ている（`FmEngineApi.cpp` の `#define FMENGINE_EXPORTS` と
`FmEngine.h` の `SpscQueue`）。

仕様側のテストツール（**確認済み**）：FMEngineTest の `build/` にあったビルド済みの
`FMEngineTest.exe` で `patches/all.json` を WAV に書き出した。exe がどのソースから
ビルドされたかは確かめていない（更新時刻は `20c4923` のコミットより後。走らせた
時点の FMEngineTest の HEAD と作業ツリーは見ていない）。外部メモリを名前で列挙する
版であることは、次の表示から分かる。この DLL では、外部メモリを列挙して、OPNA の `RHYTHM` と
OPNB/OPNBB の `ADPCM_A`・`ADPCM_B` に ROM ファイルを探し（置いていないので
not found）、OPL4 の `PCM` と OPNA/Y8950 の `ADPCM_B`・`ADPCM_B_ROMMODE` を
`[MEM]` の行に出した。変更前の DLL（上と同じもの）では
「`FmEngine_GetMemoryCount` is not exported」の断りを出した。2つの WAV
（16 チップ、183 秒、無音ではない）はバイト一致した。

**未検証**：

- 実際の ROM イメージを渡したときの音。試験は 0x77 で埋めたデータとの差で見ている
- Linux での CMake のビルド（試した Linux 環境の cmake が 3.18 で、`CMakeLists.txt` の
  求める 3.20 に足りない）。上の g++ の共有ライブラリは、`-fvisibility=hidden` を
  `FmEngineApi.cpp` にだけ付けて直接ビルドしたもので、CMake のビルドとは
  エクスポートが違う（ymfm のシンボルも出る）
- g++ 11.3 と macOS
- C から（`/TC` や gcc で）このヘッダを include した呼び出し。ヘッダそのものの
  確認は FMEngineTest 側にある

### 気づいたが手を付けていないこと

- DLL のバージョンは 2.0.0 のまま。同じ関数名で引数の意味が変わったので、
  バージョンで新旧を見分けたい場合は `FmEngineApi.rc` と `CMakeLists.txt` の
  2か所を変える。仕様は `FmEngine_GetPartCount` / `FmEngine_GetMemoryCount` の
  有無で見分けるとしている
- 警告 C4005（`FMENGINE_EXPORTS` を `FmEngineApi.cpp` と CMake の両方で定義して
  いる）

## FmEngine_AddChip の clock=0（既定のクロック）をやめる

FmEngineApi の仕様の改訂（FMEngineTest `866f4a3`）に追従した。`FmEngine_AddChip`
は clock=0 に `FM_ERR_INVALID_ARG` を返し、エンジンは既定のクロックを持たない。
仕様側の理由：既定のクロックは典型的な値にすぎず、エンジンによって食い違って
いた。同じチップでも機種によってクロックが違い、レジスタ値はクロックを前提に
計算する。

前提：レジスタ値を書く呼び出し側が、そのクロックを知っていること（仕様と同じ）。

### 変更

- `FmClock` の定数と `ChipEntry::defaultClock` を消した。`createChip()` /
  `createChipByName()` は clock が 0 なら nullptr、`FmEngine::addChip()` /
  `addChipByName()` は `UINT32_MAX` を返す。clock の既定引数も消したので、clock を
  渡していない C++ の呼び出しはコンパイルが通らなくなる
- README の「標準クロック」の列を「クロックの例」にし、エンジンの既定値ではない
  ことを書いた
- テストは `_test/test_clocks.h` の `testClock()` でクロックを渡す。値は消した
  既定値と同じなので、テストの期待値（OPNA のネイティブレート 55,466Hz など）は
  変わらない

### 仕様に書かれておらず、こちらで決めたこと

どれも変えるときは数行で済む。

- 名前が未知で clock も 0 なら `FM_ERR_INVALID_ARG`。引数の検査を名前の照合より
  先にする（null の name と同じ扱い）
- `FmEngine_AddChip` の中で確保に失敗したら `FM_ERR_ALLOC` を返す。今までは
  例外が DLL の外に出ていた。仕様の戻り値には前から `FM_ERR_ALLOC` がある
- C++ の API でも clock=0 を拒否する

### 外部メモリの文言の明確化（FMEngineTest `c0589c1`）

仕様に「エンジンは `FmEngine_SetMemory` の `data` に書き込まない」が入った。
YMEngine の `setMemory()` は、書き込み先を持たないブロックとして割り当てるので、
コードの変更は要らなかった。`memory_map_test` に、レジスタ経由の転送が
`setMemory()` のデータに入らないことを足した。

### 確認

`_test/chip_clock_test.cpp` を追加した。**確認済み**（MSVC 19.51 と g++ 11.3
でビルドして実行、全件通過）：

- 全16チップで、`addChip()` / `addChipByName()` / `createChip()` /
  `createChipByName()` が clock=0 を拒否し、チップが増えない。未知の名前も拒否する
- `testClock()` の値とその2倍で、`FmChip::clock()` が渡した値になり、ネイティブ
  レートが2倍になる（例：OPL2 は 49,715 → 99,431）

試験が効いていることの確認（**確認済み**）：`FmChip.h` の写しで、clock=0 を
3,579,545 に置き換える版では reject が16件、パターン A のコンストラクタで clock を
無視する版では pass がパターン A の10チップぶん落ちた。`setMemory()` のデータを
書き込み可能にする版では、`memory_map_test` の追加した1件だけが落ちた。

`memory_map_test` / `keyoff_retrigger_test` / `opn_split_test` /
`part_gain_test` も、MSVC と g++ 11.3 で全件通った。

DLL：CMake（NMake Makefiles、MSVC 19.51、Release）でビルドが通った。C API は、
リポジトリに残さない確認用のプログラムで DLL を `LoadLibrary` して確かめた
（**確認済み**）：clock=0 は `FM_ERR_INVALID_ARG` で `out_id` を書き換えない。
未知の名前と clock=0 の組も `FM_ERR_INVALID_ARG`。未知の名前は
`FM_ERR_UNKNOWN_CHIP`。OPL2 を 3,579,545 / 7,159,090 で足すとネイティブレートが
49,715 / 99,431 になる。`FM_ERR_ALLOC` を返す経路は走らせていない（**未検証**）。

## 外部メモリの ROM/RAM を区別する（FmEngine_SetMemoryEx）

FmEngineApi の仕様の改訂（FMEngineTest `e002890` の `docs/FmEngineApi.md`
「外部メモリの割り当て (任意)」）に追従した。仕様が参照実装より先に書かれたので、
YMEngine がこの改訂の最初の実装になる。

### 変更前の YMEngine（コードで確認）

- ymfm は ADPCM-B を1つの空間（`ACCESS_ADPCM_B`）で読み書きする。ROM/RAM
  選択ビット（`rom_ram()`）が変えるのは `address_shift()` のアドレスの刻みだけ
  （ROM と x8 は 32 バイト、x1 は 4 バイト）。YMEngine もこの空間に `SetMemory`
  のデータを1つ置いていたので、OPNA/Y8950 の ROM モードと RAM モードは同じ
  メモリを読んでいた。実機では物理的に別のメモリ（利用者から）
- チップからの書き込みは常に捨てていた（`writeable=false`）。内部 RAM を確保する
  `MemoryYmfmInterface::allocMemory()` は、どこからも呼ばれていなかった
- `ACCESS_IO`（SSG の I/O ポート、OPM の CT など）が ADPCM-B の領域に落ちていた。
  読み出しは YMEngine の API から呼ばれず、書き込みは上のとおり捨てるので、
  出力には影響していなかった
- `SetMemory` に未知の chip_id を渡すと、assert だけで、Release では範囲外の
  要素に触っていた。種別に 0 や 4 以上を渡すと ADPCM-B に割り当てていた

### 決めたこと（利用者と決めた）

- `FmEngine_GetMemorySize` は、割り当てたブロックの大きさの合計を返す。
  `SetMemory` だけを使う呼び出し側では今までと同じ値になる
- OPNA/Y8950 で ROM/RAM 選択ビットが ROM の間も、レジスタ経由の転送は ymfm の
  とおりメモリに書く（`ADPCM_B_ROMMODE` 側へ）。仕様側の前提では、実機は ROM
  モードでレジスタ経由の書き込みができない。ymfm の動作を変えない方を選んだ

### 仕様に書かれていないこと（こちらの案を利用者が了承した）

どれも変えるときは、関数の条件1か所と `memory_map_test` の該当行、README の
該当行で済む（C++ の名前だけは、ヘッダと既存のテスト3本と README_ymfm に及ぶ）。

- `base + size` が 2^32 を越えたら `FM_ERR_INVALID_ARG`。ちょうど 2^32 で
  終わる範囲は受け付ける
- チップのアドレスの幅を越える範囲も受け付ける（チップはそこを読まない）
- `FmEngine_SetMemoryEx` は ROM のブロックも複製しない。`FM_ERR_UNAVAILABLE`
  は返さない
- `FmEngine_SetMemory` は、その種別のブロックをすべて外してから `[0, size)` を
  割り当てる（今までの「置き換え」と同じ）。`FM_MEM_ADPCM_B_ROMMODE` と、範囲外の
  番号と、未知の chip_id は `FM_ERR_INVALID_ARG`。チップが持たない種別は、
  今までどおり受け付ける（チップは読まない）
- C++ 側に `ChipMemoryType` / `ChipMemoryAccess` を置き、`ChipPart` と同じく
  C API と番号を揃えた。`FmEngine::setMemory()` / `getMemorySize()` の引数は
  `ymfm::access_class` から `ChipMemoryType` に変わる（`ymfm::access_class` では
  ROM モードのメモリを表せないため）。`FmEngine::mapMemory()` は C API と同じく
  nullptr で外す
- `allocMemory()` は消した

前提：ymfm の ADPCM-B が選択ビットを `adpcm_b_registers::rom_ram()` に持ち、
`ym2608` / `y8950` の `m_adpcm_b` が protected であること。名前が変われば
`detail::Ym2608Split` / `detail::Y8950Mem` のコンパイルが通らなくなる。

前提：RAM の割り当てはストリーム開始前に決まる（仕様と同じ）。割り当ての変更は
スレッドセーフではない。

### 仕様と食い違うところ

仕様は `FM_ACCESS_RAM` について「`FmEngine_Write` によってチップがメモリに書いた
値は、その `FmEngine_Write` が戻った後に始まった `FmEngine_Generate` が戻った
時点でブロックに入っている」とする。YMEngine は KEY ON/OFF の衝突で書き込みを
保留し、保留を呼び出しをまたいで持ち越す（「キー衝突時の先行生成を呼び出しを
またいで保留する」の節）。保留の後ろに並んだ転送は、その間の `FmEngine_Generate`
では反映されない。**確認済み**（`memory_map_test` の store：Y8950 で KEY ON → OFF
の後に転送を並べ、1 サンプルと 94 サンプルの呼び出しの後にはブロックに入って
おらず、さらに 1 サンプルで入った）。

ヘッダと README には「書き込みを反映した `FmEngine_Generate` が戻った時点で
入っている」と、YMEngine の実際の動作を書いた。利用者の判断で、保留の設計は
変えず、この動作のままとする。FMEngineTest の仕様書には手を入れていない。

前提：KEY OFF/ON の状態を最低約2ms 観測させることを、書き込みの反映の早さより
優先する（「キー衝突時の先行生成を呼び出しをまたいで保留する」の節と同じ）。
この前提が変われば、保留の持ち越しと一緒にこの食い違いも見直す。

### 実装

- `MemoryYmfmInterface` は種別ごとにブロックの一覧（番地、大きさ、読み出し元、
  RAM なら書き込み先）を持ち、アクセスのたびに番地を含むブロックを探す
- OPNA と Y8950 は、構築時に ADPCM-B のレジスタをインターフェースに結び付け、
  `ACCESS_ADPCM_B` のたびに `rom_ram()` で `ADPCM_B_ROMMODE` / `ADPCM_B` を選ぶ。
  Y8950 は上流の `m_adpcm_b` が protected なので、`detail::Y8950Mem` を挟んだ。
  OPNB/OPNBB は刻みが固定で、選択ビットを見ない
- `FmChip::hasMemory()` でチップごとの種別を判定する

挙動の変化：

- OPNA/Y8950 で `SetMemory(FM_MEM_ADPCM_B)` に渡したデータは、RAM モードでだけ
  読まれる。ROM モードで鳴らしていた呼び出し側は無音になる（仕様側で受け入れ
  済み）。**確認済み**（`memory_map_test` の play：OPNA の ROM モードで、
  `setMemory(ADPCM_B)` の出力は何も割り当てない場合と一致した）
- `SetMemory` の未知の chip_id と、種別の 0・4・5 以上が `FM_ERR_INVALID_ARG` に
  なる

再生中にモードを切り替えた場合：ymfm はスタート番地を再生開始時の刻みで計算する
（`load_start()`）。読むメモリはアクセスの時点のビットで切り替わるが、刻みは
古いままになる（ymfm のコードで確認。走らせてはいない）。

### 確認

`_test/memory_map_test.cpp` を追加した。**確認済み**（MSVC 19.51 と g++ 11.3
でビルドして実行、全件通過）：

- accept：全16チップ × 種別 0〜5 の受け付けが表と一致（割り当てと取り外しの
  両方）。範囲の検査、取り外し、`getMemorySize()` の合計、`setMemory()` の置き換え
- route：`MemoryYmfmInterface` 単体で、RAM（x1/x8）モードは `ADPCM_B`、ROM
  モードは `ADPCM_B_ROMMODE` を読む。ブロックの前後の番地は 0。ROM のブロック
  への書き込みは捨て、RAM のブロックにはその場で入る。`ACCESS_IO` はどの
  ブロックにも触らない
- play：OPNA・Y8950 の各モードで、読まれるべき側に割り当てると、何も割り当てない
  場合と 1,891〜4,799 サンプル（4,800 中）食い違う。反対側に割り当てても 0。
  OPNB は選択ビットを立てても `ADPCM_B` を読む。RAM のブロックを生成の合間に
  書き換えると、次の生成から出力が変わる
- store：レジスタ経由の転送（Y8950・OPNA の録音モード、OPL4 のメモリアクセス
  モード）が、`write()` の直後にはブロックに入っておらず、`generate()` の後に
  RAM のブロックに入る。ROM のブロックには入らない。OPL4 で ROM と RAM を1つの
  空間に並べ、RAM 側の番地への転送だけが入る

試験が効いていることの確認（**確認済み**）：`FmChip.h` の写しで、選択ビットを
結び付けない、ビットの判定を反転する、RAM を ROM として扱う、の3通りを作り、
それぞれ 5 件、18 件、8 件落ちることを見た。

既存の `keyoff_retrigger_test` / `opn_split_test` / `part_gain_test` も、MSVC と
g++ 11.3 で全件通った（テストの `ymfm::ACCESS_PCM` を `ChipMemoryType::PCM` に
書き換えた）。

DLL：CMake（NMake Makefiles、MSVC 19.51、Release）でビルドが通り、
`FmEngine_SetMemoryEx` がエクスポートされることを dumpbin で確認した。警告
C4005（`FMENGINE_EXPORTS` の再定義）と C4324（`SpscQueue` のパディング）は、
今回変えていない行から出ている（変更前のビルドとは比べていない）。

C API：**確認済み**（リポジトリに残さない確認用のプログラムで、ビルドした DLL を
`LoadLibrary` / `GetProcAddress` で呼んだ）。null のハンドル、未知の chip_id、
チップが持たない種別、未知の access、size 0、重なりは `FM_ERR_INVALID_ARG`。
NULL での取り外し、`GetMemorySize` の合計、`SetMemory` の拒否と互換の受け付け、
Y8950 のレジスタ経由の転送が `FmEngine_Generate` の後に呼び出し元のブロックに
入ることを見た。C API を通す試験はリポジトリには無い。

### 気づいたが手を付けていないこと

利用者の判断で、どちらも今は直さない。

- `FmEngine_Write` のキュー（`SpscQueue<RegWriteCmd, 4096>`）が一杯になると、
  書き込みは捨てられ、`FM_OK` が返る（コードで確認）。レジスタ経由で大きな
  データを転送するときは、間に `FmEngine_Generate` を挟まないと取りこぼす
- `FmEngine_Write` / `SetGain` / `GetGain` の未知の chip_id は assert だけで、
  Release では範囲外の要素に触る（コードで確認）

## 部位をチップの出力端子ごとに分ける

OPN 系に部位ゲインを入れたあと、ほかに別々の出力を持つチップを調べた。

### 調べた結果

- **OPLL 系**：ymfm が `data[0]`=メロディ、`data[1]`=リズムを分けて出す
  （ymfm のコードで確認）。実機の YM2413 はメロディ（MO）とリズム（RO）を
  別の端子から出す。**推測**：ピン構成の記憶による。YMF281 と YM2423 も同じ
  2端子と見た。**推測**：根拠は ymfm が3つとも同じ `opll_base` で扱っている
  ことだけ
- **VRC7**：実機はリズムを持たず、出力も1本。**推測**：NESdev の記述の記憶に
  よる。一方、ymfm の `ds1001` は音色表を替えただけの `opll_base` で、0x0E を
  書けばリズムが鳴る（ymfm のコードで確認）
- **OPL3**：ymfm は A/B/C/D の4出力を作る。YMEngine は A/B だけを使い、C/D を
  捨てていた（コードで確認）
- **OPL4**：DO0（FM の C/D）、DO1（AWM の C/D）、DO2（FM の A/B と AWM の A/B）。
  YMEngine は DO2 だけを使っていた。DO2 の中の FM と AWM はチップ内で混ぜ、
  比率はレジスタ 0xF8/0xF9 で決まり、混ぜたあとで16ビットに切り詰める
  （ymfm のコードで確認）
- **OPL / OPL2 / Y8950**：ymfm の出力は1本。Y8950 の ADPCM は FM と足して
  から DAC の丸めを掛けるので、ymfm 上では分けられない。実機の Y8950 で ADPCM
  が別の端子から出ているかは**未確認**
- **OPN2 / OPM / OPZ**：ステレオ1系統

### 外から見える値（利用者と決めた）

- 部位は「チップ名_端子名」で名付ける。`FM_PART_OPN_FM`=0、`OPN_SSG`=1、
  `OPLL_MELODY`=2、`OPLL_RHYTHM`=3、`OPL3_AB`=4、`OPL3_CD`=5、`OPL4_DO0`=6、
  `OPL4_DO1`=7、`OPL4_DO2`=8
- `FM_PART_FM` / `FM_PART_SSG` を `FM_PART_OPN_FM` / `FM_PART_OPN_SSG` に改名
  した。番号は 0/1 のままなので、バイナリ互換は保たれる。改名の時点で、旧名は
  GitHub の master に push 済み・タグ無しだった
- 番号はチップをまたいで重ならない。別のチップの部位を渡したら
  `FM_ERR_INVALID_ARG` を返す
- 出力が1本のチップ（OPL、OPL2、Y8950、OPN2、OPM、OPZ）は部位を持たない。
  「出力全体」の意味もあった `FM_PART_FM` は無くなる。部位を持つチップ
  （OPLL 系、OPL3、OPL4）にも「出力全体」の部位は無い
- 既定値は `OPL3_CD`、`OPL4_DO0`、`OPL4_DO1` が 0、それ以外は 1.0。既定の
  出力を変えないため。FM の出力先を A/B/C/D 全部にしたチャンネルは A/B と
  C/D に同じ音を出すので、C/D を混ぜると二重に足される
- VRC7 は OPLL と同じ2部位を持つ（ymfm の動作に合わせる）
- チップが持つ部位を問い合わせる `FmEngine_GetPartMask(engine, chip_id,
  &mask)` を足す。bit n が `FmPart` の n 番に当たる。`AddChip` で得た
  chip_id に問い合わせる形だけにした（利用者の要望は「インスタンスから知れれば
  よい」）。戻り値と引数の形は `FmEngine_GetPartGain` に合わせ、未知の chip_id
  と null は `FM_ERR_INVALID_ARG` を返す

前提：部位が32個以下であること（マスクが `uint32_t`）。超えれば
`FmEngine::getPartMask()` の `static_assert` でビルドが止まる。

前提：ymfm の OPLL 系・ymf262・ymf278b の出力の数と並びが変わらないこと。
数が変われば `FmChipImpl::generateNative()` の `static_assert` でビルドが
止まる。数が同じまま並びだけ変われば、`part_gain_test` の engine が落ちる
（音から端子を確かめているため）。

やり直しの値段：名前を変えるならヘッダ、README 2本、テスト。番号を変えるのは
リリース後ならバイナリ互換を壊す。

見送った案：

- 番号をチップ間で使い回す（`OPLL_MELODY`=0 など）。理由：別のチップの部位を
  渡すと、黙って別の出力が変わる
- 名前に `FM_PART_` を付けない（`OPL3_AB` など）。理由：C の enum は
  グローバルな識別子になり、利用者のコードと衝突しうる
- OPN の旧名を残す。理由：利用者が命名をそろえる方を選んだ
- 部位を持つチップで `FM_PART_FM` を「出力全体」として受け付ける。理由：
  チップ全体 × 出力全体 × 部位の3段になり、`FmEngine_SetGain` と役割が重なる
- 出力が1本のチップに汎用の部位（`FM_PART_MAIN`）を残す。理由：
  `FmEngine_SetGain` と同じことしかできない
- C/D 側の既定値を 1.0 にする。理由：上の二重の足し込みが起き、既定の音が変わる
- VRC7 には部位を持たせない。理由：利用者が ymfm の動作に合わせる方を選んだ
- OPL4 を FM と AWM に分ける（前の節で「後で分ける」と書いた案）。理由：DO2 の
  中の FM と AWM はチップ内で混ぜ、比率はレジスタで決まり、混ぜたあとで切り
  詰める。部位で分けるとレジスタと役割が重なり、切り詰め方も変わる。分けるなら
  物理的な端子（DO0/DO1/DO2）の単位にした
- Y8950 を FM と ADPCM に分ける。理由：ymfm では和に DAC の丸めを掛けるので
  分けられない。実機で別の端子かも確かめられない
- 部位の問い合わせに専用の関数を足さず、`FmEngine_GetPartGain` が
  `FM_ERR_INVALID_ARG` を返すかで判定してもらう。理由：エラーで有無を判定する
  形になり、部位の数も呼び出し側のヘッダに頼ることになる
- チップ名から（`AddChip` の前に）部位を引く関数、部位の名前の文字列を返す
  関数。理由：利用者がインスタンスから知れれば足りるとした

### 実装

- OPLL 系・OPL3・OPL4 は、ymfm の出力を部位ごとに別々に出力レートへ変換し、
  ゲインを掛けてから L/R に混ぜる。リサンプラのチャンネル数は OPLL 系が2
  （今までと同じ）、OPL3 が4、OPL4 が6
- `PartGains` に `chip_l` / `chip_r`（チップのゲインだけ）を足し、部位を
  持たないチップはこれを使う
- `FmEngineApi.cpp` の `static_assert` で、C の `FmPart` と C++ の `ChipPart`
  の番号を照合する

既定値での出力：

- OPL3 と OPL4：今までと同じ。**未検証**：出力レートがネイティブレートと違う
  場合はコードからの見立て。チャンネルごとの補間の計算は変わらず、C/D 側に
  0 を掛けて足すだけ
- OPLL 系：今まではメロディ+リズムを1本で変換していたが、2本で変換してから
  足す。出力レートがネイティブレートと違う場合は浮動小数の丸めの範囲で変わり
  うる。**未検証**：差の大きさは測っていない
- いずれもネイティブレートでは今までと完全に一致する（下の確認）

性能：OPL3 と OPL4 は補間するチャンネルが増える。**未検証**：測っていない。

### 確認

`_test/part_gain_test.cpp` を追加した。**確認済み**（MSVC 19.51 と g++ 11.3
でビルドして実行、全件通過）：

- route：ネイティブレートで生成し、同じ書き込みをした上流のチップと全サンプルを
  比較した。OPLL/OPLLP/OPLLX/VRC7、OPL3、OPL4 の各部位で不一致 0。比べる出力は
  どれも鳴っている。対照（わざと別の出力と比べる）では 19,913〜20,000 サンプル
  食い違う
- 既定値：OPLL 系はメロディ+リズム、OPL3 は A/B、OPL4 は DO2 と完全一致
- 部位を持たない OPM・OPL：部位のゲインを全部 0 にしてもチップのゲインで鳴る
- 全16チップ × 9部位（と範囲外の番号）の受け付けと既定値。`getPartMask()` が
  チップごとの期待値（テスト側の表から作る）と一致し、未知の chip_id を拒否する
- engine：C/D にだけ出した FM チャンネルと AWM チャンネルは、既定では無音で、
  `OPL4_DO0` / `OPL4_DO1` を上げると聞こえる。OPLL のメロディだけを鳴らすと
  リズム側は無音、リズムだけならメロディ側は無音

試験が効いていることの確認（**確認済み**）：`FmChip.h` の写しで割り当てを
入れ替えた3通り（OPL4 の DO0⇔DO2、OPLL のメロディ⇔リズム、OPL3 の A/B⇔C/D）
を作り、それぞれで落ちることを見た。engine の試験は ymfm の出力の並びに頼らない
ので、単独でも落ちた。OPLL の既定値の試験は、メロディとリズムのゲインが
どちらも 1.0 なので、入れ替えを見分けられない（ほかの試験が落ちる）。

`opn_split_test` と `keyoff_retrigger_test` も、MSVC と g++ 11.3 で全件通った。
`opn_split_test` を g++ で走らせたのはこれが初めて。

DLL：`cl` で `FmEngineApi.cpp` と ymfm を直接ビルドし（警告なし）、
`FmEngine_SetPartGain` / `FmEngine_GetPartGain` / `FmEngine_GetPartMask` が
エクスポートされることを dumpbin で確認した。CMake では今回ビルドしていない
（**未検証**）。

C API の `FmEngine_GetPartMask`：**確認済み**（MSVC。リポジトリに残さない
確認用のプログラムを `FmEngineApi.cpp` と一緒にビルドして実行）。対応チップの
一覧の全16チップをチップ名で追加し、手で書いた期待値の表と一致した。未知の
chip_id、null の出力先、null のハンドルは `FM_ERR_INVALID_ARG` を返した。C API を
通す試験はリポジトリには無い。

気づいたが手を付けていないこと：ymfm の `ym2414`（OPZ）は2出力を作るが、
YMEngine は `data[0]` だけを使っている（コードで確認）。意図したものかは**未確認**。

## OPN 系の FM と SSG を本来のレートで別々に生成する

発端は openMSX PR #2209（Makoto/YM2608 対応）の議論。

### 穴

ymfm の `ym2203` / `ym2608` / `ym2610` の `generate()` は、FM と SSG を1本の
列にまとめるため、同じ値を繰り返して速い方のレートに揃える。YMEngine は
`set_fidelity()` を呼ばないので既定の `OPN_FIDELITY_MAX` になり、OPNA 標準
クロックで 998,400Hz の列を作る（prescale 6 では FM を18回、SSG を4回
繰り返す）。これを `LinearResampler` が線形補間で 44.1k/48k に落とす。

- 性能：1チップあたり毎秒約100万回ループする
- 音：繰り返しで平らになった列を線形補間すると、実質「最寄りのサンプルを
  拾う」動作になる。FM は約 55.5kHz → 48kHz を最寄り点で拾うのと同じになる。
  **推測**：コードから導いた。音も誤差も測っていない

### 方針

- `ym2203` / `ym2608` / `ym2610` を継承したクラスを作り、protected の
  `clock_fm()` / `clock_fm_and_adpcm()` / `m_last_fm` / `m_ssg` を使って FM と
  SSG を別々に clock する。上流の `generate()` は呼ばない
- `FmChipImpl` 側は FM 用と SSG 用に `LinearResampler` を2本持ち、目標レートに
  変換してから足す
- 本来のレートは prescale（reg 0x2D〜0x2F）で変わるので、書き込みのあとで
  prescale の変化を見てリサンプラを設定し直す
- 本来のレートは整数にならない（7,987,200/144 = 55,466.67Hz）。`LinearResampler`
  は整数のレートしか受け取らないため、clock と分周比の分数で持たせる

前提：上流 ymfm の OPN 系クラスの protected メンバの名前と構成が変わらない
こと。変わればコンパイルが通らなくなるので、黙って壊れることはない。

openMSX PR #2209 のスレッドで、同じ継承方式がすでに試されている。上流の
`generate()` と全サンプルが一致し、openMSX 上のコストは MAX の約半分だった
という報告がある。**未検証**：出典はスレッドの報告で、YMEngine では
確かめていない。一致の確認は、同スレッドの `native-rate-check.cc` と同じ
形の試験を `_test/` に置いて行う。

### 外から見える値（利用者と決めた）

- **部位ごとのゲイン**：実機では FM 出力と SSG 出力を外部回路でミックスする
  ため、音量バランスはハードウェアの実装によって違う。そこで部位ごとに
  ゲインを調整できるようにする。
  `FmEngine_SetPartGain(engine, chip_id, part, gain_l, gain_r)` と
  `FmEngine_GetPartGain` を足す。`part` は enum（`FM_PART_FM` / `FM_PART_SSG`）。
  既存の `FmEngine_SetGain` はチップ全体に掛けるゲインとして残し、実際の
  ゲインは「チップのゲイン × 部位のゲイン」にする。SSG を持たないチップに
  `FM_PART_SSG` を指定したら `FM_ERR_INVALID_ARG` を返す
- **部位ゲインの既定値 1.0 は今の音量に合わせる**：OPNA/OPNB/OPNBB は上流と
  同じく3チャンネルの和に 2/3 を掛け、OPN は3チャンネルをそのまま足す。
  既存の利用者の出力が変わらないことを優先した
- **`FmEngine_GetNativeRate` は FM のレートを返す**（OPNA 標準クロックで
  約 55,466Hz を整数に切り捨てた値。prescale を変えたあとはその時点の値）。
  今返している 998,400 から値が変わる

見送った案：

- 部位ごとに専用の関数を足す（`FmEngine_SetSsgGain` など）。理由：部位を
  分けるたびに関数が増える。OPL4 の FM/AWM などを後で分けるときに、同じ
  関数で済むようにした
- 2/3 を外し、OPN と OPNA 系で SSG の目盛りを揃える。理由：既定の出力で
  SSG が1.5倍になり、既存の利用者の音量バランスが変わる。openMSX のスレッド
  では「2/3 は1本にまとめていた頃の名残で、実機には無いのでは」という指摘が
  ある。ただ、部位ゲインで利用者が合わせられるので、既定値は今の音量を優先した
- `FmEngine_GetNativeRate` で今の値（998,400）を返し続ける。理由：内部では
  もう使わない値になる
- SSG のレートを取る関数を別に足す。理由：API が増える割に、利用する場面が
  見えていない
- `ssg_override`（ymfm が用意している SSG の差し替え口）を使う。理由：SSG は
  外に出せるが、FM の clock が protected にあるので、どのみち継承が要る
- `set_fidelity(OPN_FIDELITY_MIN)` だけにする。理由：ループ回数は約6分の1に
  なるが、FM を繰り返す処理と SSG を平均する処理は残る

### 実装中に見つけた `LinearResampler` の穴（全チップに影響）

旧実装は呼び出しのたびに「必要数 + 2」個のソースを生成し、次回へは位相の
端数だけを持ち越していた。使わなかった末尾の1〜2サンプルは捨てられていた。
捨てた分だけチップの時間が先に進むので、音程が上がり、呼び出しの境目で
波形が跳ぶ。

**確認済み**（旧実装に 0,1,2,... を出すソースをつなぎ、55,466Hz → 48,000Hz を
240 サンプルずつ 200 回呼んだ）：ソースの消費が期待の 55,466 に対して 55,800
（+0.60%、1回あたり約1.7サンプル）。影響は本来のレートが出力レートに近い
チップほど大きい。旧 OPN 系は約 1MHz から落としていたので 0.03% 程度だった
が、分離すると 55kHz から落とすので他のチップと同じ程度になる。

直し方：必要な分だけ生成し、まだ使い終わっていないサンプルを次回へ持ち越す
（補間の左端。アップサンプルでは右端も持ち越すので最大2個）。ソースのレートは
分数（src_num / src_den）で受け取るようにした。この修正で OPN 系以外のチップも
出力が変わる。

### 確認

`_test/opn_split_test.cpp` を追加した。**確認済み**（MSVC 19.51 でビルドして実行、全件通過）：

- リサンプラ：55,466→48,000（240 ずつ・1 ずつ）、7,987,200/144→48,000、
  7,987,200/32→44,100（7 ずつ）、44,100→48,000 で、ソースの消費が期待値の
  ±2 以内、出力の直線からのずれが 0.01 以下。44,100→48,000 は、持ち越しを
  1個に限っていた途中版で +99 サンプルの読み捨てが出て落ち、2個に広げて通った
- 上流 `generate()` との一致：YM2203 / YM2608 / YM2610 / YM2610B で、FM・SSG
  ともに不一致 0（FM・SSG が鳴っているサンプルはそれぞれ約1.9万）。YM2203 と
  YM2608 は prescale 6 → 2 → 6 → 3 の切り替えを含む。対照として、派生側で
  KEY ON を抜くと FM が、SSG の音量を抜くと SSG が約1.3万サンプル食い違う
- `FmEngine`：ネイティブレート（OPN/OPNA 55,466、OPNB 55,555、OPNA の
  prescale 2 で 166,400）、既定の SSG 音量（OPNA は 3ch 和 × 2/3、OPN は
  そのまま。矩形波のピークが計算値と 1e-3 以内）、部位ゲイン（0 で無音、
  チップのゲインとの積、SSG を持たない OPL3 と未知の chip_id は拒否）

`keyoff_retrigger_test` も全件通った。**確認済み**（変更前と変更後のソースで
同じ試験を走らせた）：1サンプルずつ呼ぶ tiny 条件の値が、まとめて呼ぶ batch
条件と完全に一致するようになった。変更前は、1ms窓ピークの最小値が OPL3 ch0 で
tiny 0.0000 / batch 0.0021、OPM ch0 で 0.0038 / 0.0400 と食い違っていた。
呼び出しの分け方で出力が変わっていたのは、上のリサンプラの穴による。

DLL（CMake、Visual Studio 18 2026、Release）のビルドが通り、
`FmEngine_SetPartGain` / `FmEngine_GetPartGain` がエクスポートされることを
dumpbin で確認した。g++ ではビルドしていない（**未検証**）。

性能（**確認済み**、1台の Windows 10 機で、OPNA/OPN を1個、FM 1ch と SSG
3ch を鳴らしたまま 48kHz で60秒ぶんを 480 サンプルずつ生成、3回）：

| | 変更前 | 変更後 |
|---|---|---|
| OPNA | 1,786〜1,813ms | 932〜1,153ms |
| OPN  | 1,384〜1,551ms | 549〜614ms |

試験していないこと：

- ADPCM-A/B とリズムに実データを流したときの一致。**推測**：上流の
  `clock_fm_and_adpcm()` の中で FM と一緒に clock され `m_last_fm` に足される
  ので、FM と同じ経路で一致する。根拠はコードを読んだことだけ
- prescale を切り替えた瞬間の音（リサンプラを設定し直すときの過渡）

残る問題：`LinearResampler` には低域通過フィルタが無い。SSG の矩形波を
250kHz → 48kHz に線形補間で落とすと、折り返しが出る。分離すれば経路ごとに
変換方式を選べるようになるので、別の変更として扱う。

## ymfm を 17decfa から 81aec25 に更新

一度 `261d905` で 81aec25 に上げたが、`1b0072e` で 17decfa に戻っていた。
戻したのは別マシンのリポジトリと状態を合わせるためで、内容に問題があった
わけではない（利用者の記憶による）。差分を読み直して取り込んだ。

| 上流コミット | 内容 | 影響 |
|---|---|---|
| `6cc0c4a` | OPZ の LFO 波形と位相の端数を初期化する | 未初期化メモリに左右されなくなる |
| `e9f57df` | ymfm のサンプルプログラムの修正 | 無し（ビルドしていない） |
| `2252890` | OPL2 の波形選択を reg 0x01 bit5 (WSE) が立っているときだけ有効にする | OPL2 で 0x01 に 0x20 を書かないと正弦波になる |
| `81aec25` | OPN 系の F-Number 上位ラッチを保持し続け、下位の書き込みごとに適用する。OPNA の両バンクでラッチを共有する | 旧版は直前に上位を書いていない下位の書き込みを捨てていた |

後の2件は利用者から見える挙動の変化なので、README の「チップ固有のレジスタの扱い」に
書いた。どちらも実機の挙動に合わせる修正と判断した。**推測**：根拠は上流の
PR の説明とコード。OPNA のラッチが1組であることを実機で確かめる手段は無い。

**確認済み**：DLL (MSVC Release) のビルドが通る。`keyoff_retrigger_test` は
MSVC と g++ 10.2 の両方で全件通る。

## キー衝突時の先行生成を呼び出しをまたいで保留する (FmEngine.h)

[Y8960emu](https://github.com/madscient/Y8960emu) の `e27862a` と同じ方式を移植した。

### 穴

`FmEngine::generate()` は、同じチャンネルスロットのキー状態が未観測のまま
再び変わろうとすると、書き込みの前に `minKeyOnTickSamples()`（約2ms）を
先に生成していた。この先行生成はその呼び出しの `samples` の中でしか行わず、
使い切ったあとの衝突は間を空けずに書き込んでいた。ymfm はキー状態を次の
サンプル生成時にしか見ないため、KEY OFF が観測されなかった。

- 衝突が多いとき（240サンプルずつの呼び出しで、5チャンネル以上が同時に
  KEY OFF → KEY ON）：後ろのチャンネルの KEY OFF が消える
- 呼び出しが細かいとき（`generate(1)` など）：先行生成が端数しか取れず、
  KEY OFF が1サンプル程度しか観測されない

OPL4 の AWM（port2）も同じ穴を持つ。ymfm の `pcm_channel::keyonoff()` は
保留フラグを立てるだけで、状態の切り替えは次の `prepare()` で行うため
（ymfm のコードで確認）。

### 直し方

- 衝突した書き込みを `m_pending` に保留し、前の状態のまま
  `minKeyOnTickSamples()` 分を生成し終えてから適用する
- 保留中の生成は呼び出しをまたいで数える。今回の `samples` に収まらない分は、
  次の呼び出しの頭で続きを生成してから適用する
- 保留中は後続の書き込みも適用しない（順序を保つ）
- `keyOnTransitionMask()` は直前値キャッシュを更新するので、保留する書き込みでも
  pop した時点で1回だけ呼び、マスクは保留と一緒に持つ
- 未観測の追跡（`m_keyDirtyMask`）のクリアを、呼び出しの頭から「1サンプル以上
  生成した時点」（`renderSpan()`）に移す。呼び出しの末尾で `samples` を使い
  切ったあとに適用した書き込みは、次の呼び出しの頭でもまだ未観測だからである

`FmChip.h` の `keyOnTransitionMask()` とスロットの割り当ては変えていない。

前提：「KEY OFF/ON の状態は最低約2ms観測させる」ことを、発音タイミングの
正確さより優先する。この前提が変わらない限り、この方式が成り立つ。

代償：衝突1回につき、書き込みの適用が最大約2ms遅れる。衝突が N 回あれば
最大 N × 約2ms。**推測**：設計から導いた値で、遅れそのものは測っていない。

見送った案：「余地が尽きたら、衝突した書き込みから後を次の呼び出しの頭で
適用する」。理由：余地が尽きる直前に適用した書き込みは、まだ1サンプルも
生成されていない。次の呼び出しの頭で続きを適用すると、やはり観測されない。

### 確認

`_test/keyoff_retrigger_test.cpp` を追加した。KEY OFF → KEY ON を次の3条件で
与え、KEY OFF が観測されるかを見る。

- batch：1回の呼び出しにまとめて渡す
- crowd：先に5チャンネルが KEY OFF → KEY ON し、240サンプルずつ呼ぶ
- tiny：1サンプルずつ呼ぶ

判定は、持続音なら1ms窓ピークの谷（dip）、減衰しきった打楽器なら打撃の
再立ち上がり（hit）で見る。

**確認済み**（MSVC でビルドして実行。`CLAUDE.md` の `cl` の手順でビルドできる
ことも確認した。`g++` の手順でも g++ 10.2 (Debian 11) でビルドでき、直したあとの
全件が同じ値で通った）：

| ケース | 判定 | 直す前 batch / crowd / tiny | 直したあと |
|---|---|---|---|
| OPL3 ch0 | dip | OK / FAIL / FAIL | 全 OK |
| OPL3 リズム BD | hit | OK / FAIL / OK | 全 OK |
| OPNA ch0 | dip | OK / FAIL / FAIL | 全 OK |
| OPM ch0 | dip | OK / FAIL / FAIL | 全 OK |
| OPLL ch0 | dip | OK / FAIL / OK | 全 OK |
| OPLL リズム BD | hit | OK / FAIL / OK | 全 OK |
| OPL4 AWM ch0 | dip | OK / FAIL / FAIL | 全 OK |

直す前の crowd では、リズム BD の打撃が鳴らなかった（`after OFF/ON=0.0000`）。
直す前の tiny では、OPL3 ch0 の1ms窓ピークが 0.2438 までしか落ちなかった
（持続時の 0.2462 に対して）。直したあとは 0.0000 まで落ちた。

直す前でも tiny が通ったケースについての見立て（**推測**）：

- hit 判定は、減衰しきった状態から打撃が立ち上がるかだけを見る。KEY OFF が
  1サンプルでも観測されればアタックが始まるので、KEY OFF の長さを判定できない
- OPLL の dip が通るのは、ymfm の OPLL がキーオン時に独自の減衰段を持つためと
  Y8960emu では見ている。ymfm のコードでは確かめていない
