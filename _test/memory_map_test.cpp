// memory_map_test.cpp
// 外部メモリの割り当て (FmChip.h の MemoryYmfmInterface と FmEngine の
// mapMemory / setMemory / getMemorySize) の回帰テスト。
//
//   accept : 全チップ × 全メモリ種別で、mapMemory が受け付ける組み合わせ。
//            範囲の検査 (size 0、2^32 越え、重なり、隣接)、未知の access、
//            nullptr での取り外し、getMemorySize が大きさの合計を返すこと。
//            setMemory が ADPCM_B_ROMMODE と範囲外の種別を拒否し、それまでの
//            割り当てを [0, size) に置き換えること
//   route  : MemoryYmfmInterface 単体で、ymfm のアクセス種別と ROM/RAM 選択ビット
//            から、どのブロックのどのバイトを読み書きするか。ブロックの境界、
//            割り当ての無い番地、ROM のブロックへの書き込み、ACCESS_IO
//   play   : 実際のチップ (OPNA / Y8950 / OPNB) で ADPCM-B を外部メモリから
//            再生し、選択ビットに応じた側のブロックだけが読まれること。何も
//            割り当てない場合と出力を比べ、読まれるべき側に割り当てると変わり、
//            反対側に割り当てても1サンプルも変わらないことを見る。RAM の
//            ブロックは複製されず、生成の合間に書き換えると出力が変わること
//   store  : レジスタ経由の転送 (ADPCM-B の録音モード、OPL4 のメモリアクセス
//            モード) で、チップの書き込みが RAM のブロックにその場で入り、ROM の
//            ブロックと setMemory() のデータには入らないこと。write() の直後には
//            入っておらず、その後の generate() が戻った時点で入っていること。
//            KEY ON/OFF の衝突で書き込みを保留している間は、後続の転送も
//            持ち越されること
//
// 全件通れば終了コード 0。

#include "FmEngine.h"
#include "test_clocks.h"
#include <algorithm>
#include <cstdio>
#include <vector>

static int  g_fail = 0;
static char msg[256];

static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_fail;
}

struct W { uint32_t port; uint8_t reg; uint8_t val; };

static void writeAll(FmEngine& eng, uint32_t id, const std::vector<W>& prog) {
    for (const W& w : prog) eng.write(id, w.reg, w.val, w.port);
}

constexpr auto ROM = ChipMemoryAccess::ROM;
constexpr auto RAM = ChipMemoryAccess::RAM;

