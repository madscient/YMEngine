// main.cpp
// FmEngineApi.dll を使うサンプルアプリ。
// FmEngine.h / WasapiOutput.h を include せず、
// FmEngineApi.h (C ファサード) だけで完結する。
//
// デモ内容:
//   Section 1: 対応チップ一覧と情報を表示
//   Section 2: OPL3 (SoundBlaster 16 風) + OPNA (PC-88 風) を同時再生
//   Section 3: リアルタイムゲイン操作デモ

#include "FmEngineApi.h"
#include <cstdio>
#include <cmath>
#include <windows.h>

// -------------------------------------------------------
//  エラーチェックヘルパー
// -------------------------------------------------------
static void check(FmResult r, const char* msg) {
    if (r != FM_OK) {
        fprintf(stderr, "ERROR %s: code=%d\n", msg, (int)r);
        ExitProcess(1);
    }
}

// -------------------------------------------------------
//  dB → 線形スケール変換
// -------------------------------------------------------
static float dBToLinear(float dB) {
    return powf(10.0f, dB / 20.0f);
}

// -------------------------------------------------------
//  Section 1: 全対応チップの情報を表示
// -------------------------------------------------------
static void printChipInfo(uint32_t sample_rate) {
    // 表示専用の一時エンジン
    FmEngineHandle eng = FmEngine_Create(sample_rate);

    const FmChipType allChips[] = {
        FM_CHIP_Y8950, FM_CHIP_OPL,   FM_CHIP_OPL2,  FM_CHIP_OPL3,
        FM_CHIP_OPL4,  FM_CHIP_OPN,   FM_CHIP_OPNA,  FM_CHIP_OPNB,
        FM_CHIP_OPNBB, FM_CHIP_OPN2,  FM_CHIP_OPM,   FM_CHIP_OPLL,
        FM_CHIP_OPLLP, FM_CHIP_OPLLX, FM_CHIP_OPZ,   FM_CHIP_VRC7,
    };

    printf("=== Supported Chips ===\n");
    printf("%-22s  %8s Hz  ->  %6u Hz (target)\n", "Name", "Native", sample_rate);
    printf("%-22s  %----------\n", "----", "----");

    for (FmChipType ct : allChips) {
        uint32_t id = 0;
        if (FmEngine_AddChip(eng, ct, 0, &id) == FM_OK) {
            printf("  %-20s  %8u Hz\n",
                FmEngine_GetChipName(eng, id),
                FmEngine_GetNativeRate(eng, id));
        }
    }
    printf("\n");
    FmEngine_Destroy(eng);
}

// -------------------------------------------------------
//  OPL3 (YMF262) — CH0 にシンプルな FM トーン
//  A4 (440 Hz) 相当、2オペレータ
// -------------------------------------------------------
static void setupOpl3(FmEngineHandle eng, uint32_t id) {
    // OPL3 モード有効
    FmEngine_Write(eng, id, 0x05, 0x01, 1);

    // Operator 0 (Modulator): AM/VIB/EG/KSR/MULT
    FmEngine_Write(eng, id, 0x20, 0x01, 0);  // MULT=1
    FmEngine_Write(eng, id, 0x40, 0x28, 0);  // KSL=0, TL=40 (volume)
    FmEngine_Write(eng, id, 0x60, 0xF2, 0);  // AR=15, DR=2
    FmEngine_Write(eng, id, 0x80, 0x35, 0);  // SL=3, RR=5

    // Operator 1 (Carrier)
    FmEngine_Write(eng, id, 0x23, 0x01, 0);
    FmEngine_Write(eng, id, 0x43, 0x00, 0);  // TL=0 (full vol)
    FmEngine_Write(eng, id, 0x63, 0xF2, 0);
    FmEngine_Write(eng, id, 0x83, 0x35, 0);

    // CH0 出力: L+R
    FmEngine_Write(eng, id, 0xC0, 0x31, 0);  // feedback=3, algo=1, L+R

    // CH0 周波数: A4 (440 Hz) Block=4, F-num=0x16A
    FmEngine_Write(eng, id, 0xA0, 0x6A, 0);  // F-num low
    FmEngine_Write(eng, id, 0xB0, 0x34, 0);  // Key-on + Block=4 + F-num high
}

