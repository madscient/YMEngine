// part_gain_test.cpp
// 部位ごとのゲイン (FmChip.h の ChipPart と FmChipImpl::mixParts) の回帰テスト。
//
//   route  : OPLL 系・OPL3・OPL4 で、部位のゲインが ymfm のどの出力に掛かるか。
//            ネイティブレートで生成して (リサンプラを素通しにして)、同じ書き込みを
//            した上流のチップの出力と全サンプルを比べる。部位ごとに別の音を鳴らし、
//            比べる出力がどれも鳴っていることを確かめる。対照として、わざと別の
//            出力と比べて不一致になることも確かめる
//   default: 既定のゲインで、OPLL 系はメロディ+リズム、OPL3 は A/B、OPL4 は DO2
//            だけが出ること
//   chip   : 部位を持たないチップにチップのゲインが掛かり、部位のゲインは
//            使われないこと
//   accept : 全チップ × 全部位で、受け付ける組み合わせと既定値
//   engine : FmEngine 経由で、チップのゲインと部位のゲインが掛かること。
//            各部位が実際にその端子の音を出すこと (C/D にだけ出したチャンネルが
//            A/B 側から聞こえないこと、など)。route は ymfm の出力の並びを前提に
//            期待値を作るので、上流で並びが変わったときはこちらで落ちる
//
// 全件通れば終了コード 0。

#include "FmEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_fail;
}

static constexpr float    kScale   = 1.0f / 32768.0f;
static constexpr uint32_t kSamples = 20000;

struct W { uint32_t port; uint8_t reg; uint8_t val; };

static size_t at(ChipPart p) { return static_cast<size_t>(p); }

static PartGains zeroGains() {
    PartGains g{};
    return g;
}

static PartGains defaultGains() {
    PartGains g{};
    for (uint32_t p = 0; p < kChipPartCount; ++p)
        g.l[p] = g.r[p] = defaultPartGain(static_cast<ChipPart>(p));
    g.chip_l = g.chip_r = 1.0f;
    return g;
}

// 指定した部位だけに L=0.5 / R=0.25 を掛ける。L と R を取り違えても分かるよう
// 値を変えてある。チップのゲインは 0 にして、部位を持つチップが使わないことも見る
static PartGains only(ChipPart p) {
    PartGains g = zeroGains();
    g.l[at(p)] = 0.5f;
    g.r[at(p)] = 0.25f;
    return g;
}

using Expect = std::function<void(const int32_t*, float&, float&)>;

// ymfm の data[kl] / data[kr] に L=0.5 / R=0.25 を掛けた値を期待する
static Expect pick(size_t kl, size_t kr) {
    return [=](const int32_t* d, float& l, float& r) {
        l = static_cast<float>(d[kl]) * kScale * 0.5f;
        r = static_cast<float>(d[kr]) * kScale * 0.25f;
    };
}

// =========================================================
//  route / default / chip
// =========================================================
struct RunResult {
    uint64_t mismatch = 0;
    std::vector<uint64_t> active;   // ymfm の data[k] が 0 でなかったサンプル数
};

template<typename Ref>
static RunResult run(ChipType type, const std::vector<W>& prog, const PartGains& g,
                     const Expect& expect, const std::vector<uint8_t>* pcm) {
    MemoryYmfmInterface iface;
    if (pcm) iface.setMemory(ymfm::ACCESS_PCM, pcm->data(), static_cast<uint32_t>(pcm->size()));
    Ref ref(iface);
    ref.reset();

    auto chip = createChip(type);
    if (pcm) chip->setMemory(ymfm::ACCESS_PCM, pcm->data(), static_cast<uint32_t>(pcm->size()));
    chip->setTargetRate(chip->nativeRate());

    for (const W& w : prog) {
        chip->write(w.port, w.reg, w.val);
        ref.write(w.port * 2, w.reg);
        ref.write(w.port * 2 + 1, w.val);
    }

    std::vector<float> l(kSamples), r(kSamples);
    chip->generate(l.data(), r.data(), kSamples, g);

    RunResult res;
    res.active.assign(Ref::OUTPUTS, 0);
    typename Ref::output_data od;
    for (uint32_t i = 0; i < kSamples; ++i) {
        ref.generate(&od);
        float el, er;
        expect(od.data, el, er);
        if (l[i] != el || r[i] != er) ++res.mismatch;
        for (uint32_t k = 0; k < Ref::OUTPUTS; ++k)
            if (od.data[k] != 0) ++res.active[k];
    }
    return res;
}

