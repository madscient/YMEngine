// test_clocks.h
// テストで使うチップのクロック (Hz)。エンジンは既定のクロックを持たないので、
// テストが渡す。テストの期待値 (OPNA のネイティブレート 55,466Hz など) は
// この値を前提にしている。
#pragma once

#include "FmChip.h"
#include <cstring>

inline uint32_t testClock(ChipType type) {
    switch (type) {
        case ChipType::Y8950:
        case ChipType::OPL:
        case ChipType::OPL2:
        case ChipType::OPLL:
        case ChipType::OPLLP:
        case ChipType::OPLLX:
        case ChipType::VRC7:
        case ChipType::OPM:
        case ChipType::OPZ:   return 3'579'545;
        case ChipType::OPL3:  return 14'318'180;
        case ChipType::OPL4:  return 33'868'800;
        case ChipType::OPN:   return 3'993'600;
        case ChipType::OPNA:  return 7'987'200;
        case ChipType::OPNB:
        case ChipType::OPNBB: return 8'000'000;
        case ChipType::OPN2:  return 7'670'453;
    }
    return 0;
}

// 未知の名前なら 0
inline uint32_t testClock(const char* name) {
    for (const ChipEntry* e = chipTable(); e->name; ++e)
        if (std::strcmp(e->name, name) == 0) return testClock(e->type);
    return 0;
}