// =========================================================
//  accept
// =========================================================
static void testAccept() {
    // bit n = ChipMemoryType の n 番
    constexpr uint32_t A = 1u << 1, B = 1u << 2, P = 1u << 3, R = 1u << 4;
    struct Row { ChipType type; uint32_t mask; };
    const Row rows[] = {
        {ChipType::Y8950, B | R}, {ChipType::OPL,   0}, {ChipType::OPL2,  0},
        {ChipType::OPL3,  0},     {ChipType::OPL4,  P}, {ChipType::OPN,   0},
        {ChipType::OPNA,  A | B | R}, {ChipType::OPNB, A | B}, {ChipType::OPNBB, A | B},
        {ChipType::OPN2,  0},     {ChipType::OPM,   0}, {ChipType::OPLL,  0},
        {ChipType::OPLLP, 0},     {ChipType::OPLLX, 0}, {ChipType::OPZ,   0},
        {ChipType::VRC7,  0},
    };
    std::vector<uint8_t> buf(16);
    for (const Row& row : rows) {
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(row.type, testClock(row.type));
        uint32_t mapped = 0, unmapped = 0;
        // 0 と kChipMemoryTypeEnd は範囲外の番号
        for (uint32_t t = 0; t <= kChipMemoryTypeEnd; ++t) {
            const auto type = static_cast<ChipMemoryType>(t);
            if (eng.mapMemory(id, type, 0, buf.data(), 16, ROM)) mapped |= 1u << t;
            if (eng.mapMemory(id, type, 0, nullptr, 16, ROM))    unmapped |= 1u << t;
        }
        std::snprintf(msg, sizeof msg, "accept %-16s map=0x%02X unmap=0x%02X (expect 0x%02X)",
                      eng.getChipName(id), mapped, unmapped, row.mask);
        check(mapped == row.mask && unmapped == row.mask, msg);
    }

    {
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(ChipType::OPL4, testClock(ChipType::OPL4));
        const auto PCM = ChipMemoryType::PCM;
        std::vector<uint8_t> a(0x200), b(0x100), c(0x10);
        check(!eng.mapMemory(id + 1, PCM, 0, a.data(), 1, ROM), "unknown chip_id rejected");
        check(!eng.mapMemory(id, PCM, 0, a.data(), 0, ROM), "size 0 rejected");
        check(!eng.mapMemory(id, PCM, 0, nullptr, 0, ROM), "unmap with size 0 rejected");
        check(!eng.mapMemory(id, PCM, 0, a.data(), 1, static_cast<ChipMemoryAccess>(2)),
              "unknown access rejected");
        check(!eng.mapMemory(id, PCM, 0xFFFFFFF0u, c.data(), 0x11, ROM), "range over 2^32 rejected");
        check(!eng.mapMemory(id, PCM, 0xFFFFFFF0u, nullptr, 0x11, ROM), "unmap over 2^32 rejected");
        check(eng.mapMemory(id, PCM, 0xFFFFFFF0u, c.data(), 0x10, ROM), "range ending at 2^32 accepted");
        check(eng.mapMemory(id, PCM, 0x1000, a.data(), 0x100, ROM), "map [0x1000, 0x1100)");
        check(!eng.mapMemory(id, PCM, 0x10FF, b.data(), 1, ROM), "overlap with the last byte rejected");
        check(!eng.mapMemory(id, PCM, 0x0F01, b.data(), 0x100, ROM), "overlap with the first byte rejected");
        check(!eng.mapMemory(id, PCM, 0x0F00, b.data(), 0x300, ROM), "covering range rejected");
        check(eng.mapMemory(id, PCM, 0x0F00, b.data(), 0x100, ROM), "adjacent below accepted");
        check(eng.mapMemory(id, PCM, 0x1100, b.data(), 0x100, RAM), "adjacent above accepted");
        std::snprintf(msg, sizeof msg, "getMemorySize = sum of blocks: 0x%X", eng.getMemorySize(id, PCM));
        check(eng.getMemorySize(id, PCM) == 0x10 + 0x300, msg);
        check(eng.mapMemory(id, PCM, 0x10FF, nullptr, 2, ROM), "unmap a range across two blocks");
        std::snprintf(msg, sizeof msg, "  removes both whole blocks: 0x%X", eng.getMemorySize(id, PCM));
        check(eng.getMemorySize(id, PCM) == 0x10 + 0x100, msg);
        check(eng.mapMemory(id, PCM, 0x1000, a.data(), 0x200, ROM), "  the range can be mapped again");
        check(eng.mapMemory(id, PCM, 0x8000, nullptr, 0x100, ROM), "unmap where nothing is mapped");

        check(!eng.setMemory(id, ChipMemoryType::ADPCM_B_ROMMODE, a.data(), 1), "setMemory rejects ADPCM_B_ROMMODE");
        check(!eng.setMemory(id, static_cast<ChipMemoryType>(0), a.data(), 1), "setMemory rejects type 0");
        check(!eng.setMemory(id, static_cast<ChipMemoryType>(kChipMemoryTypeEnd), a.data(), 1),
              "setMemory rejects type past the end");
        check(!eng.setMemory(id, PCM, nullptr, 1), "setMemory rejects nullptr");
        check(!eng.setMemory(id, PCM, a.data(), 0), "setMemory rejects size 0");
        check(!eng.setMemory(id + 1, PCM, a.data(), 1), "setMemory rejects unknown chip_id");
        check(eng.setMemory(id, PCM, a.data(), 0x80) && eng.getMemorySize(id, PCM) == 0x80,
              "setMemory replaces every block");
        check(!eng.mapMemory(id, PCM, 0x7F, b.data(), 1, ROM) &&
              eng.mapMemory(id, PCM, 0x80, b.data(), 1, ROM), "  with [0, size)");
    }

    {
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(ChipType::OPN2, testClock(ChipType::OPN2));
        std::vector<uint8_t> a(16);
        check(eng.setMemory(id, ChipMemoryType::ADPCM_B, a.data(), 16) &&
              eng.getMemorySize(id, ChipMemoryType::ADPCM_B) == 16,
              "setMemory accepts a type the chip does not have");
    }
}

