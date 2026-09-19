// keyoff_retrigger_test.cpp
// 回帰テスト: 同じチャンネルへの KEY OFF → KEY ON がキューに同居しても、
// KEY OFF が観測されることを確認する。次の3つの条件で見る。
//   batch : 1回の generate() にまとめて渡す
//   crowd : 先に他の5チャンネルが同じく KEY OFF → KEY ON し、
//           generate() を 240 サンプルずつ呼ぶ (衝突が多く1回の呼び出しに収まらない)
//   tiny  : generate() を 1 サンプルずつ呼ぶ
//
// 判定は2種類。
//   dip : 減衰しない持続音。KEY OFF が観測されればリリースで音量が一度大きく
//         落ちてから戻る。観測されない (または一瞬しか観測されない) と落ちない。
//   hit : 減衰しきった打楽器。KEY OFF が観測されれば再び打撃が立ち上がる。
//
// 対象チャンネルは ch0 (リズムは BD)。発音は約3kHz 前後の単一オペレータ
// (AWM は矩形波) で、AR 最大・減衰なし・RR 最大 (KEY OFF で即座に消える)。
// ch1-5 は衝突を起こすためだけのチャンネルで、音は出さない (AR=0 のまま
// アタックしない。OPLL は F-Number=0)。
//
#include "FmEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr uint32_t kRate = 48000;

static float peak(const std::vector<float>& v, size_t from, size_t to) {
    float p = 0.0f;
    for (size_t i = from; i < to; ++i) p = std::fmax(p, std::fabs(v[i]));
    return p;
}

// 1ms 窓ごとのピークの最小値。発音は1窓に数周期入る高さにしてあるので、
// 波形の位相では窓ピークが落ちない。
static float minWindowPeak(const std::vector<float>& v) {
    constexpr size_t kWin = kRate / 1000;
    float m = 1e9f;
    for (size_t i = 0; i + kWin <= v.size(); i += kWin / 2)
        m = std::fmin(m, peak(v, i, i + kWin));
    return m;
}

struct Write3 { uint8_t reg, on, off; };

// ---- OPL3 -------------------------------------------------------------
// NEW=1 で出力先 (C0 の bit4-7) を有効にする。F-Number=0x200, Block=7, MUL=1。
static void setupOpl3(FmEngine& eng, uint32_t id) {
    eng.write(id, 0x05, 0x01, 1);
    eng.write(id, 0x20, 0x21); eng.write(id, 0x40, 0x3F); eng.write(id, 0x60, 0xFF); eng.write(id, 0x80, 0x0F);
    eng.write(id, 0x23, 0x21); eng.write(id, 0x43, 0x00); eng.write(id, 0x63, 0xF0); eng.write(id, 0x83, 0x0F);
    eng.write(id, 0xA0, 0x00); eng.write(id, 0xC0, 0xF0);
}
static const Write3 kOpl3Target = { 0xB0, 0x3E, 0x1E };
static const Write3 kOpl3Others[] = {
    { 0xB1, 0x20, 0x00 }, { 0xB2, 0x20, 0x00 }, { 0xB3, 0x20, 0x00 },
    { 0xB4, 0x20, 0x00 }, { 0xB5, 0x20, 0x00 },
};

// リズムモードの BD (ch6)。キャリアは EG-TYP=0 (減衰音) の DR=7 で、1秒後には
// 減衰しきっている。
static void setupOpl3Rhythm(FmEngine& eng, uint32_t id) {
    eng.write(id, 0x05, 0x01, 1);
    eng.write(id, 0x30, 0x01); eng.write(id, 0x50, 0x3F); eng.write(id, 0x70, 0xFF); eng.write(id, 0x90, 0x0F);
    eng.write(id, 0x33, 0x01); eng.write(id, 0x53, 0x00); eng.write(id, 0x73, 0xF7); eng.write(id, 0x93, 0xFF);
    eng.write(id, 0xA6, 0x00); eng.write(id, 0xB6, 0x1E); eng.write(id, 0xC6, 0xF0);
    eng.write(id, 0xBD, 0x20);
}
static const Write3 kOpl3RhythmTarget = { 0xBD, 0x30, 0x20 };

// ---- OPNA -------------------------------------------------------------
// ch0 は op1 だけを鳴らす (CON=7, op2-4 は TL=127)。F-Number=0x400, Block=7。
static void setupOpna(FmEngine& eng, uint32_t id) {
    eng.write(id, 0xB0, 0x07); eng.write(id, 0xB4, 0xC0);
    eng.write(id, 0x30, 0x01); eng.write(id, 0x40, 0x00); eng.write(id, 0x50, 0x1F);
    eng.write(id, 0x60, 0x00); eng.write(id, 0x70, 0x00); eng.write(id, 0x80, 0x0F);
    eng.write(id, 0x44, 0x7F); eng.write(id, 0x48, 0x7F); eng.write(id, 0x4C, 0x7F);
    eng.write(id, 0xA4, 0x3C); eng.write(id, 0xA0, 0x00);
}
// reg 0x28 の bit0-2 がチャンネル (0-2, 4-6)、bit4 が op1。
static const Write3 kOpnaTarget = { 0x28, 0x10, 0x00 };
static const Write3 kOpnaOthers[] = {
    { 0x28, 0x11, 0x01 }, { 0x28, 0x12, 0x02 }, { 0x28, 0x14, 0x04 },
    { 0x28, 0x15, 0x05 }, { 0x28, 0x16, 0x06 },
};

