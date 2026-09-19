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

全件通れば終了コード 0 を返す。

Windows（vcvars64.bat を通した環境、リポジトリ直下で）：
```cmd
cl /std:c++20 /EHsc /O2 /utf-8 /I src /I extern\ymfm\src _test\keyoff_retrigger_test.cpp extern\ymfm\src\ymfm_*.cpp
keyoff_retrigger_test.exe
```

Linux / macOS（**未検証**）：
```bash
g++ -std=c++20 -O2 -I src -I extern/ymfm/src _test/keyoff_retrigger_test.cpp extern/ymfm/src/ymfm_*.cpp -o keyoff_retrigger_test
./keyoff_retrigger_test
```

`FmEngine::generate()` のキュー消化や `FmChip::keyOnTransitionMask()` を
変えたら `keyoff_retrigger_test` を走らせる。テストで使う音色は、リリースの
速いもの（RR 最大）にする。リリースが遅いと、約2ms の KEY OFF では谷が出ず
判定できない。