// =========================================================
//  route
// =========================================================
static void testRoute() {
    ymfm::adpcm_b_registers regs;
    regs.reset();
    MemoryYmfmInterface m;
    m.bindAdpcmBRegs(&regs);

    std::vector<uint8_t> ram(0x100), rom(0x100), low(0x10, 0x5A), a(0x10), pcm(0x10, 0);
    for (size_t i = 0; i < ram.size(); ++i) { ram[i] = uint8_t(i); rom[i] = uint8_t(0x80 | i); }
    for (size_t i = 0; i < a.size(); ++i) a[i] = uint8_t(0x40 + i);
    m.map(ChipMemoryType::ADPCM_B,         0x100, ram.data(), 0x100, RAM);
    m.map(ChipMemoryType::ADPCM_B,         0,     low.data(), 0x10,  RAM);
    m.map(ChipMemoryType::ADPCM_B_ROMMODE, 0x100, rom.data(), 0x100, ROM);
    m.map(ChipMemoryType::ADPCM_A,         0,     a.data(),   0x10,  ROM);
    m.map(ChipMemoryType::PCM,             0,     pcm.data(), 0x10,  RAM);

    auto rd = [&](ymfm::access_class t, uint32_t addr) { return m.ymfm_external_read(t, addr); };
    const auto B = ymfm::ACCESS_ADPCM_B;

    regs.write(0x01, 0x00);  // RAM (x1)
    check(rd(B, 0x0FF) == 0 && rd(B, 0x100) == 0x00 && rd(B, 0x1FF) == 0xFF && rd(B, 0x200) == 0,
          "RAM mode reads ADPCM_B; unmapped addresses around the block read 0");
    check(rd(B, 0x105) == 0x05 && rd(B, 0x00F) == 0x5A, "  base + i reads data[i] in each block");
    regs.write(0x01, 0x02);  // RAM (x8)
    check(rd(B, 0x105) == 0x05, "x8 RAM mode also reads ADPCM_B");
    regs.write(0x01, 0x01);  // ROM
    check(rd(B, 0x105) == 0x85 && rd(B, 0x00F) == 0, "ROM mode reads ADPCM_B_ROMMODE");

    m.ymfm_external_write(B, 0x110, 0xEE);  // ROM モード中の書き込みは ROM のブロックに当たる
    check(rom[0x10] == 0x90 && ram[0x10] == 0x10, "write to a ROM block is dropped");
    regs.write(0x01, 0x00);
    m.ymfm_external_write(B, 0x110, 0xEE);
    check(ram[0x10] == 0xEE && rom[0x10] == 0x90, "write to a RAM block lands in place");
    m.ymfm_external_write(B, 0x300, 0xEE);  // 割り当ての無い番地
    check(rd(B, 0x300) == 0, "write to an unmapped address is dropped");

    check(rd(ymfm::ACCESS_ADPCM_A, 3) == 0x43 && rd(ymfm::ACCESS_PCM, 3) == 0,
          "ADPCM_A and PCM read their own blocks");
    m.ymfm_external_write(ymfm::ACCESS_PCM, 3, 0x77);
    check(pcm[3] == 0x77, "  PCM write lands in its RAM block");
    m.ymfm_external_write(ymfm::ACCESS_IO, 0, 0x11);
    check(rd(ymfm::ACCESS_IO, 0) == 0 && low[0] == 0x5A && a[0] == 0x40 && pcm[0] == 0,
          "ACCESS_IO touches no memory");

    MemoryYmfmInterface unbound;
    unbound.map(ChipMemoryType::ADPCM_B,         0, ram.data(), 0x100, ROM);
    unbound.map(ChipMemoryType::ADPCM_B_ROMMODE, 0, rom.data(), 0x100, ROM);
    check(unbound.ymfm_external_read(B, 5) == 0x05, "without bound registers ADPCM-B reads ADPCM_B");
}

