// c_api_test.cpp
// C API (FmEngineApi.h) の回帰テスト。ビルドした DLL を実行時にロードして、
// アプリケーションと同じ経路で呼ぶ。エンジンの C++ ヘッダは include しない。
//
//   symbols: ヘッダが宣言する関数がすべてエクスポートされていること。部位と
//            外部メモリを番号で指定する形の関数が残っていないこと
//   chips  : 対応チップの一覧が、このテストの表と一致すること
//   part   : 全チップで、列挙した部位の名前と FmEngine_GetPartGain の既定値が
//            FmEngineApi の仕様の表と一致すること。名前で設定した値を読み戻せる
//            こと。チップが持たない名前、NULL、未知の chip_id、NULL のハンドルを
//            拒否すること
//   memory : 全チップで、列挙した外部メモリの名前が仕様の表と一致すること。
//            列挙した名前がどれも FmEngine_SetMemory と FmEngine_SetMemoryEx に
//            渡せること。チップが持たない名前、NULL、範囲の検査、取り外し
//   sound  : 名前で指定したゲインとメモリが、その名前の出力とメモリに効くこと
//
// 使い方: c_api_test <DLL のパス>
// 全件通れば終了コード 0。
//
// クロックに依る期待値を持たないので、全チップに同じクロックを渡す。

#include "FmEngineApi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

static int  g_fail = 0;
static char msg[512];

static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_fail;
}

// =========================================================
//  DLL のロード
// =========================================================
#ifdef _WIN32
static void* openLibrary(const char* path) { return LoadLibraryA(path); }
static void* findSymbol(void* lib, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
}
#else
static void* openLibrary(const char* path) { return dlopen(path, RTLD_NOW); }
static void* findSymbol(void* lib, const char* name) { return dlsym(lib, name); }
#endif

// ヘッダの宣言は型を取るためだけに使う。直接呼ぶとインポートライブラリが要る
#define FM_API_FUNCTIONS(X) \
    X(Create) X(Destroy) X(Inquiry) X(GetSupportedChip) X(AddChip) X(GetChipName) \
    X(GetNativeRate) X(GetSampleRate) X(Write) X(SetGain) X(GetGain) \
    X(GetPartCount) X(GetPartName) X(SetPartGain) X(GetPartGain) \
    X(GetMemoryCount) X(GetMemoryName) X(SetMemory) X(SetMemoryEx) X(Generate)

struct Api {
#define X(name) decltype(&FmEngine_##name) name = nullptr;
    FM_API_FUNCTIONS(X)
#undef X
};

