// main.cpp
// FmEngineApi.dll を使うサンプルアプリ。
// FmEngine.h / WasapiOutput.h を include せず、
// FmEngineApi.h (C ファサード) だけで完結する。

#include "FmEngineApi.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <windows.h>   // Sleep(), WideCharToMultiByte()

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
//  OPL3 簡易メロディ
// -------------------------------------------------------
static void setupOpl3(FmEngineHandle eng, uint32_t id) {
    FmEngine_Write(eng, id, 0x05, 0x01, 1);
    FmEngine_Write(eng, id, 0x20, 0x01, 0);
    FmEngine_Write(eng, id, 0x40, 0x10, 0);
    FmEngine_Write(eng, id, 0x60, 0xF0, 0);
    FmEngine_Write(eng, id, 0x80, 0x77, 0);
    FmEngine_Write(eng, id, 0x23, 0x01, 0);
    FmEngine_Write(eng, id, 0x43, 0x00, 0);
    FmEngine_Write(eng, id, 0x63, 0xF0, 0);
    FmEngine_Write(eng, id, 0x83, 0x55, 0);
    FmEngine_Write(eng, id, 0xA0, 0x6A, 0);
    FmEngine_Write(eng, id, 0xB0, 0x34, 0); // Key-on
}

// -------------------------------------------------------
//  OPN2 簡易トーン
// -------------------------------------------------------
static void setupOpn2(FmEngineHandle eng, uint32_t id) {
    FmEngine_Write(eng, id, 0x22, 0x00, 0);
    FmEngine_Write(eng, id, 0x2B, 0x00, 0);
    FmEngine_Write(eng, id, 0xB0, 0x04, 0);
    FmEngine_Write(eng, id, 0xB4, 0xC0, 0);
    // OP1-4 (DT/MUL, TL, AR, D1R, D2R, SL/RR, SSG)
    const uint8_t ops[4][7] = {
        {0x71, 0x23, 0x1F, 0x05, 0x02, 0x11, 0x00},
        {0x0D, 0x2D, 0x1F, 0x05, 0x02, 0x11, 0x00},
        {0x33, 0x26, 0x1F, 0x05, 0x02, 0x11, 0x00},
        {0x01, 0x00, 0x1F, 0x07, 0x02, 0x11, 0x00},
    };
    const uint8_t bases[4] = {0x30, 0x34, 0x38, 0x3C};
    for (int i = 0; i < 4; ++i) {
        FmEngine_Write(eng, id, bases[i]+0x00, ops[i][0], 0);
        FmEngine_Write(eng, id, bases[i]+0x10, ops[i][1], 0);
        FmEngine_Write(eng, id, bases[i]+0x20, ops[i][2], 0);
        FmEngine_Write(eng, id, bases[i]+0x30, ops[i][3], 0);
        FmEngine_Write(eng, id, bases[i]+0x40, ops[i][4], 0);
        FmEngine_Write(eng, id, bases[i]+0x50, ops[i][5], 0);
        FmEngine_Write(eng, id, bases[i]+0x90, ops[i][6], 0);
    }
    FmEngine_Write(eng, id, 0xA4, 0x22, 0);
    FmEngine_Write(eng, id, 0xA0, 0x8A, 0);
    FmEngine_Write(eng, id, 0x28, 0xF0, 0); // Key-on
}

// -------------------------------------------------------
//  dB → 線形変換 (FmEngineApi は ChipGain::dBToLinear を公開しないため)
// -------------------------------------------------------
static float dBToLinear(float dB) {
    return powf(10.0f, dB / 20.0f);
}