// =========================================================
//  play
// =========================================================
static constexpr uint32_t kPlaySamples = 4800;

// ADPCM-B を外部メモリの番地 0 から繰り返し再生する。end=0x00FF なので、ROM と
// x8 は 8KB、x1 は 1KB、OPNB (256 バイト刻み) は end=0x001F で 8KB を読む
static std::vector<W> opnaPlay(bool romMode) {
    return {
        {1, 0x00, 0x01}, {1, 0x00, 0x00},                   // リセット
        {1, 0x01, uint8_t(0xC0 | (romMode ? 0x01 : 0x00))}, // L/R, ROM/RAM
        {1, 0x02, 0x00}, {1, 0x03, 0x00},                   // start
        {1, 0x04, 0xFF}, {1, 0x05, 0x00},                   // end
        {1, 0x09, 0xFF}, {1, 0x0A, 0xFF},                   // delta-N
        {1, 0x0B, 0xFF},                                    // level
        {1, 0x00, 0xB0},                                    // start | memory | repeat
    };
}

static std::vector<W> y8950Play(bool romMode) {
    return {
        {0, 0x07, 0x01}, {0, 0x07, 0x00},
        {0, 0x08, uint8_t(romMode ? 0x01 : 0x00)},
        {0, 0x09, 0x00}, {0, 0x0A, 0x00},
        {0, 0x0B, 0xFF}, {0, 0x0C, 0x00},
        {0, 0x10, 0xFF}, {0, 0x11, 0xFF},
        {0, 0x12, 0xFF},
        {0, 0x07, 0xB0},
    };
}

// OPNB は ROM/RAM 選択ビットを持たない。ビットを立てても ADPCM_B を読むことを見る
static std::vector<W> opnbPlay() {
    return {
        {0, 0x10, 0x01}, {0, 0x10, 0x00},
        {0, 0x11, 0xC1},
        {0, 0x12, 0x00}, {0, 0x13, 0x00},
        {0, 0x14, 0x1F}, {0, 0x15, 0x00},
        {0, 0x19, 0xFF}, {0, 0x1A, 0xFF},
        {0, 0x1B, 0xFF},
        {0, 0x10, 0x90},
    };
}

struct Mapping { ChipMemoryType type; bool legacy; };

static std::vector<float> play(ChipType chip, const std::vector<W>& prog, const Mapping* map,
                               std::vector<uint8_t>& mem) {
    FmEngine eng(48000);
    const uint32_t id = eng.addChip(chip, testClock(chip));
    if (map) {
        const uint32_t size = static_cast<uint32_t>(mem.size());
        const bool ok = map->legacy ? eng.setMemory(id, map->type, mem.data(), size)
                                    : eng.mapMemory(id, map->type, 0, mem.data(), size, ROM);
        if (!ok) check(false, "play: mapping rejected");
    }
    writeAll(eng, id, prog);
    std::vector<float> l(kPlaySamples), r(kPlaySamples);
    eng.generate(l.data(), r.data(), kPlaySamples);
    return l;
}

static size_t mismatches(const std::vector<float>& a, const std::vector<float>& b) {
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i) n += (a[i] != b[i]);
    return n;
}

