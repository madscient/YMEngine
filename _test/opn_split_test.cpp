// opn_split_test.cpp
// OPN 系の FM と SSG を別々のレートで生成する実装 (FmChip.h の detail::*Split)
// と、それを支える LinearResampler・部位ゲインの回帰テスト。
//
//   resampler : 呼び出しを細かく分けても、ソースを読み捨てず連続した列として
//               補間すること
//   equiv     : 派生クラスで FM と SSG を別々に clock した値が、上流の
//               generate() (OPN_FIDELITY_MAX) の値と全サンプルで一致すること。
//               prescale の切り替えを含む。書き込みを1つ抜くと不一致になる
//               ことも確かめ、比較が FM と SSG の両方に効いていることを示す
//   engine    : FmEngine 経由のネイティブレート、既定の SSG 音量、部位ゲイン
//
// 全件通れば終了コード 0。

#include "FmEngine.h"
#include "test_clocks.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_fail;
}

// =========================================================
//  resampler
// =========================================================
// 0,1,2,... を出すソースを src_num/src_den Hz → dst Hz に chunk ずつ変換し、
// (1) 生成したソース数が経過時間に見合うか、(2) 出力が直線に乗るかを見る。
// 初期状態で持ち越しているサンプル 0 がソースの -1 番目に当たるので、
// 出力 k の理想値は k × (src/dst) − 1。ただしソースの 0 番より手前
// (k × ratio < 1) はランプに乗らないので判定しない。
static void testResampler(uint64_t src_num, uint32_t src_den, uint32_t dst,
                          uint32_t chunk, uint32_t calls) {
    LinearResampler<1> rs;
    rs.setup(src_num, src_den, dst);
    const double ratio = double(src_num) / src_den / dst;
    uint64_t generated = 0;
    double   max_err   = 0.0;
    uint64_t k         = 0;
    std::vector<float> out(chunk);
    for (uint32_t c = 0; c < calls; ++c) {
        float* o[1] = { out.data() };
        rs.process([&](float* const* b, uint32_t n) {
            for (uint32_t i = 0; i < n; ++i) b[0][i] = float(generated++);
        }, o, chunk);
        for (uint32_t i = 0; i < chunk; ++i, ++k) {
            if (double(k) * ratio < 1.0) continue;
            max_err = std::fmax(max_err, std::fabs(out[i] - (double(k) * ratio - 1.0)));
        }
    }
    const double expected = double(calls) * chunk * ratio;
    char msg[256];
    std::snprintf(msg, sizeof msg,
        "resampler %.1fHz->%uHz chunk=%u: generated=%llu expected~%.1f",
        double(src_num) / src_den, dst, chunk, (unsigned long long)generated, expected);
    check(std::fabs(double(generated) - expected) <= 2.0, msg);
    std::snprintf(msg, sizeof msg,
        "resampler %.1fHz->%uHz chunk=%u: max deviation from line=%.4f",
        double(src_num) / src_den, dst, chunk, max_err);
    check(max_err < 0.05, msg);
}

// =========================================================
//  equiv
// =========================================================
struct Step { uint32_t seg; uint32_t port; uint8_t reg; uint8_t val; bool prescale; };