// match=true なら全サンプル一致、false (対照) なら不一致があることを期待する。
// mustBeActive に挙げた ymfm の出力が鳴っていなければ、比較が空振りしているので落とす
template<typename Ref>
static void checkRun(const char* chip, const char* what, ChipType type,
                     const std::vector<W>& prog, const PartGains& g, const Expect& expect,
                     std::initializer_list<size_t> mustBeActive, bool match,
                     const std::vector<uint8_t>* pcm = nullptr) {
    const RunResult res = run<Ref>(type, prog, g, expect, pcm);
    bool active = true;
    for (size_t k : mustBeActive) active = active && res.active[k] > 0;
    std::string act;
    for (size_t k = 0; k < res.active.size(); ++k)
        act += (k ? "," : "") + std::to_string(res.active[k]);
    char msg[320];
    std::snprintf(msg, sizeof msg, "%s %s: mismatch=%llu/%u active=[%s]%s",
        chip, what, (unsigned long long)res.mismatch, kSamples, act.c_str(),
        match ? "" : " (control: expect mismatch)");
    check(active && (match ? res.mismatch == 0 : res.mismatch > 0), msg);
}

// ---- OPLL 系 -----------------------------------------------------------
// メロディ ch0 (音色0 = 自作音色の持続音) とリズムの BD を同時に鳴らす
static std::vector<W> opllProgram() {
    return {
        {0, 0x00, 0x21}, {0, 0x01, 0x21}, {0, 0x02, 0x3F}, {0, 0x03, 0x00},
        {0, 0x04, 0xFF}, {0, 0x05, 0xF0}, {0, 0x06, 0x0F}, {0, 0x07, 0x0F},
        {0, 0x10, 0x00}, {0, 0x30, 0x00}, {0, 0x20, 0x1F},
        {0, 0x16, 0x20}, {0, 0x26, 0x05}, {0, 0x17, 0x50}, {0, 0x27, 0x05},
        {0, 0x18, 0xC0}, {0, 0x28, 0x01},
        {0, 0x36, 0x00}, {0, 0x37, 0x00}, {0, 0x38, 0x00},
        {0, 0x0E, 0x30},
    };
}

template<typename Ref>
static void testOpll(const char* name, ChipType type) {
    const auto prog = opllProgram();
    checkRun<Ref>(name, "default = melody + rhythm", type, prog, defaultGains(),
        [](const int32_t* d, float& l, float& r) {
            l = r = static_cast<float>(d[0] + d[1]) * kScale;
        }, {0, 1}, true);
    checkRun<Ref>(name, "OPLL_MELODY only", type, prog, only(ChipPart::OPLL_MELODY),
        pick(0, 0), {0, 1}, true);
    checkRun<Ref>(name, "OPLL_RHYTHM only", type, prog, only(ChipPart::OPLL_RHYTHM),
        pick(1, 1), {0, 1}, true);
    checkRun<Ref>(name, "OPLL_MELODY only vs rhythm", type, prog, only(ChipPart::OPLL_MELODY),
        pick(1, 1), {0, 1}, false);
}