// 何も割り当てない場合と比べ、read 側に割り当てると変わり、other 側では変わらない。
// 割り当てが無いと 0 を読み、アキュムレータはゆっくり上限に張り付く。8KB を回る
// ROM モードでは、0x77 を読んだ側も上限に張り付いた後半で一致するので、変わる
// サンプル数の閾値は全体の1割にしてある
static void checkSide(const char* name, ChipType chip, const std::vector<W>& prog,
                      Mapping read, const Mapping* other) {
    std::vector<uint8_t> mem(0x2000, 0x77), none;
    const auto base = play(chip, prog, nullptr, none);
    const size_t dRead  = mismatches(play(chip, prog, &read, mem), base);
    const size_t dOther = other ? mismatches(play(chip, prog, other, mem), base) : 0;
    if (other)
        std::snprintf(msg, sizeof msg, "%s: differs from unmapped by %zu samples / other side by %zu",
                      name, dRead, dOther);
    else
        std::snprintf(msg, sizeof msg, "%s: differs from unmapped by %zu samples", name, dRead);
    check(dRead > kPlaySamples / 10 && dOther == 0, msg);
}

static void testPlay() {
    const Mapping ramSide{ChipMemoryType::ADPCM_B, false};
    const Mapping romSide{ChipMemoryType::ADPCM_B_ROMMODE, false};
    const Mapping legacy {ChipMemoryType::ADPCM_B, true};
    checkSide("OPNA  RAM mode reads ADPCM_B",         ChipType::OPNA,  opnaPlay(false),  ramSide, &romSide);
    checkSide("OPNA  ROM mode reads ADPCM_B_ROMMODE", ChipType::OPNA,  opnaPlay(true),   romSide, &ramSide);
    checkSide("Y8950 RAM mode reads ADPCM_B",         ChipType::Y8950, y8950Play(false), ramSide, &romSide);
    checkSide("Y8950 ROM mode reads ADPCM_B_ROMMODE", ChipType::Y8950, y8950Play(true),  romSide, &ramSide);
    checkSide("OPNB  reads ADPCM_B with the ROM bit", ChipType::OPNB,  opnbPlay(),       ramSide, nullptr);
    checkSide("OPNA  setMemory(ADPCM_B) is the RAM-mode memory", ChipType::OPNA, opnaPlay(true),
              romSide, &legacy);

    // RAM のブロックは複製されない。生成の合間に書き換えると、次の生成から読まれる
    {
        std::vector<uint8_t> mem(0x2000, 0x00);
        FmEngine a(48000), b(48000);
        const uint32_t clk = testClock(ChipType::Y8950);
        const uint32_t ia = a.addChip(ChipType::Y8950, clk), ib = b.addChip(ChipType::Y8950, clk);
        a.mapMemory(ia, ChipMemoryType::ADPCM_B, 0, mem.data(), static_cast<uint32_t>(mem.size()), RAM);
        writeAll(a, ia, y8950Play(false));
        writeAll(b, ib, y8950Play(false));
        std::vector<float> la(kPlaySamples), ra(kPlaySamples), lb(kPlaySamples), rb(kPlaySamples);
        a.generate(la.data(), ra.data(), kPlaySamples);
        b.generate(lb.data(), rb.data(), kPlaySamples);
        const size_t before = mismatches(la, lb);
        std::fill(mem.begin(), mem.end(), 0x77);
        a.generate(la.data(), ra.data(), kPlaySamples);
        b.generate(lb.data(), rb.data(), kPlaySamples);
        const size_t after = mismatches(la, lb);
        std::snprintf(msg, sizeof msg,
                      "RAM block is read in place: zeros vs unmapped %zu, after filling %zu", before, after);
        check(before == 0 && after > kPlaySamples / 10, msg);
    }
}

