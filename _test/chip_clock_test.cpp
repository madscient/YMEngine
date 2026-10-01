// chip_clock_test.cpp
// チップのクロック (FmChip.h の createChip / createChipByName と FmEngine の
// addChip / addChipByName) の回帰テスト。
//
//   reject : 全チップで clock=0 を拒否し、チップが増えないこと。未知の名前も拒否
//   pass   : 渡したクロックがそのままチップに渡ること。既定値に置き換えていれば
//            同じ値にはならないよう、testClock() の値とその2倍の両方で見る。
//            ネイティブレートがクロックに比例すること (2倍で2倍、端数切り捨ての
//            ぶん +1 まで)
//
// 全件通れば終了コード 0。

#include "FmEngine.h"
#include "test_clocks.h"
#include <cstdio>

static int  g_fail = 0;
static char msg[256];

static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++g_fail;
}

static void testReject() {
    for (const ChipEntry* e = chipTable(); e->name; ++e) {
        FmEngine eng(48000);
        const bool engine = eng.addChip(e->type, 0) == UINT32_MAX &&
                            eng.addChipByName(e->name, 0) == UINT32_MAX &&
                            eng.chipCount() == 0;
        const bool factory = !createChip(e->type, 0) && !createChipByName(e->name, 0);
        std::snprintf(msg, sizeof msg, "reject %-6s clock=0", e->name);
        check(engine && factory, msg);
    }
    FmEngine eng(48000);
    check(eng.addChipByName("NOPE", 3'579'545) == UINT32_MAX && eng.chipCount() == 0,
          "reject unknown name");
}

static void testPass() {
    for (const ChipEntry* e = chipTable(); e->name; ++e) {
        const uint32_t c = testClock(e->type);
        FmEngine eng(48000);
        const uint32_t a = eng.addChip(e->type, c);
        const uint32_t b = eng.addChipByName(e->name, c * 2);
        const bool added = a != UINT32_MAX && b != UINT32_MAX;
        const uint32_t ca = added ? eng.chip(a)->clock() : 0, cb = added ? eng.chip(b)->clock() : 0;
        const uint32_t ra = eng.nativeRate(a), rb = eng.nativeRate(b);
        std::snprintf(msg, sizeof msg, "pass %-6s clock %u -> %u, %u -> %u; native %u -> %u",
                      e->name, c, ca, c * 2, cb, ra, rb);
        check(added && ca == c && cb == c * 2 && ra > 0 && (rb == ra * 2 || rb == ra * 2 + 1), msg);
    }
}

int main() {
    testReject();
    testPass();

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