// ---- OPL3 / OPL4 の FM ----------------------------------------------------
// ch0 を A/B (C0=0x30)、ch1 を C/D (C1=0xC0) に出す。音程と音色を変えてある
static std::vector<W> opl3FmProgram(uint8_t newBits) {
    return {
        {1, 0x05, newBits},
        {0, 0x20, 0x21}, {0, 0x40, 0x3F}, {0, 0x60, 0xFF}, {0, 0x80, 0x0F},
        {0, 0x23, 0x21}, {0, 0x43, 0x00}, {0, 0x63, 0xF0}, {0, 0x83, 0x0F},
        {0, 0xA0, 0x00}, {0, 0xC0, 0x30}, {0, 0xB0, 0x3E},
        {0, 0x21, 0x21}, {0, 0x41, 0x3F}, {0, 0x61, 0xFF}, {0, 0x81, 0x0F},
        {0, 0x24, 0x22}, {0, 0x44, 0x00}, {0, 0x64, 0xF0}, {0, 0x84, 0x0F},
        {0, 0xA1, 0x80}, {0, 0xC1, 0xC0}, {0, 0xB1, 0x2A},
    };
}

static void testOpl3() {
    const char* name = "OPL3";
    const auto prog = opl3FmProgram(0x01);
    const std::initializer_list<size_t> all = {0, 1, 2, 3};
    checkRun<ymfm::ymf262>(name, "default = A/B only", ChipType::OPL3, prog, defaultGains(),
        [](const int32_t* d, float& l, float& r) {
            l = static_cast<float>(d[0]) * kScale;
            r = static_cast<float>(d[1]) * kScale;
        }, all, true);
    checkRun<ymfm::ymf262>(name, "OPL3_AB only", ChipType::OPL3, prog, only(ChipPart::OPL3_AB),
        pick(0, 1), all, true);
    checkRun<ymfm::ymf262>(name, "OPL3_CD only", ChipType::OPL3, prog, only(ChipPart::OPL3_CD),
        pick(2, 3), all, true);
    checkRun<ymfm::ymf262>(name, "OPL3_CD only vs A/B", ChipType::OPL3, prog, only(ChipPart::OPL3_CD),
        pick(0, 1), all, false);
}

// ---- OPL4 ---------------------------------------------------------------
// 波形メモリに波形0のヘッダ (12バイト) と 16 サンプルの 8bit 矩形波を置く
// (keyoff_retrigger_test と同じ)。AR=15 / DR=0 / RR=15 / RC=15。
static std::vector<uint8_t> makeAwmMemory() {
    constexpr uint32_t kBase = 0x1000;
    constexpr uint32_t kLen  = 16;
    std::vector<uint8_t> mem(kBase + kLen, 0);
    const uint8_t header[12] = {
        0x00, (kBase >> 8) & 0xFF, kBase & 0xFF,
        0x00, 0x00,
        (0x10000 - kLen) >> 8, (0x10000 - kLen) & 0xFF,
        0x00, 0xF0, 0x00, 0xFF, 0x00,
    };
    std::copy(header, header + 12, mem.begin());
    for (uint32_t i = 0; i < kLen; ++i) mem[kBase + i] = (i < kLen / 2) ? 0x60 : 0xA0;
    return mem;
}

// FM は OPL3 と同じ (ch0 → DO2, ch1 → DO0)。AWM ch0 は 0x68 の bit4 を立てて
// DO1 に出す。NEW2=1 にしないと port2 への書き込みが無視される
static std::vector<W> opl4Program() {
    auto p = opl3FmProgram(0x03);
    const std::vector<W> awm = {
        {2, 0x20, 0x00}, {2, 0x38, 0x10}, {2, 0x08, 0x00}, {2, 0x50, 0x01},
        {2, 0x68, 0x90},
    };
    p.insert(p.end(), awm.begin(), awm.end());
    return p;
}

static void testOpl4() {
    const char* name = "OPL4";
    const auto prog = opl4Program();
    const auto mem  = makeAwmMemory();
    const std::initializer_list<size_t> all = {0, 1, 2, 3, 4, 5};
    // ymf278b の data[0..1]=DO0, [2..3]=DO1, [4..5]=DO2
    checkRun<ymfm::ymf278b>(name, "default = DO2 only", ChipType::OPL4, prog, defaultGains(),
        [](const int32_t* d, float& l, float& r) {
            l = static_cast<float>(d[4]) * kScale;
            r = static_cast<float>(d[5]) * kScale;
        }, all, true, &mem);
    checkRun<ymfm::ymf278b>(name, "OPL4_DO0 only", ChipType::OPL4, prog, only(ChipPart::OPL4_DO0),
        pick(0, 1), all, true, &mem);
    checkRun<ymfm::ymf278b>(name, "OPL4_DO1 only", ChipType::OPL4, prog, only(ChipPart::OPL4_DO1),
        pick(2, 3), all, true, &mem);
    checkRun<ymfm::ymf278b>(name, "OPL4_DO2 only", ChipType::OPL4, prog, only(ChipPart::OPL4_DO2),
        pick(4, 5), all, true, &mem);
    checkRun<ymfm::ymf278b>(name, "OPL4_DO1 only vs DO0", ChipType::OPL4, prog, only(ChipPart::OPL4_DO1),
        pick(0, 1), all, false, &mem);
}