// ---- OPM --------------------------------------------------------------
// ch0 は M1 だけを鳴らす (CON=7, 他は TL=127)。KC=A4, MUL=7。
static void setupOpm(FmEngine& eng, uint32_t id) {
    eng.write(id, 0x20, 0xC7); eng.write(id, 0x28, 0x4A);
    eng.write(id, 0x40, 0x07); eng.write(id, 0x60, 0x00); eng.write(id, 0x80, 0x1F);
    eng.write(id, 0xA0, 0x00); eng.write(id, 0xC0, 0x00); eng.write(id, 0xE0, 0x0F);
    eng.write(id, 0x68, 0x7F); eng.write(id, 0x70, 0x7F); eng.write(id, 0x78, 0x7F);
}
// reg 0x08 の bit0-2 がチャンネル、bit3 が M1。
static const Write3 kOpmTarget = { 0x08, 0x08, 0x00 };
static const Write3 kOpmOthers[] = {
    { 0x08, 0x09, 0x01 }, { 0x08, 0x0A, 0x02 }, { 0x08, 0x0B, 0x03 },
    { 0x08, 0x0C, 0x04 }, { 0x08, 0x0D, 0x05 },
};

// ---- OPLL -------------------------------------------------------------
static void setupOpll(FmEngine& eng, uint32_t id) {
    eng.write(id, 0x00, 0x21); eng.write(id, 0x01, 0x21);
    eng.write(id, 0x02, 0x3F); eng.write(id, 0x03, 0x00);
    eng.write(id, 0x04, 0xFF); eng.write(id, 0x05, 0xF0);
    eng.write(id, 0x06, 0x0F); eng.write(id, 0x07, 0x0F);
    eng.write(id, 0x10, 0x00); eng.write(id, 0x30, 0x00);
}
static const Write3 kOpllTarget = { 0x20, 0x1F, 0x0F };
static const Write3 kOpllOthers[] = {
    { 0x21, 0x10, 0x00 }, { 0x22, 0x10, 0x00 }, { 0x23, 0x10, 0x00 },
    { 0x24, 0x10, 0x00 }, { 0x25, 0x10, 0x00 },
};

// リズムモードの BD。ch6-8 の F-Number/Block は MSX-MUSIC の慣用値。
static void setupOpllRhythm(FmEngine& eng, uint32_t id) {
    eng.write(id, 0x16, 0x20); eng.write(id, 0x26, 0x05);
    eng.write(id, 0x17, 0x50); eng.write(id, 0x27, 0x05);
    eng.write(id, 0x18, 0xC0); eng.write(id, 0x28, 0x01);
    eng.write(id, 0x36, 0x00); eng.write(id, 0x37, 0x00); eng.write(id, 0x38, 0x00);
    eng.write(id, 0x0E, 0x20);
}
static const Write3 kOpllRhythmTarget = { 0x0E, 0x30, 0x20 };

// ---- OPL4 AWM (port2) -------------------------------------------------
// 波形メモリに波形0のヘッダ (12バイト) と 16 サンプルの 8bit 矩形波を置く。
// ヘッダの後半5バイトは load_wavetable() が LFO/VIB, AR/DR, SL/SR, RC/RR, AM
// の各レジスタに書き込む値で、AR=15 / DR=0 / RR=15 / RC=15 にしてある。
static std::vector<uint8_t> makeAwmMemory() {
    constexpr uint32_t kBase = 0x1000;
    constexpr uint32_t kLen  = 16;
    std::vector<uint8_t> mem(kBase + kLen, 0);
    const uint8_t header[12] = {
        0x00, (kBase >> 8) & 0xFF, kBase & 0xFF,       // 8bit, 開始アドレス
        0x00, 0x00,                                    // ループ位置 0
        (0x10000 - kLen) >> 8, (0x10000 - kLen) & 0xFF, // 終了位置 (2の補数)
        0x00, 0xF0, 0x00, 0xFF, 0x00,
    };
    std::copy(header, header + 12, mem.begin());
    for (uint32_t i = 0; i < kLen; ++i) mem[kBase + i] = (i < kLen / 2) ? 0x60 : 0xA0;
    return mem;
}
static const std::vector<uint8_t> kAwmMemory = makeAwmMemory();