static bool testSymbols(void* lib, Api& api) {
    std::string missing;
    uint32_t count = 0;
#define X(name) \
    api.name = reinterpret_cast<decltype(api.name)>(findSymbol(lib, "FmEngine_" #name)); \
    ++count; \
    if (!api.name) missing += " FmEngine_" #name;
    FM_API_FUNCTIONS(X)
#undef X
    std::snprintf(msg, sizeof msg, "symbols: %u declared, missing [%s ]", count, missing.c_str());
    check(missing.empty(), msg);

    std::string stale;
    for (const char* name : {"FmEngine_GetPartMask", "FmEngine_GetMemorySize"})
        if (findSymbol(lib, name)) stale += std::string(" ") + name;
    std::snprintf(msg, sizeof msg, "symbols: no export left from the numbered API [%s ]", stale.c_str());
    check(stale.empty(), msg);
    return missing.empty();
}

// =========================================================
//  FmEngineApi の仕様の表
// =========================================================
static constexpr uint32_t kRate  = 48000;
static constexpr uint32_t kClock = 8'000'000;

struct PartSpec {
    const char* name;
    float       gain;   // 既定値
};

struct ChipSpec {
    const char*              chip;
    std::vector<PartSpec>    parts;
    std::vector<const char*> memories;
};

static const std::vector<ChipSpec>& chipSpecs() {
    static const std::vector<PartSpec> opn  = {{"FM", 1.0f}, {"SSG", 1.0f}};
    static const std::vector<PartSpec> opll = {{"MELODY", 1.0f}, {"RHYTHM", 1.0f}};
    static const std::vector<ChipSpec> specs = {
        {"Y8950", {},   {"ADPCM_B", "ADPCM_B_ROMMODE"}},
        {"OPL",   {},   {}},
        {"OPL2",  {},   {}},
        {"OPL3",  {{"AB", 1.0f}, {"CD", 0.0f}}, {}},
        {"OPL4",  {{"DO0", 0.0f}, {"DO1", 0.0f}, {"DO2", 1.0f}}, {"PCM"}},
        {"OPN",   opn,  {}},
        {"OPNA",  opn,  {"RHYTHM", "ADPCM_B", "ADPCM_B_ROMMODE"}},
        {"OPNB",  opn,  {"ADPCM_A", "ADPCM_B"}},
        {"OPNBB", opn,  {"ADPCM_A", "ADPCM_B"}},
        {"OPN2",  {},   {}},
        {"OPM",   {},   {}},
        {"OPLL",  opll, {}},
        {"OPLLP", opll, {}},
        {"OPLLX", opll, {}},
        {"OPZ",   {},   {}},
        {"VRC7",  opll, {}},
    };
    return specs;
}

// 表に出てくる名前のすべてと、どのチップのものでもない名前。チップが持たない
// 名前を拒否することを、これを総当たりして見る
static std::vector<std::string> everyName() {
    std::vector<std::string> names = {"", "fm", "Ssg", "OPN_FM", "FM ", "pcm", "FM_MEM_PCM", "ROM"};
    for (const ChipSpec& c : chipSpecs()) {
        for (const PartSpec& p : c.parts) names.push_back(p.name);
        for (const char* m : c.memories)  names.push_back(m);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

static bool hasPart(const ChipSpec& c, const std::string& name) {
    return std::any_of(c.parts.begin(), c.parts.end(),
                       [&](const PartSpec& p) { return name == p.name; });
}

static bool hasMemory(const ChipSpec& c, const std::string& name) {
    return std::any_of(c.memories.begin(), c.memories.end(),
                       [&](const char* m) { return name == m; });
}

// count / name の関数で列挙した名前を返す。同じ index をもう一度引いて名前が
// 変わる、名前が重なる、数の次の index が NULL でないなら ok を false にする
template<typename CountFn, typename NameFn>
static std::vector<std::string> enumerate(CountFn count, NameFn name, bool& ok) {
    std::vector<std::string> names;
    const uint32_t n = count();
    for (uint32_t i = 0; i < n; ++i) {
        const char* s = name(i);
        const char* again = name(i);
        if (!s || !again || std::strcmp(s, again) != 0) { ok = false; names.push_back("(null)"); continue; }
        if (std::find(names.begin(), names.end(), s) != names.end()) ok = false;
        names.push_back(s);
    }
    if (name(n) != nullptr) ok = false;
    return names;
}

static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& e : v) s += (s.empty() ? "" : ",") + e;
    return s;
}

// =========================================================
//  chips
// =========================================================
static void testChips(const Api& api) {
    FmEngineHandle eng = api.Create(kRate);
    std::vector<std::string> got, want;
    for (uint32_t i = 0; i < api.Inquiry(eng); ++i) got.push_back(api.GetSupportedChip(eng, i));
    for (const ChipSpec& c : chipSpecs()) want.push_back(c.chip);
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    std::snprintf(msg, sizeof msg, "chips: engine supports [%s]", join(got).c_str());
    check(got == want, msg);
    api.Destroy(eng);
}

// =========================================================
//  part
// =========================================================
static void testPart(const Api& api) {
    const auto names = everyName();
    for (const ChipSpec& c : chipSpecs()) {
        FmEngineHandle eng = api.Create(kRate);
        uint32_t id = UINT32_MAX;
        if (api.AddChip(eng, c.chip, kClock, &id) != FM_OK) {
            std::snprintf(msg, sizeof msg, "part %s: AddChip failed", c.chip);
            check(false, msg);
            api.Destroy(eng);
            continue;
        }

        bool listOk = true;
        auto listed = enumerate([&] { return api.GetPartCount(eng, id); },
                                [&](uint32_t i) { return api.GetPartName(eng, id, i); }, listOk);
        std::snprintf(msg, sizeof msg, "part %-5s names [%s]", c.chip, join(listed).c_str());
        std::sort(listed.begin(), listed.end());
        std::vector<std::string> want;
        for (const PartSpec& p : c.parts) want.push_back(p.name);
        std::sort(want.begin(), want.end());
        check(listOk && listed == want, msg);

        std::string bad;
        for (const PartSpec& p : c.parts) {
            float l = -1.0f, r = -1.0f;
            if (api.GetPartGain(eng, id, p.name, &l, &r) != FM_OK || l != p.gain || r != p.gain)
                bad += std::string(" default:") + p.name;
            if (api.SetPartGain(eng, id, p.name, 0.25f, 0.75f) != FM_OK)
                bad += std::string(" set:") + p.name;
        }
        for (const std::string& n : names) {
            if (hasPart(c, n)) continue;
            float l = -1.0f, r = -1.0f;
            if (api.SetPartGain(eng, id, n.c_str(), 0.5f, 0.5f) != FM_ERR_INVALID_ARG ||
                api.GetPartGain(eng, id, n.c_str(), &l, &r) != FM_ERR_INVALID_ARG ||
                l != -1.0f || r != -1.0f)
                bad += " accepted:'" + n + "'";
        }
        float l = -1.0f, r = -1.0f;
        if (api.SetPartGain(eng, id, nullptr, 0.5f, 0.5f) != FM_ERR_INVALID_ARG ||
            api.GetPartGain(eng, id, nullptr, &l, &r) != FM_ERR_INVALID_ARG)
            bad += " accepted:NULL";
        // 拒否した呼び出しが、ほかの部位の値を変えていないこと
        for (const PartSpec& p : c.parts) {
            if (api.GetPartGain(eng, id, p.name, &l, &r) != FM_OK || l != 0.25f || r != 0.75f)
                bad += std::string(" readback:") + p.name;
            if (api.GetPartGain(eng, id, p.name, nullptr, &r) != FM_ERR_INVALID_ARG ||
                api.GetPartGain(eng, id, p.name, &l, nullptr) != FM_ERR_INVALID_ARG)
                bad += std::string(" null-out:") + p.name;
        }
        std::snprintf(msg, sizeof msg, "part %-5s defaults, round trip, rejects: wrong at [%s ]",
                      c.chip, bad.c_str());
        check(bad.empty(), msg);
        api.Destroy(eng);
    }

    FmEngineHandle eng = api.Create(kRate);
    uint32_t id = UINT32_MAX;
    api.AddChip(eng, "OPNA", kClock, &id);
    float l = -1.0f, r = -1.0f;
    check(api.GetPartCount(eng, id + 1) == 0 && api.GetPartName(eng, id + 1, 0) == nullptr &&
          api.SetPartGain(eng, id + 1, "FM", 1.0f, 1.0f) == FM_ERR_INVALID_ARG &&
          api.GetPartGain(eng, id + 1, "FM", &l, &r) == FM_ERR_INVALID_ARG,
          "part: unknown chip_id has no parts");
    check(api.GetPartCount(nullptr, id) == 0 && api.GetPartName(nullptr, id, 0) == nullptr &&
          api.SetPartGain(nullptr, id, "FM", 1.0f, 1.0f) == FM_ERR_INVALID_ARG &&
          api.GetPartGain(nullptr, id, "FM", &l, &r) == FM_ERR_INVALID_ARG,
          "part: NULL handle is rejected");
    api.Destroy(eng);
}

// =========================================================
//  memory
// =========================================================
static void testMemory(const Api& api) {
    const auto names = everyName();
    std::vector<uint8_t> a(32), b(32);
    for (const ChipSpec& c : chipSpecs()) {
        FmEngineHandle eng = api.Create(kRate);
        uint32_t id = UINT32_MAX;
        if (api.AddChip(eng, c.chip, kClock, &id) != FM_OK) {
            std::snprintf(msg, sizeof msg, "memory %s: AddChip failed", c.chip);
            check(false, msg);
            api.Destroy(eng);
            continue;
        }

        bool listOk = true;
        auto listed = enumerate([&] { return api.GetMemoryCount(eng, id); },
                                [&](uint32_t i) { return api.GetMemoryName(eng, id, i); }, listOk);
        std::snprintf(msg, sizeof msg, "memory %-5s names [%s]", c.chip, join(listed).c_str());
        std::sort(listed.begin(), listed.end());
        std::vector<std::string> want(c.memories.begin(), c.memories.end());
        std::sort(want.begin(), want.end());
        check(listOk && listed == want, msg);

        std::string bad;
        auto expect = [&](FmResult got, FmResult wanted, const char* name, const char* what) {
            if (got != wanted) bad += std::string(" ") + name + ":" + what;
        };
        const auto ROM = FM_ACCESS_ROM;
        const auto RAM = FM_ACCESS_RAM;
        for (const char* m : c.memories) {
            // FmEngine_SetMemory は [0, size) に割り当てる
            expect(api.SetMemory(eng, id, m, a.data(), 16), FM_OK, m, "set");
            expect(api.SetMemoryEx(eng, id, m, 8, b.data(), 16, ROM), FM_ERR_INVALID_ARG, m, "overlap");
            expect(api.SetMemoryEx(eng, id, m, 16, b.data(), 16, RAM), FM_OK, m, "adjacent");
            expect(api.SetMemoryEx(eng, id, m, 0, nullptr, 32, ROM), FM_OK, m, "unmap");
            expect(api.SetMemoryEx(eng, id, m, 8, b.data(), 16, ROM), FM_OK, m, "map-after-unmap");
            // 取り外しでは access を見ない
            expect(api.SetMemoryEx(eng, id, m, 8, nullptr, 16, static_cast<FmMemoryAccess>(2)),
                   FM_OK, m, "unmap-ignores-access");
            expect(api.SetMemoryEx(eng, id, m, 8, a.data(), 16, RAM), FM_OK, m, "map-after-second-unmap");
            expect(api.SetMemoryEx(eng, id, m, 0x100, b.data(), 0, ROM), FM_ERR_INVALID_ARG, m, "size0");
            expect(api.SetMemoryEx(eng, id, m, 0xFFFFFFF0u, b.data(), 0x11, ROM), FM_ERR_INVALID_ARG, m, "over-2^32");
            expect(api.SetMemoryEx(eng, id, m, 0x100, b.data(), 16, static_cast<FmMemoryAccess>(2)),
                   FM_ERR_INVALID_ARG, m, "access");
            expect(api.SetMemory(eng, id, m, nullptr, 16), FM_ERR_INVALID_ARG, m, "set-null");
            expect(api.SetMemory(eng, id, m, a.data(), 0), FM_ERR_INVALID_ARG, m, "set-size0");
        }
        for (const std::string& n : names) {
            if (hasMemory(c, n)) continue;
            const std::string shown = "'" + n + "'";
            expect(api.SetMemory(eng, id, n.c_str(), a.data(), 16), FM_ERR_INVALID_ARG, shown.c_str(), "set");
            expect(api.SetMemoryEx(eng, id, n.c_str(), 0, b.data(), 16, ROM), FM_ERR_INVALID_ARG, shown.c_str(), "map");
            expect(api.SetMemoryEx(eng, id, n.c_str(), 0, nullptr, 16, ROM), FM_ERR_INVALID_ARG, shown.c_str(), "unmap");
        }
        expect(api.SetMemory(eng, id, nullptr, a.data(), 16), FM_ERR_INVALID_ARG, "NULL", "set");
        expect(api.SetMemoryEx(eng, id, nullptr, 0, b.data(), 16, ROM), FM_ERR_INVALID_ARG, "NULL", "map");
        std::snprintf(msg, sizeof msg, "memory %-5s set, map, unmap, rejects: wrong at [%s ]",
                      c.chip, bad.c_str());
        check(bad.empty(), msg);
        api.Destroy(eng);
    }

    FmEngineHandle eng = api.Create(kRate);
    uint32_t id = UINT32_MAX;
    api.AddChip(eng, "OPNA", kClock, &id);
    check(api.GetMemoryCount(eng, id + 1) == 0 && api.GetMemoryName(eng, id + 1, 0) == nullptr &&
          api.SetMemory(eng, id + 1, "RHYTHM", a.data(), 16) == FM_ERR_INVALID_ARG &&
          api.SetMemoryEx(eng, id + 1, "RHYTHM", 0, b.data(), 16, FM_ACCESS_ROM) == FM_ERR_INVALID_ARG,
          "memory: unknown chip_id has no memories");
    check(api.GetMemoryCount(nullptr, id) == 0 && api.GetMemoryName(nullptr, id, 0) == nullptr &&
          api.SetMemory(nullptr, id, "RHYTHM", a.data(), 16) == FM_ERR_INVALID_ARG &&
          api.SetMemoryEx(nullptr, id, "RHYTHM", 0, b.data(), 16, FM_ACCESS_ROM) == FM_ERR_INVALID_ARG,
          "memory: NULL handle is rejected");
    api.Destroy(eng);
}

// =========================================================
//  sound
// =========================================================
struct W { uint32_t port; uint8_t reg; uint8_t val; };

static void writeAll(const Api& api, FmEngineHandle eng, uint32_t id, const std::vector<W>& prog) {
    for (const W& w : prog) api.Write(eng, id, w.reg, w.val, w.port);
}

static constexpr uint32_t kSamples = 4800;

struct Peaks { float l, r; };

static Peaks peaksAfter(const Api& api, FmEngineHandle eng, uint32_t warm) {
    std::vector<float> l(warm + kSamples), r(warm + kSamples);
    api.Generate(eng, l.data(), r.data(), warm + kSamples);
    Peaks p{0.0f, 0.0f};
    for (uint32_t i = warm; i < warm + kSamples; ++i) {
        p.l = std::fmax(p.l, std::fabs(l[i]));
        p.r = std::fmax(p.r, std::fabs(r[i]));
    }
    return p;
}

// SSG のトーン A を最大音量で鳴らす
static const std::vector<W> kSsgTone = {
    {0, 0x07, 0x3E}, {0, 0x00, 0x00}, {0, 0x01, 0x01}, {0, 0x08, 0x0F},
};

// OPNA のリズム 6 音を同時に鳴らす
static const std::vector<W> kOpnaRhythm = {
    {0, 0x11, 0x3F}, {0, 0x10, 0x3F},
};

// OPNA の ADPCM-B を ROM モードで、番地 0 から 8KB 繰り返し再生する
static const std::vector<W> kOpnaAdpcmBRomMode = {
    {1, 0x00, 0x01}, {1, 0x00, 0x00},
    {1, 0x01, 0xC1},
    {1, 0x02, 0x00}, {1, 0x03, 0x00},
    {1, 0x04, 0xFF}, {1, 0x05, 0x00},
    {1, 0x09, 0xFF}, {1, 0x0A, 0xFF},
    {1, 0x0B, 0xFF},
    {1, 0x00, 0xB0},
};

// OPNA を1個置き、memory に 0x77 の 8KB を FmEngine_SetMemory で渡して (nullptr なら
// 渡さずに) prog を鳴らす
static std::vector<float> playOpna(const Api& api, const std::vector<W>& prog, const char* memory,
                                   const std::vector<uint8_t>& mem) {
    FmEngineHandle eng = api.Create(kRate);
    uint32_t id = UINT32_MAX;
    api.AddChip(eng, "OPNA", kClock, &id);
    if (memory && api.SetMemory(eng, id, memory, mem.data(), static_cast<uint32_t>(mem.size())) != FM_OK)
        check(false, "sound: SetMemory rejected");
    writeAll(api, eng, id, prog);
    std::vector<float> l(kSamples), r(kSamples);
    api.Generate(eng, l.data(), r.data(), kSamples);
    api.Destroy(eng);
    return l;
}

static size_t mismatches(const std::vector<float>& a, const std::vector<float>& b) {
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i) n += (a[i] != b[i]);
    return n;
}

static void testSound(const Api& api) {
    // 部位の名前がその出力を指すこと。L と R を取り違えても分かるよう値を変える
    {
        FmEngineHandle eng = api.Create(kRate);
        uint32_t id = UINT32_MAX;
        api.AddChip(eng, "OPNA", kClock, &id);
        writeAll(api, eng, id, kSsgTone);
        const Peaks def = peaksAfter(api, eng, 4800);
        api.SetPartGain(eng, id, "FM", 0.0f, 0.0f);
        const Peaks fm0 = peaksAfter(api, eng, 480);
        api.SetPartGain(eng, id, "SSG", 0.5f, 0.0f);
        const Peaks half = peaksAfter(api, eng, 480);
        api.SetPartGain(eng, id, "SSG", 0.0f, 0.0f);
        const Peaks ssg0 = peaksAfter(api, eng, 480);
        std::snprintf(msg, sizeof msg,
            "sound OPNA SSG tone: default %.4f/%.4f, FM=0 %.4f/%.4f, SSG=0.5/0 %.4f/%.4f, SSG=0 %.4f/%.4f",
            def.l, def.r, fm0.l, fm0.r, half.l, half.r, ssg0.l, ssg0.r);
        check(def.l > 0.1f && def.r > 0.1f &&
              std::fabs(fm0.l - def.l) < 1e-3f && std::fabs(fm0.r - def.r) < 1e-3f &&
              half.l > 0.05f && half.l < def.l * 0.75f && half.r == 0.0f &&
              ssg0.l == 0.0f && ssg0.r == 0.0f, msg);
        api.Destroy(eng);
    }

    // 外部メモリの名前がそのメモリを指すこと。何も渡さない場合の出力と比べ、
    // 読まれるメモリに渡すと変わり、読まれないメモリに渡しても変わらない
    {
        const std::vector<uint8_t> mem(0x2000, 0x77);
        const auto base = playOpna(api, kOpnaRhythm, nullptr, mem);
        const size_t dRhythm = mismatches(playOpna(api, kOpnaRhythm, "RHYTHM", mem), base);
        const size_t dOther  = mismatches(playOpna(api, kOpnaRhythm, "ADPCM_B", mem), base);
        std::snprintf(msg, sizeof msg,
            "sound OPNA rhythm: RHYTHM changes %zu samples / ADPCM_B changes %zu", dRhythm, dOther);
        check(dRhythm > kSamples / 10 && dOther == 0, msg);

        const auto baseB = playOpna(api, kOpnaAdpcmBRomMode, nullptr, mem);
        const size_t dRom = mismatches(playOpna(api, kOpnaAdpcmBRomMode, "ADPCM_B_ROMMODE", mem), baseB);
        const size_t dRam = mismatches(playOpna(api, kOpnaAdpcmBRomMode, "ADPCM_B", mem), baseB);
        std::snprintf(msg, sizeof msg,
            "sound OPNA ADPCM-B in ROM mode: ADPCM_B_ROMMODE changes %zu samples / ADPCM_B changes %zu",
            dRom, dRam);
        check(dRom > kSamples / 10 && dRam == 0, msg);
    }

    // FmEngine_SetMemoryEx の access がそのまま効くこと。Y8950 の録音モードで番地 0 から
    // 書き、FM_ACCESS_RAM のブロックには入り、FM_ACCESS_ROM のブロックには入らない
    for (const FmMemoryAccess access : {FM_ACCESS_RAM, FM_ACCESS_ROM}) {
        std::vector<uint8_t> block(0x40, 0xEE), sent;
        for (uint32_t i = 0; i < 32; ++i) sent.push_back(static_cast<uint8_t>(i * 7 + 3));
        FmEngineHandle eng = api.Create(kRate);
        uint32_t id = UINT32_MAX;
        api.AddChip(eng, "Y8950", kClock, &id);
        const FmResult mapped = api.SetMemoryEx(eng, id, "ADPCM_B", 0, block.data(), 0x40, access);
        std::vector<W> prog = {
            {0, 0x07, 0x01}, {0, 0x07, 0x00}, {0, 0x08, 0x00},
            {0, 0x09, 0x00}, {0, 0x0A, 0x00}, {0, 0x0B, 0xFF}, {0, 0x0C, 0x00},
            {0, 0x07, 0x60},
        };
        for (uint8_t v : sent) prog.push_back({0, 0x0F, v});
        writeAll(api, eng, id, prog);
        float l, r;
        api.Generate(eng, &l, &r, 1);
        const bool stored = std::equal(sent.begin(), sent.end(), block.begin());
        const bool untouched = std::all_of(block.begin(), block.end(), [](uint8_t v) { return v == 0xEE; });
        const bool ram = access == FM_ACCESS_RAM;
        std::snprintf(msg, sizeof msg, "sound Y8950 transfer to a %s block: %s",
                      ram ? "FM_ACCESS_RAM" : "FM_ACCESS_ROM",
                      stored ? "stored" : (untouched ? "untouched" : "partly written"));
        check(mapped == FM_OK && (ram ? stored : untouched), msg);
        api.Destroy(eng);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <engine DLL>\n", argv[0]);
        return 2;
    }
    void* lib = openLibrary(argv[1]);
    if (!lib) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }

    Api api;
    if (testSymbols(lib, api)) {
        testChips(api);
        testPart(api);
        testMemory(api);
        testSound(api);
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