// ---- 部位を持たないチップ -------------------------------------------------
// 部位のゲインを全部 0 にして、チップのゲインだけで鳴ることを見る
static PartGains chipOnly() {
    PartGains g = zeroGains();
    g.chip_l = 0.5f;
    g.chip_r = 0.25f;
    return g;
}

static void testChipGain() {
    // OPM: ch0 は M1 だけを鳴らす (CON=7, 他は TL=127)。L/R とも出す
    const std::vector<W> opm = {
        {0, 0x20, 0xC7}, {0, 0x28, 0x4A},
        {0, 0x40, 0x07}, {0, 0x60, 0x00}, {0, 0x80, 0x1F},
        {0, 0xA0, 0x00}, {0, 0xC0, 0x00}, {0, 0xE0, 0x0F},
        {0, 0x68, 0x7F}, {0, 0x70, 0x7F}, {0, 0x78, 0x7F},
        {0, 0x08, 0x08},
    };
    checkRun<ymfm::ym2151>("OPM", "chip gain only", ChipType::OPM, opm, chipOnly(),
        pick(0, 1), {0, 1}, true);

    // OPL: 出力は1本 (data[0])
    const std::vector<W> opl = {
        {0, 0x20, 0x21}, {0, 0x40, 0x3F}, {0, 0x60, 0xFF}, {0, 0x80, 0x0F},
        {0, 0x23, 0x21}, {0, 0x43, 0x00}, {0, 0x63, 0xF0}, {0, 0x83, 0x0F},
        {0, 0xA0, 0x00}, {0, 0xB0, 0x3E},
    };
    checkRun<ymfm::ym3526>("OPL", "chip gain only", ChipType::OPL, opl, chipOnly(),
        pick(0, 0), {0}, true);
}

// =========================================================
//  accept
// =========================================================
struct AcceptCase {
    const char*           name;
    ChipType              type;
    std::vector<ChipPart> parts;
};

static void testAccept() {
    const std::vector<ChipPart> opn  = {ChipPart::OPN_FM, ChipPart::OPN_SSG};
    const std::vector<ChipPart> opll = {ChipPart::OPLL_MELODY, ChipPart::OPLL_RHYTHM};
    const AcceptCase cases[] = {
        {"Y8950", ChipType::Y8950, {}},
        {"OPL",   ChipType::OPL,   {}},
        {"OPL2",  ChipType::OPL2,  {}},
        {"OPL3",  ChipType::OPL3,  {ChipPart::OPL3_AB, ChipPart::OPL3_CD}},
        {"OPL4",  ChipType::OPL4,  {ChipPart::OPL4_DO0, ChipPart::OPL4_DO1, ChipPart::OPL4_DO2}},
        {"OPN",   ChipType::OPN,   opn},
        {"OPNA",  ChipType::OPNA,  opn},
        {"OPNB",  ChipType::OPNB,  opn},
        {"OPNBB", ChipType::OPNBB, opn},
        {"OPN2",  ChipType::OPN2,  {}},
        {"OPM",   ChipType::OPM,   {}},
        {"OPLL",  ChipType::OPLL,  opll},
        {"OPLLP", ChipType::OPLLP, opll},
        {"OPLLX", ChipType::OPLLX, opll},
        {"OPZ",   ChipType::OPZ,   {}},
        {"VRC7",  ChipType::VRC7,  opll},
    };
    for (const AcceptCase& c : cases) {
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(c.type);
        std::string bad;
        // kChipPartCount 番 (範囲外) も拒否されること
        for (uint32_t p = 0; p <= kChipPartCount; ++p) {
            const ChipPart part = static_cast<ChipPart>(p);
            const bool want = std::find(c.parts.begin(), c.parts.end(), part) != c.parts.end();
            float l = -1.0f, r = -1.0f;
            const bool got = eng.getPartGain(id, part, l, r);
            const bool defOk = !want || (l == defaultPartGain(part) && r == defaultPartGain(part));
            const bool set = eng.setPartGain(id, part, 0.25f, 0.75f);
            float l2 = -1.0f, r2 = -1.0f;
            const bool got2 = eng.getPartGain(id, part, l2, r2);
            const bool rtOk = !want || (l2 == 0.25f && r2 == 0.75f);
            if (got != want || set != want || got2 != want || !defOk || !rtOk)
                bad += " " + std::to_string(p);
        }
        char msg[256];
        std::snprintf(msg, sizeof msg, "accept %s: %zu part(s), wrong at [%s ]",
            c.name, c.parts.size(), bad.c_str());
        check(bad.empty(), msg);
    }
}