// FM は ch1 を使う (YM2610 は ch0/ch3 を持たない)。
static std::vector<Step> program() {
    std::vector<Step> p;
    auto w = [&](uint32_t seg, uint8_t reg, uint8_t val, bool ps = false) {
        p.push_back({seg, 0, reg, val, ps});
    };
    // SSG: A=トーン、B=トーン+ノイズ、C=トーン+エンベロープ
    w(0, 0x07, 0x28);
    w(0, 0x00, 0x1C); w(0, 0x01, 0x01);
    w(0, 0x02, 0x80); w(0, 0x03, 0x00);
    w(0, 0x04, 0x40); w(0, 0x05, 0x02);
    w(0, 0x06, 0x05);
    w(0, 0x08, 0x0F); w(0, 0x09, 0x0C); w(0, 0x0A, 0x10);
    w(0, 0x0B, 0x00); w(0, 0x0C, 0x02); w(0, 0x0D, 0x0E);
    // FM ch1: アルゴリズム7、4オペレータとも出力
    for (uint8_t op = 0; op < 4; ++op) {
        w(0, uint8_t(0x31 + op * 4), 0x01);
        w(0, uint8_t(0x41 + op * 4), 0x10);
        w(0, uint8_t(0x51 + op * 4), 0x1F);
        w(0, uint8_t(0x61 + op * 4), 0x05);
        w(0, uint8_t(0x71 + op * 4), 0x02);
        w(0, uint8_t(0x81 + op * 4), 0x2F);
    }
    w(0, 0xB1, 0x07); w(0, 0xB5, 0xC0);
    w(0, 0xA5, 0x22); w(0, 0xA1, 0x69);
    w(0, 0x28, 0xF1);
    // 鳴っている途中の変更
    w(3, 0xA5, 0x1A); w(3, 0xA1, 0x40);
    w(3, 0x00, 0x80); w(3, 0x0D, 0x0A);
    // prescale 2 → 6 → 3 (YM2610 は prescale を持たないので送らない)
    w(6, 0x2F, 0x00, true);
    w(8, 0x28, 0x01);
    w(10, 0x2D, 0x00, true);
    w(12, 0x2D, 0x00, true); w(12, 0x2E, 0x00, true);
    w(14, 0x28, 0xF1);
    return p;
}
constexpr uint32_t kSegments = 18;
// 18,9,6 (FM) と 4,2,1 (SSG) のどの繰り返し数でも割り切れる長さ。
// 区間の頭が繰り返しの頭に揃うので、区間ごとに比較できる。
constexpr uint32_t kSegLen = 36 * 40;

template<typename Stock, typename Split>
static void runEquiv(uint32_t stockDiv, bool hasPrescale, int skipIdx,
                     uint64_t& mm_fm, uint64_t& mm_ssg,
                     uint64_t& active_fm, uint64_t& active_ssg) {
    BasicYmfmInterface ia, ib;
    Stock st(ia);
    Split sp(ib);
    st.reset();
    sp.reset();
    mm_fm = mm_ssg = active_fm = active_ssg = 0;

    const auto prog = program();
    std::vector<typename Stock::output_data> buf(kSegLen);
    std::vector<int32_t> fl, fr, ss;
    for (uint32_t seg = 0; seg < kSegments; ++seg) {
        for (size_t i = 0; i < prog.size(); ++i) {
            const Step& s = prog[i];
            if (s.seg != seg || (s.prescale && !hasPrescale)) continue;
            st.write(s.port * 2, s.reg);
            st.write(s.port * 2 + 1, s.val);
            if (int(i) == skipIdx) continue;
            sp.write(s.port * 2, s.reg);
            sp.write(s.port * 2 + 1, s.val);
        }
        // 抜いた書き込みが prescale だと両者の繰り返し数がずれるが、
        // 抜くのは prescale 以外に限っているので、どちらから取っても同じ
        const uint32_t holdFm  = sp.fmDivider()  / stockDiv;
        const uint32_t holdSsg = sp.ssgDivider() / stockDiv;

        st.generate(buf.data(), kSegLen);
        fl.resize(kSegLen / holdFm);
        fr.resize(kSegLen / holdFm);
        ss.resize(kSegLen / holdSsg);
        for (size_t i = 0; i < fl.size(); ++i) sp.clockFm(fl[i], fr[i]);
        for (size_t i = 0; i < ss.size(); ++i) ss[i] = sp.clockSsg();

        constexpr uint32_t F = Stock::FM_OUTPUTS;
        for (uint32_t k = 0; k < kSegLen; ++k) {
            const auto& d = buf[k].data;
            const int32_t stL = d[0];
            const int32_t stR = (F >= 2) ? d[1] : d[0];
            int32_t stS = 0;
            for (uint32_t j = 0; j < Stock::SSG_OUTPUTS; ++j) stS += d[F + j];
            if (stL != fl[k / holdFm] || stR != fr[k / holdFm]) ++mm_fm;
            if (stS != ss[k / holdSsg]) ++mm_ssg;
            if (stL != 0 || stR != 0) ++active_fm;
            if (stS != 0) ++active_ssg;
        }
    }
}

static int findStep(uint32_t seg, uint8_t reg) {
    const auto prog = program();
    for (size_t i = 0; i < prog.size(); ++i)
        if (prog[i].seg == seg && prog[i].reg == reg) return int(i);
    return -1;
}