// -------------------------------------------------------
//  main
// -------------------------------------------------------
int main() {
    // ① エンジン作成 (48000 Hz)
    FmEngineHandle eng = FmEngine_Create(48000);
    if (!eng) { fputs("FmEngine_Create failed\n", stderr); return 1; }

    // ② チップ追加 (クロック明示)
    uint32_t opl3Id = 0, opn2Id = 0;
    check(FmEngine_AddChip(eng, FM_CHIP_OPL3, 0,          &opl3Id), "AddChip OPL3");
    check(FmEngine_AddChip(eng, FM_CHIP_OPN2, 7600489u,   &opn2Id), "AddChip OPN2 PAL");

    printf("%-20s  native=%5u Hz  target=%5u Hz\n",
           FmEngine_GetChipName(eng, opl3Id),
           FmEngine_GetNativeRate(eng, opl3Id),
           FmEngine_GetSampleRate(eng));
    printf("%-20s  native=%5u Hz  target=%5u Hz\n",
           FmEngine_GetChipName(eng, opn2Id),
           FmEngine_GetNativeRate(eng, opn2Id),
           FmEngine_GetSampleRate(eng));

    // ③ ゲイン設定
    check(FmEngine_SetGain(eng, opl3Id, 1.0f, 1.0f),                             "SetGain OPL3");
    check(FmEngine_SetGain(eng, opn2Id, dBToLinear(-3.0f), dBToLinear(-3.0f)),   "SetGain OPN2");

    float gl, gr;
    FmEngine_GetGain(eng, opn2Id, &gl, &gr);
    printf("OPN2 gain: L=%.3f  R=%.3f\n", gl, gr);

    // ④ オーディオデバイス列挙・選択
    // Wasapi_GetDeviceCount() を呼ぶとデバイスリストが更新される。
    // 明示的に別デバイスを使いたい場合は selectedDeviceId を書き換える。
    const uint32_t deviceCount = Wasapi_GetDeviceCount();
    printf("=== Audio Devices (%u found) ===\n", deviceCount);
    wchar_t selectedDeviceId[256] = {};

    // ワイド文字列 → アクティブコードページ変換ヘルパー
    auto toMB = [](const wchar_t* wstr) -> std::string {
        if (!wstr || wstr[0] == L'\0') return {};
        const UINT cp = GetACP();
        const int len = WideCharToMultiByte(cp, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string buf(len, '\0');
        WideCharToMultiByte(cp, 0, wstr, -1, buf.data(), len, nullptr, nullptr);
        buf.resize(len - 1); // 終端 '\0' を除く
        return buf;
    };

    for (uint32_t i = 0; i < deviceCount; ++i) {
        wchar_t id[256] = {}, name[256] = {};
        Wasapi_GetDeviceId(i, id, 256);
        Wasapi_GetDeviceName(i, name, 256);
        int isDef = Wasapi_IsDefaultDevice(i);
        printf("  [%u] %s%s\n", i, toMB(name).c_str(), isDef ? " (default)" : "");
        if (isDef && selectedDeviceId[0] == L'\0')
            wcscpy_s(selectedDeviceId, id);
    }
    printf("\n");

    // ⑤ WASAPI 出力 (selectedDeviceId のデバイスを使用)
    WasapiHandle wasapi = (selectedDeviceId[0] != L'\0')
        ? Wasapi_CreateWithDevice(eng, 0, selectedDeviceId)
        : Wasapi_Create(eng, 0);
    if (!wasapi) { fputs("Wasapi_Create failed\n", stderr); FmEngine_Destroy(eng); return 1; }

    printf("Engine sample rate : %u Hz\n", FmEngine_GetSampleRate(eng));
    printf("Device sample rate : %u Hz\n", Wasapi_GetSampleRate(wasapi));
    if (FmEngine_GetSampleRate(eng) != Wasapi_GetSampleRate(wasapi)) {
        printf("  (rates differ — WasapiOutput will resample internally)\n");
    }
    printf("\n");

    check(Wasapi_Start(wasapi), "Wasapi_Start");

    // ⑤ レジスタ設定
    setupOpl3(eng, opl3Id);
    setupOpn2(eng, opn2Id);

    printf("Playing 2 seconds...\n");
    Sleep(1000);

    // ゲインをリアルタイム変更
    printf("Fade OPN2 to -12 dB\n");
    FmEngine_SetGain(eng, opn2Id, dBToLinear(-12.0f), dBToLinear(-12.0f));
    Sleep(1000);

    // ⑥ 停止・解放
    Wasapi_Stop(wasapi);
    Wasapi_Destroy(wasapi);
    FmEngine_Destroy(eng);

    printf("Done.\n");
    return 0;
}