// =========================================================
//  engine
// =========================================================
static float peakAfter(FmEngine& eng, uint32_t warm, uint32_t n) {
    std::vector<float> l(warm + n), r(warm + n);
    eng.generate(l.data(), r.data(), warm + n);
    float pk = 0.0f;
    for (uint32_t i = warm; i < warm + n; ++i)
        pk = std::fmax(pk, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
    return pk;
}

static void writeAll(FmEngine& eng, uint32_t id, const std::vector<W>& prog) {
    for (const W& w : prog) eng.write(id, w.reg, w.val, w.port);
}

static void testEngine() {
    char msg[256];

    // OPLL: チップのゲインは部位のゲインに掛かる
    {
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(ChipType::OPLL);
        writeAll(eng, id, opllProgram());
        const float pk = peakAfter(eng, 4800, 4800);
        eng.setGain(id, 0.0f);
        const float pk0 = peakAfter(eng, 480, 4800);
        eng.setGain(id, 1.0f);
        eng.setPartGain(id, ChipPart::OPLL_MELODY, 0.0f, 0.0f);
        eng.setPartGain(id, ChipPart::OPLL_RHYTHM, 0.0f, 0.0f);
        const float pkParts0 = peakAfter(eng, 480, 4800);
        std::snprintf(msg, sizeof msg, "engine OPLL: peak=%.5f, chip gain 0 -> %.7f, both parts 0 -> %.7f",
            pk, pk0, pkParts0);
        check(pk > 0.01f && pk0 == 0.0f && pkParts0 == 0.0f, msg);
    }

    // OPM: 部位を持たないチップにもチップのゲインが掛かる
    {
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(ChipType::OPM);
        const std::vector<W> opm = {
            {0, 0x20, 0xC7}, {0, 0x28, 0x4A},
            {0, 0x40, 0x07}, {0, 0x60, 0x00}, {0, 0x80, 0x1F},
            {0, 0xA0, 0x00}, {0, 0xC0, 0x00}, {0, 0xE0, 0x0F},
            {0, 0x68, 0x7F}, {0, 0x70, 0x7F}, {0, 0x78, 0x7F},
            {0, 0x08, 0x08},
        };
        writeAll(eng, id, opm);
        const float pk = peakAfter(eng, 4800, 4800);
        eng.setGain(id, 0.0f);
        const float pk0 = peakAfter(eng, 480, 4800);
        std::snprintf(msg, sizeof msg, "engine OPM: peak=%.5f, chip gain 0 -> %.7f", pk, pk0);
        check(pk > 0.01f && pk0 == 0.0f, msg);
    }

    // OPL3: C/D だけに出したチャンネルは、既定では聞こえず、OPL3_CD を上げると聞こえる
    {
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(ChipType::OPL3);
        auto prog = opl3FmProgram(0x01);
        prog.push_back({0, 0xB0, 0x1E});   // ch0 (A/B) を KEY OFF
        writeAll(eng, id, prog);
        const float pkDefault = peakAfter(eng, 9600, 4800);
        eng.setPartGain(id, ChipPart::OPL3_CD, 1.0f, 1.0f);
        const float pkCd = peakAfter(eng, 480, 4800);
        std::snprintf(msg, sizeof msg, "engine OPL3 C/D-only channel: default peak=%.7f, OPL3_CD=1 -> %.5f",
            pkDefault, pkCd);
        check(pkDefault == 0.0f && pkCd > 0.01f, msg);
    }

    // OPL4: FM の C/D と AWM の C/D は DO2 に出ず、それぞれ DO0 / DO1 から聞こえる。
    // route は ymfm の出力の並びを前提に期待値を作るので、端子の意味はここで音から見る
    {
        const auto mem = makeAwmMemory();
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(ChipType::OPL4);
        eng.setMemory(id, ymfm::ACCESS_PCM, mem.data(), static_cast<uint32_t>(mem.size()));
        auto prog = opl4Program();
        prog.push_back({0, 0xB0, 0x1E});   // FM ch0 (A/B → DO2) を KEY OFF
        writeAll(eng, id, prog);
        const float pkDefault = peakAfter(eng, 9600, 4800);
        eng.setPartGain(id, ChipPart::OPL4_DO0, 1.0f, 1.0f);
        eng.setPartGain(id, ChipPart::OPL4_DO2, 0.0f, 0.0f);
        const float pkDo0 = peakAfter(eng, 480, 4800);
        eng.setPartGain(id, ChipPart::OPL4_DO0, 0.0f, 0.0f);
        eng.setPartGain(id, ChipPart::OPL4_DO1, 1.0f, 1.0f);
        const float pkDo1 = peakAfter(eng, 480, 4800);
        std::snprintf(msg, sizeof msg,
            "engine OPL4 FM C/D + AWM C/D: default peak=%.7f, DO0 only -> %.5f, DO1 only -> %.5f",
            pkDefault, pkDo0, pkDo1);
        check(pkDefault == 0.0f && pkDo0 > 0.01f && pkDo1 > 0.01f, msg);
    }

    // OPLL: メロディだけ鳴らすとリズム側は無音、リズムだけ鳴らすとメロディ側は無音
    {
        const auto all = opllProgram();
        const std::vector<W> melody(all.begin(), all.begin() + 11);
        std::vector<W> rhythm(all.begin() + 11, all.end());

        auto peakWith = [](const std::vector<W>& prog, float gMelody, float gRhythm) {
            FmEngine eng(48000);
            const uint32_t id = eng.addChip(ChipType::OPLL);
            eng.setPartGain(id, ChipPart::OPLL_MELODY, gMelody, gMelody);
            eng.setPartGain(id, ChipPart::OPLL_RHYTHM, gRhythm, gRhythm);
            writeAll(eng, id, prog);
            return peakAfter(eng, 0, 4800);
        };
        const float mOnM = peakWith(melody, 1.0f, 0.0f), mOnR = peakWith(melody, 0.0f, 1.0f);
        const float rOnR = peakWith(rhythm, 0.0f, 1.0f), rOnM = peakWith(rhythm, 1.0f, 0.0f);
        std::snprintf(msg, sizeof msg,
            "engine OPLL melody-only: MELODY %.5f / RHYTHM %.7f; rhythm-only: RHYTHM %.5f / MELODY %.7f",
            mOnM, mOnR, rOnR, rOnM);
        check(mOnM > 0.01f && mOnR == 0.0f && rOnR > 0.01f && rOnM == 0.0f, msg);
    }
}

int main() {
    testOpll<ymfm::ym2413>("OPLL",  ChipType::OPLL);
    testOpll<ymfm::ymf281>("OPLLP", ChipType::OPLLP);
    testOpll<ymfm::ym2423>("OPLLX", ChipType::OPLLX);
    testOpll<ymfm::ds1001>("VRC7",  ChipType::VRC7);
    testOpl3();
    testOpl4();
    testChipGain();
    testAccept();
    testEngine();

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