// NEW2=1 にしないと port2 への書き込みが無視される。オクターブ 1, F-Number 0。
static void setupOpl4Awm(FmEngine& eng, uint32_t id) {
    eng.setMemory(id, ymfm::ACCESS_PCM, kAwmMemory.data(), static_cast<uint32_t>(kAwmMemory.size()));
    eng.write(id, 0x05, 0x03, 1);
    eng.write(id, 0x20, 0x00, 2); eng.write(id, 0x38, 0x10, 2);
    eng.write(id, 0x08, 0x00, 2); eng.write(id, 0x50, 0x01, 2);
}
static const Write3 kOpl4AwmTarget = { 0x68, 0x80, 0x00 };
static const Write3 kOpl4AwmOthers[] = {
    { 0x69, 0x80, 0x00 }, { 0x6A, 0x80, 0x00 }, { 0x6B, 0x80, 0x00 },
    { 0x6C, 0x80, 0x00 }, { 0x6D, 0x80, 0x00 },
};

enum class Judge { Dip, Hit };

struct Case {
    const char* label;
    const char* chip;
    uint32_t port;
    void (*setup)(FmEngine&, uint32_t);
    Write3 target;
    const Write3* others;
    size_t otherCount;
    Judge judge;
};

enum class Mode { Batch, Crowd, Tiny };

static bool run(const Case& c, Mode mode) {
    FmEngine eng(kRate);
    const uint32_t id = eng.addChipByName(c.chip);
    c.setup(eng, id);
    for (size_t i = 0; i < c.otherCount; ++i) eng.write(id, c.others[i].reg, c.others[i].on, c.port);
    eng.write(id, c.target.reg, c.target.on, c.port);

    std::vector<float> l(kRate), r(kRate);
    eng.generate(l.data(), r.data(), kRate);
    const float attack = peak(l, 0, 2048);
    const float held   = peak(l, kRate - 2048, kRate);

    if (mode == Mode::Crowd)
        for (size_t i = 0; i < c.otherCount; ++i) {
            eng.write(id, c.others[i].reg, c.others[i].off, c.port);
            eng.write(id, c.others[i].reg, c.others[i].on, c.port);
        }
    eng.write(id, c.target.reg, c.target.off, c.port);
    eng.write(id, c.target.reg, c.target.on, c.port);

    const uint32_t chunk = (mode == Mode::Batch) ? 4096 : (mode == Mode::Crowd) ? 240 : 1;
    std::vector<float> l2(4800), r2(4800);
    for (uint32_t pos = 0; pos < l2.size(); pos += chunk) {
        const uint32_t n = std::min<uint32_t>(chunk, static_cast<uint32_t>(l2.size()) - pos);
        eng.generate(l2.data() + pos, r2.data() + pos, n);
    }

    const char* modeName = (mode == Mode::Batch) ? "batch" : (mode == Mode::Crowd) ? "crowd" : "tiny ";
    bool ok;
    if (c.judge == Judge::Dip) {
        const float dip  = minWindowPeak(l2);
        const float tail = peak(l2, l2.size() - 480, l2.size());
        ok = held > 0.05f && dip < held * 0.3f && tail > held * 0.8f;
        std::printf("[%s] %-8s %s dip : held=%.4f min1ms=%.4f tail=%.4f\n",
                    ok ? "OK" : "FAIL", c.label, modeName, held, dip, tail);
    } else {
        const float retrig = peak(l2, 0, l2.size());
        ok = attack > 0.05f && held < attack * 0.1f && retrig > attack * 0.5f;
        std::printf("[%s] %-8s %s hit : attack=%.4f held=%.4f after OFF/ON=%.4f\n",
                    ok ? "OK" : "FAIL", c.label, modeName, attack, held, retrig);
    }
    return ok;
}

int main() {
    const Case cases[] = {
        { "OPL3",    "OPL3", 0, setupOpl3,       kOpl3Target,       kOpl3Others,    5, Judge::Dip },
        { "OPL3 BD", "OPL3", 0, setupOpl3Rhythm, kOpl3RhythmTarget, kOpl3Others,    5, Judge::Hit },
        { "OPNA",    "OPNA", 0, setupOpna,       kOpnaTarget,       kOpnaOthers,    5, Judge::Dip },
        { "OPM",     "OPM",  0, setupOpm,        kOpmTarget,        kOpmOthers,     5, Judge::Dip },
        { "OPLL",    "OPLL", 0, setupOpll,       kOpllTarget,       kOpllOthers,    5, Judge::Dip },
        { "OPLL BD", "OPLL", 0, setupOpllRhythm, kOpllRhythmTarget, kOpllOthers,    5, Judge::Hit },
        { "OPL4 AWM","OPL4", 2, setupOpl4Awm,    kOpl4AwmTarget,    kOpl4AwmOthers, 5, Judge::Dip },
    };
    bool ok = true;
    for (const Case& c : cases)
        for (Mode m : { Mode::Batch, Mode::Crowd, Mode::Tiny })
            ok = run(c, m) && ok;
    return ok ? 0 : 1;
}