// =========================================================
//  store
// =========================================================
static constexpr uint8_t kSentinel = 0xEE;
static constexpr size_t  kBlock    = 0x40;

static std::vector<uint8_t> payload() {
    std::vector<uint8_t> v(32);
    for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(i * 7 + 3);
    return v;
}

// 録音モード (rec | memory) で番地 0 から bytes を書く
static std::vector<W> y8950Store(bool romMode, const std::vector<uint8_t>& bytes) {
    std::vector<W> p = {
        {0, 0x07, 0x01}, {0, 0x07, 0x00},
        {0, 0x08, uint8_t(romMode ? 0x01 : 0x00)},
        {0, 0x09, 0x00}, {0, 0x0A, 0x00},
        {0, 0x0B, 0xFF}, {0, 0x0C, 0x00},
        {0, 0x07, 0x60},
    };
    for (uint8_t b : bytes) p.push_back({0, 0x0F, b});
    return p;
}

static std::vector<W> opnaStore(bool romMode, const std::vector<uint8_t>& bytes) {
    std::vector<W> p = {
        {1, 0x00, 0x01}, {1, 0x00, 0x00},
        {1, 0x01, uint8_t(romMode ? 0x01 : 0x00)},
        {1, 0x02, 0x00}, {1, 0x03, 0x00},
        {1, 0x04, 0xFF}, {1, 0x05, 0x00},
        {1, 0x00, 0x60},
    };
    for (uint8_t b : bytes) p.push_back({1, 0x08, b});
    return p;
}

static bool holds(const std::vector<uint8_t>& block, const std::vector<uint8_t>& bytes) {
    return std::equal(bytes.begin(), bytes.end(), block.begin()) &&
           std::all_of(block.begin() + bytes.size(), block.end(),
                       [](uint8_t v) { return v == kSentinel; });
}

static bool untouched(const std::vector<uint8_t>& block) {
    return std::all_of(block.begin(), block.end(), [](uint8_t v) { return v == kSentinel; });
}

// ADPCM_B と ADPCM_B_ROMMODE の両方に番地 0 からブロックを割り当てて転送する
static void checkStore(const char* name, ChipType chip, const std::vector<W>& prog,
                       ChipMemoryAccess ramSideAccess, bool expectRamSide, bool expectRomSide) {
    const auto bytes = payload();
    std::vector<uint8_t> ramSide(kBlock, kSentinel), romSide(kBlock, kSentinel);
    FmEngine eng(48000);
    const uint32_t id = eng.addChip(chip, testClock(chip));
    eng.mapMemory(id, ChipMemoryType::ADPCM_B, 0, ramSide.data(), kBlock, ramSideAccess);
    eng.mapMemory(id, ChipMemoryType::ADPCM_B_ROMMODE, 0, romSide.data(), kBlock, RAM);
    writeAll(eng, id, prog);
    const bool beforeGenerate = untouched(ramSide) && untouched(romSide);
    float l, r;
    eng.generate(&l, &r, 1);
    const bool ok = beforeGenerate &&
                    (expectRamSide ? holds(ramSide, bytes) : untouched(ramSide)) &&
                    (expectRomSide ? holds(romSide, bytes) : untouched(romSide));
    std::snprintf(msg, sizeof msg, "%s (untouched before generate: %s)", name,
                  beforeGenerate ? "yes" : "no");
    check(ok, msg);
}