// -------------------------------------------------------
//  OPNA (YM2608) — CH0 に FM トーン
//  PC-88 / PC-98 を意識した設定
// -------------------------------------------------------
static void setupOpna(FmEngineHandle eng, uint32_t id) {
    // LFO off
    FmEngine_Write(eng, id, 0x22, 0x00, 0);

    // CH0: algorithm=4, feedback=6
    FmEngine_Write(eng, id, 0xB0, 0x34, 0);
    // CH0: L+R output
    FmEngine_Write(eng, id, 0xB4, 0xC0, 0);

    // OP1-4 (base=0x30+op_offset, ch=0)
    //  base offsets: OP1=+0x00, OP2=+0x08, OP3=+0x04, OP4=+0x0C
    //  register groups: DT/MUL=0x30, TL=0x40, RS/AR=0x50, AM/D1R=0x60, D2R=0x70, SL/RR=0x80
    struct OpParam { uint8_t dt_mul, tl, rs_ar, am_d1r, d2r, sl_rr; };
    const OpParam ops[4] = {
        {0x71, 0x1F, 0x9F, 0x05, 0x02, 0x11},  // OP1
        {0x0D, 0x1F, 0x9F, 0x05, 0x02, 0x11},  // OP2
        {0x33, 0x1F, 0x9F, 0x05, 0x02, 0x11},  // OP3
        {0x01, 0x00, 0x9F, 0x07, 0x02, 0x11},  // OP4
    };
    const uint8_t op_offset[4] = {0x00, 0x08, 0x04, 0x0C};

    for (int i = 0; i < 4; ++i) {
        uint8_t b = op_offset[i]; // ch0 なのでそのまま
        FmEngine_Write(eng, id, 0x30 + b, ops[i].dt_mul,  0);
        FmEngine_Write(eng, id, 0x40 + b, ops[i].tl,      0);
        FmEngine_Write(eng, id, 0x50 + b, ops[i].rs_ar,   0);
        FmEngine_Write(eng, id, 0x60 + b, ops[i].am_d1r,  0);
        FmEngine_Write(eng, id, 0x70 + b, ops[i].d2r,     0);
        FmEngine_Write(eng, id, 0x80 + b, ops[i].sl_rr,   0);
    }

    // CH0 周波数: A4 (440 Hz) Block=4
    FmEngine_Write(eng, id, 0xA4, 0x22, 0);  // Block+F-num MSB
    FmEngine_Write(eng, id, 0xA0, 0x8A, 0);  // F-num LSB

    // Key-on CH0 全OP
    FmEngine_Write(eng, id, 0x28, 0xF0, 0);
}

// -------------------------------------------------------
//  OPLL (YM2413) — CH0 に内蔵音色 (Violin) で鳴らす
//  MSX2+ / Sega Master System 風
// -------------------------------------------------------
static void setupOpll(FmEngineHandle eng, uint32_t id) {
    // CH0: 内蔵音色 1 (Violin), Vol=0
    FmEngine_Write(eng, id, 0x30, 0x10, 0);  // inst=1 (Violin), vol=0

    // CH0 周波数: A4 Block=4, F-num=0x161 (OPLL 用)
    FmEngine_Write(eng, id, 0x20, 0x61, 0);  // F-num low
    FmEngine_Write(eng, id, 0x10, 0x26, 0);  // Key-on + Block + F-num high
}