template<typename Stock, typename Split>
static void testEquiv(const char* name, uint32_t stockDiv, bool hasPrescale) {
    uint64_t mf, ms, af, as;
    char msg[256];

    runEquiv<Stock, Split>(stockDiv, hasPrescale, -1, mf, ms, af, as);
    std::snprintf(msg, sizeof msg, "equiv %s: mismatch FM=%llu SSG=%llu (active FM=%llu SSG=%llu)",
        name, (unsigned long long)mf, (unsigned long long)ms,
        (unsigned long long)af, (unsigned long long)as);
    check(mf == 0 && ms == 0 && af > 0 && as > 0, msg);

    // 対照: KEY ON を抜けば FM が、SSG の音量を抜けば SSG が食い違うはず
    runEquiv<Stock, Split>(stockDiv, hasPrescale, findStep(0, 0x28), mf, ms, af, as);
    std::snprintf(msg, sizeof msg, "equiv %s control (skip KEY ON): mismatch FM=%llu", name,
        (unsigned long long)mf);
    check(mf > 0, msg);
    runEquiv<Stock, Split>(stockDiv, hasPrescale, findStep(0, 0x08), mf, ms, af, as);
    std::snprintf(msg, sizeof msg, "equiv %s control (skip SSG A volume): mismatch SSG=%llu", name,
        (unsigned long long)ms);
    check(ms > 0, msg);
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

static float softClip(float x) { return x * (1.0f - (x * x) / 9.0f); }

static void ssgToneA(FmEngine& eng, uint32_t id) {
    eng.write(id, 0x07, 0x3E);             // トーン A だけ
    eng.write(id, 0x00, 0x00); eng.write(id, 0x01, 0x01);
    eng.write(id, 0x08, 0x0F);             // 最大音量
}

static void fmToneCh1(FmEngine& eng, uint32_t id) {
    for (uint8_t op = 0; op < 4; ++op) {
        eng.write(id, uint8_t(0x31 + op * 4), 0x01);
        eng.write(id, uint8_t(0x41 + op * 4), 0x10);
        eng.write(id, uint8_t(0x51 + op * 4), 0x1F);
        eng.write(id, uint8_t(0x81 + op * 4), 0x0F);
    }
    eng.write(id, 0xB1, 0x07); eng.write(id, 0xB5, 0xC0);
    eng.write(id, 0xA5, 0x22); eng.write(id, 0xA1, 0x69);
    eng.write(id, 0x28, 0xF1);
}

static void testEngine() {
    constexpr uint32_t kRate = 48000;
    char msg[256];

    {
        FmEngine eng(kRate);
        const uint32_t opn  = eng.addChip(ChipType::OPN, testClock(ChipType::OPN));
        const uint32_t opna = eng.addChip(ChipType::OPNA, testClock(ChipType::OPNA));
        const uint32_t opnb = eng.addChip(ChipType::OPNB, testClock(ChipType::OPNB));
        std::snprintf(msg, sizeof msg, "nativeRate OPN=%u OPNA=%u OPNB=%u (expect 55466/55466/55555)",
            eng.nativeRate(opn), eng.nativeRate(opna), eng.nativeRate(opnb));
        check(eng.nativeRate(opn) == 55466 && eng.nativeRate(opna) == 55466 &&
              eng.nativeRate(opnb) == 55555, msg);
        eng.write(opna, 0x2F, 0x00);
        peakAfter(eng, 0, 16);
        std::snprintf(msg, sizeof msg, "nativeRate OPNA after prescale 2 = %u (expect 166400)",
            eng.nativeRate(opna));
        check(eng.nativeRate(opna) == 166400, msg);
    }

    // 既定の SSG 音量: OPNA は3チャンネル和 × 2/3、OPN はそのまま (上流の generate() と同じ)
    {
        FmEngine eng(kRate);
        const uint32_t id = eng.addChip(ChipType::OPNA, testClock(ChipType::OPNA));
        ssgToneA(eng, id);
        const float pk = peakAfter(eng, 4800, 9600);
        const float expect = softClip((16382 * 2 / 3) / 32768.0f);
        std::snprintf(msg, sizeof msg, "OPNA SSG default level peak=%.5f (expect %.5f)", pk, expect);
        check(std::fabs(pk - expect) < 1e-3f, msg);

        eng.setPartGain(id, ChipPart::OPN_FM, 0.0f, 0.0f);
        const float pkFm0 = peakAfter(eng, 480, 9600);
        std::snprintf(msg, sizeof msg, "OPNA SSG only, FM gain 0: peak=%.5f (expect %.5f)", pkFm0, expect);
        check(std::fabs(pkFm0 - expect) < 1e-3f, msg);

        eng.setPartGain(id, ChipPart::OPN_FM, 1.0f, 1.0f);
        eng.setGain(id, 0.5f);
        eng.setPartGain(id, ChipPart::OPN_SSG, 0.5f, 0.5f);
        const float pkHalf = peakAfter(eng, 480, 9600);
        const float expectHalf = softClip((16382 * 2 / 3) / 32768.0f * 0.25f);
        std::snprintf(msg, sizeof msg, "OPNA SSG chip gain 0.5 x part gain 0.5: peak=%.5f (expect %.5f)",
            pkHalf, expectHalf);
        check(std::fabs(pkHalf - expectHalf) < 1e-3f, msg);

        eng.setPartGain(id, ChipPart::OPN_SSG, 0.0f, 0.0f);
        const float pkSsg0 = peakAfter(eng, 480, 9600);
        std::snprintf(msg, sizeof msg, "OPNA SSG only, SSG gain 0: peak=%.7f", pkSsg0);
        check(pkSsg0 == 0.0f, msg);
    }
    {
        FmEngine eng(kRate);
        const uint32_t id = eng.addChip(ChipType::OPN, testClock(ChipType::OPN));
        ssgToneA(eng, id);
        const float pk = peakAfter(eng, 4800, 9600);
        const float expect = softClip(16382 / 32768.0f);
        std::snprintf(msg, sizeof msg, "OPN SSG default level peak=%.5f (expect %.5f)", pk, expect);
        check(std::fabs(pk - expect) < 1e-3f, msg);
    }

    // FM だけ鳴らす
    {
        FmEngine eng(kRate);
        const uint32_t id = eng.addChip(ChipType::OPNA, testClock(ChipType::OPNA));
        fmToneCh1(eng, id);
        eng.setPartGain(id, ChipPart::OPN_SSG, 0.0f, 0.0f);
        const float pk = peakAfter(eng, 4800, 9600);
        std::snprintf(msg, sizeof msg, "OPNA FM only, SSG gain 0: peak=%.5f (> 0.01)", pk);
        check(pk > 0.01f, msg);
        eng.setPartGain(id, ChipPart::OPN_FM, 0.0f, 0.0f);
        const float pk0 = peakAfter(eng, 480, 9600);
        std::snprintf(msg, sizeof msg, "OPNA FM only, FM gain 0: peak=%.7f", pk0);
        check(pk0 == 0.0f, msg);
    }

    // どのチップがどの部位を受け付けるかは part_gain_test で見る
    {
        FmEngine eng(kRate);
        const uint32_t id = eng.addChip(ChipType::OPNA, testClock(ChipType::OPNA));
        float l = -1.0f, r = -1.0f;
        const bool set = eng.setPartGain(id, ChipPart::OPN_SSG, 0.25f, 0.75f);
        const bool got = eng.getPartGain(id, ChipPart::OPN_SSG, l, r);
        check(set && got && l == 0.25f && r == 0.75f, "OPNA SSG part gain round-trips");
        check(!eng.setPartGain(id + 1, ChipPart::OPN_FM, 1.0f, 1.0f), "unknown chip_id rejected");
    }
}

int main() {
    testResampler(55466, 1, 48000, 240, 200);
    testResampler(55466, 1, 48000, 1, 48000);
    testResampler(7'987'200, 144, 48000, 240, 200);
    testResampler(7'987'200, 32, 44100, 7, 5000);
    testResampler(44100, 1, 48000, 240, 200);   // アップサンプリング

    testEquiv<ymfm::ym2203, detail::Ym2203Split>("OPN (YM2203)", 4, true);
    testEquiv<ymfm::ym2608, detail::Ym2608Split>("OPNA (YM2608)", 8, true);
    testEquiv<ymfm::ym2610, detail::Ym2610Split<ymfm::ym2610>>("OPNB (YM2610)", 16, false);
    testEquiv<ymfm::ym2610b, detail::Ym2610Split<ymfm::ym2610b>>("OPNBB (YM2610B)", 16, false);

    testEngine();

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
