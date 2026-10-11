# UPSTREAM

AI 向けの記録。上流の ymfm（https://github.com/aaronsgiles/ymfm）に挙げた issue と、
その追い方。YMEngine の開発経緯は `CHANGELOG.md`。

## 追い方

- 状態を見る：
  `gh issue view <番号> -R aaronsgiles/ymfm --json state,closedAt,comments`
- 見たら、下の「最後に見た状態」を書き換える。作者から質問が付いていたら、
  返事の文面を作って利用者に見せる（投稿は利用者の了承を得てから）
- `extern/ymfm` を更新する前に、下の issue が閉じているか、関連するコミットが
  入っているかを見る。入っていたら、それぞれの「入ったら」を実施する
- 新しく issue を挙げたら、ここに節を足す

## 最後に見た状態

2026-10-11：#76、#77、#78 はオープンでコメントなし。関連の #68 と PR #40 も
オープン。上流の HEAD は `81aec25` で、`extern/ymfm` が固定しているコミットと同じ。

## YMEngine から挙げた issue

どれも 2026-10-11 に、ymfm `81aec25` を対象に挙げた。根拠の引用と測定値は issue の
本文にある。

### #76 ADPCM-B の ROM/RAM 選択と、Y8950 の 64K ビット

https://github.com/aaronsgiles/ymfm/issues/76

1. YM2608 と Y8950 の ROM/RAM 選択ビットが、`ymfm_external_read()` /
   `ymfm_external_write()` から見えない。ymfm はこのビットをアドレスの刻みにしか
   使わず、アクセスは `ACCESS_ADPCM_B` と番地だけで出てくる
2. Y8950 のレジスタ `$08` の bit1 は、資料では DRAM の種類（64K / 256K）の指定で、
   Y8950 に 8bit DRAM モードは無い。ymfm は `dram_8bit()` として扱い、刻みを
   32 バイトにする

入ったら：

- 1 について。YMEngine は選択ビットを覗くために `detail::Y8950Mem` と
  `detail::Ym2608Split::adpcmBRegs()` を挟み、`MemoryYmfmInterface::bindAdpcmBRegs()`
  で `ADPCM_B` / `ADPCM_B_ROMMODE` に振り分けている。上流が用意した手段（別の
  アクセス種別、または公開のアクセサ）に置き換え、`memory_map_test` と
  `c_api_test` を走らせる
- 2 について。YMEngine は ymfm の動作をそのまま通しているので、コードの変更は
  要らない。`memory_map_test` の route は、bit1 を立てても `ADPCM_B` を読むことだけ
  を見ていて、刻みには依らない（**未検証**：テストのコードを読んだ見立てで、
  刻みを変えて走らせてはいない）

### #77 Y8950 の ADPCM-DATA（`$0F`）の読み出し

https://github.com/aaronsgiles/ymfm/issues/77

`y8950::read_data()` が `$0F` ではなく `$09` と `$1A` を ADPCM-B に回していて、
レジスタ経由の読み出しが外部メモリに届かない。

入ったら：YMEngine の変更は要らない。YMEngine の API にはチップのレジスタを読む
関数が無く、この経路は呼ばれない。

### #78 YM2413 の EG

https://github.com/aaronsgiles/ymfm/issues/78

1. EG のレンジと分解能が 10 ビット・96dB で、実チップ（7 ビット・0.375dB 刻み・
   48dB）と違う
2. アタックが OPL と同じ曲線で、実チップの粗い階段状と違う
3. DP と、固定のリリースレート（RR=7、RS=5）に KSR が掛からない
4. モジュレータがキーオフでリリースに入る（既存の #68 と同じ内容で、参照だけした）

入ったら：OPLL 系（YM2413、YMF281、YM2423、VRC7）の出力が変わる。CLAUDE.md の
とおり全テストを走らせる。`keyoff_retrigger_test` は OPLL も対象で、KEY OFF の谷が
出るかどうかがリリースの速さに依る（CLAUDE.md の同テストの説明）ので、判定が
変わっていないかを確かめる（**未検証**：EG が変わった ymfm で走らせたことはない）。

## 関連する上流の issue と PR

YMEngine が挙げたものではないが、上と一緒に見る。

- #68（オープン）：OPLL のモジュレータがキーオフでリリースに入る。#78 の 4 と
  同じ内容
- PR #40（オープン、2022 年から）：ADPCM-B の改修（EOS / PLAYING フラグ、ダミー
  リード、終端の扱い）。作者が 2026-07-27 に取り込む意向を書いている。#76 と #77
  の箇所には触れていない（**確認済み**：2026-10-11 に差分を読み、
  `address_shift()` の分岐、アクセス種別、`y8950::read_data()` に変更が無いことを
  見た）。入ると ADPCM-B の再生が変わるので、`memory_map_test` を走らせる

## 根拠にした資料

- Y8950 Application Manual の 3-1-7（レジスタ `$08`）、3-1-11（`$0F`）、3-7、4-2 と
  Fig. 4-3。#76 と #77 の本文に引用がある
- "ADSR behavior of OPLL series"
  https://sites.google.com/site/undocumentedsoundchips/undocumented-tips/adsr-behavior-of-opll-series
- 参照実装：emu2413 `a1d5120`（v1.6.4）、Nuked-OPLL `1269cf5`

## 測り直すとき

issue の数値は、YMEngine を通さずに ymfm のチップを直接動かして測った。測定用の
プログラムはリポジトリに入れていない。直ったかどうかを確かめるには：

- ADPCM-B：`ymfm::ymfm_interface` を派生し、`ymfm_external_read()` /
  `ymfm_external_write()` に来たアクセス種別と番地を記録する。start=0x0010、
  end=0x0012 で再生し、ROM / bit1 / どちらも 0 の3通りで読まれた番地の範囲を比べる
- Y8950 の読み出し：`$07`=0x20 にしてから `$0F` を6回読み、外部メモリの読み出しが
  起きるかを見る
- YM2413 の EG：`ymfm::ym2413` を派生して `m_fm.debug_operator(n)` を取り、
  `debug_eg_attenuation()` と `debug_eg_state()` を1サンプルごとに読む（チャンネル 0
  はオペレータ 0 がモジュレータ、1 がキャリア）。`generate()` は1サンプルずつ呼ぶ