// -------------------------------------------------------
//  main
// -------------------------------------------------------
int main() {
    const uint32_t SAMPLE_RATE = 48000;

    // ===================================================
    //  Section 1: チップ情報表示
    // ===================================================
    printChipInfo(SAMPLE_RATE);

    // ===================================================
    //  Section 2: 再生エンジン構築
    // ===================================================
    FmEngineHandle eng = FmEngine_Create(SAMPLE_RATE);
    if (!eng) { fputs("FmEngine_Create failed\n", stderr); return 1; }

    // チップ追加
    //   OPL3:  標準クロック 14.318 MHz
    //   OPNA:  標準クロック  7.987 MHz
    //   OPLL:  標準クロック  3.580 MHz (ゲイン低めで混在デモ)
    uint32_t opl3Id = 0, opnaId = 0, opllId = 0;
    check(FmEngine_AddChip(eng, FM_CHIP_OPL3, 0, &opl3Id), "AddChip OPL3");
    check(FmEngine_AddChip(eng, FM_CHIP_OPNA, 0, &opnaId), "AddChip OPNA");
    check(FmEngine_AddChip(eng, FM_CHIP_OPLL, 0, &opllId), "AddChip OPLL");

    printf("=== Engine chips ===\n");
    printf("  [%u] %-20s  native=%u Hz\n", opl3Id,
        FmEngine_GetChipName(eng, opl3Id), FmEngine_GetNativeRate(eng, opl3Id));
    printf("  [%u] %-20s  native=%u Hz\n", opnaId,
        FmEngine_GetChipName(eng, opnaId), FmEngine_GetNativeRate(eng, opnaId));
    printf("  [%u] %-20s  native=%u Hz\n", opllId,
        FmEngine_GetChipName(eng, opllId), FmEngine_GetNativeRate(eng, opllId));
    printf("\n");

    // ===================================================
    //  Section 3: ゲイン設定
    // ===================================================
    check(FmEngine_SetGain(eng, opl3Id, 1.0f,                1.0f),                "SetGain OPL3");
    check(FmEngine_SetGain(eng, opnaId, dBToLinear(-3.0f),   dBToLinear(-3.0f)),   "SetGain OPNA");
    check(FmEngine_SetGain(eng, opllId, dBToLinear(-6.0f),   dBToLinear(-6.0f)),   "SetGain OPLL");

    printf("=== Initial gains ===\n");
    float gl, gr;
    FmEngine_GetGain(eng, opl3Id, &gl, &gr); printf("  OPL3  L=%.3f  R=%.3f\n", gl, gr);
    FmEngine_GetGain(eng, opnaId, &gl, &gr); printf("  OPNA  L=%.3f  R=%.3f\n", gl, gr);
    FmEngine_GetGain(eng, opllId, &gl, &gr); printf("  OPLL  L=%.3f  R=%.3f\n", gl, gr);
    printf("\n");

    // ===================================================
    //  WASAPI 出力 (Shared mode)
    // ===================================================
    WasapiHandle wasapi = Wasapi_Create(eng, 0);
    if (!wasapi) {
        fputs("Wasapi_Create failed\n", stderr);
        FmEngine_Destroy(eng);
        return 1;
    }
    check(Wasapi_Start(wasapi), "Wasapi_Start");

    // ===================================================
    //  レジスタ設定 (再生中に書き込み可能)
    // ===================================================
    setupOpl3(eng, opl3Id);
    setupOpna(eng, opnaId);
    setupOpll(eng, opllId);

    printf("Playing: OPL3 + OPNA + OPLL  (2 sec)...\n");
    Sleep(2000);

    // ===================================================
    //  リアルタイムゲイン操作デモ
    // ===================================================
    printf("Mute OPNA, boost OPLL to 0 dB...\n");
    FmEngine_SetGain(eng, opnaId, 0.0f, 0.0f);
    FmEngine_SetGain(eng, opllId, 1.0f, 1.0f);
    Sleep(1000);

    printf("Pan OPL3 hard-left...\n");
    FmEngine_SetGain(eng, opl3Id, 1.0f, 0.0f);
    Sleep(1000);

    printf("Restore all to 0 dB...\n");
    FmEngine_SetGain(eng, opl3Id, 1.0f, 1.0f);
    FmEngine_SetGain(eng, opnaId, 1.0f, 1.0f);
    FmEngine_SetGain(eng, opllId, 1.0f, 1.0f);
    Sleep(1000);

    // ===================================================
    //  停止・解放
    // ===================================================
    Wasapi_Stop(wasapi);
    Wasapi_Destroy(wasapi);
    FmEngine_Destroy(eng);

    printf("Done.\n");
    return 0;
}