static void testStore() {
    const auto bytes = payload();
    checkStore("Y8950 RAM-mode transfer lands in the ADPCM_B RAM block", ChipType::Y8950,
               y8950Store(false, bytes), RAM, true, false);
    checkStore("Y8950 RAM-mode transfer to a ROM block is dropped", ChipType::Y8950,
               y8950Store(false, bytes), ROM, false, false);
    // ymfm は ROM モードでも書く。YMEngine はそれを止めない
    checkStore("Y8950 ROM-mode transfer lands in the ADPCM_B_ROMMODE RAM block", ChipType::Y8950,
               y8950Store(true, bytes), RAM, false, true);
    checkStore("OPNA  RAM-mode transfer lands in the ADPCM_B RAM block", ChipType::OPNA,
               opnaStore(false, bytes), RAM, true, false);
    checkStore("OPNA  RAM-mode transfer to a ROM block is dropped", ChipType::OPNA,
               opnaStore(false, bytes), ROM, false, false);
    checkStore("OPNA  ROM-mode transfer lands in the ADPCM_B_ROMMODE RAM block", ChipType::OPNA,
               opnaStore(true, bytes), RAM, false, true);

    // setMemory のデータには書かない (呼び出し側は読み取り専用のメモリを渡しうる)
    {
        std::vector<uint8_t> blk(kBlock, kSentinel);
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(ChipType::Y8950, testClock(ChipType::Y8950));
        eng.setMemory(id, ChipMemoryType::ADPCM_B, blk.data(), kBlock);
        writeAll(eng, id, y8950Store(false, bytes));
        float l, r;
        eng.generate(&l, &r, 1);
        check(untouched(blk), "Y8950 RAM-mode transfer does not write setMemory data");
    }

    // OPL4: ROM [0, 0x40) と RAM [0x200000, 0x200040) を1つの空間に並べる
    {
        std::vector<uint8_t> romBlk(kBlock, kSentinel), ramBlk(kBlock, kSentinel);
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(ChipType::OPL4, testClock(ChipType::OPL4));
        eng.mapMemory(id, ChipMemoryType::PCM, 0, romBlk.data(), kBlock, ROM);
        eng.mapMemory(id, ChipMemoryType::PCM, 0x200000, ramBlk.data(), kBlock, RAM);
        std::vector<W> p = {
            {1, 0x05, 0x03},                                    // NEW2
            {2, 0x02, 0x01},                                    // メモリアクセスモード
            {2, 0x03, 0x20}, {2, 0x04, 0x00}, {2, 0x05, 0x00},  // 番地 0x200000
        };
        for (uint8_t b : bytes) p.push_back({2, 0x06, b});
        p.insert(p.end(), {{2, 0x03, 0x00}, {2, 0x04, 0x00}, {2, 0x05, 0x00}});
        for (uint8_t b : bytes) p.push_back({2, 0x06, b});
        writeAll(eng, id, p);
        float l, r;
        eng.generate(&l, &r, 1);
        check(holds(ramBlk, bytes) && untouched(romBlk),
              "OPL4 PCM transfer lands in the RAM block and not in the ROM block");
    }

    // KEY ON → OFF の2回目は約2ms (48000Hz で 96 サンプル) 保留される。後ろに並んだ
    // 転送も、保留が明けた generate() まで反映されない
    {
        std::vector<uint8_t> blk(kBlock, kSentinel);
        FmEngine eng(48000);
        const uint32_t id = eng.addChip(ChipType::Y8950, testClock(ChipType::Y8950));
        eng.mapMemory(id, ChipMemoryType::ADPCM_B, 0, blk.data(), kBlock, RAM);
        eng.write(id, 0xB0, 0x20);
        eng.write(id, 0xB0, 0x00);
        writeAll(eng, id, y8950Store(false, bytes));
        std::vector<float> l(96), r(96);
        eng.generate(l.data(), r.data(), 1);
        const bool held = untouched(blk);
        eng.generate(l.data(), r.data(), 94);
        const bool stillHeld = untouched(blk);
        eng.generate(l.data(), r.data(), 1);
        std::snprintf(msg, sizeof msg,
                      "transfer behind a held key write: held after 1=%s, after 95=%s, stored after 96=%s",
                      held ? "yes" : "no", stillHeld ? "yes" : "no", holds(blk, bytes) ? "yes" : "no");
        check(held && stillHeld && holds(blk, bytes), msg);
    }
}

int main() {
    testAccept();
    testRoute();
    testPlay();
    testStore();

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
