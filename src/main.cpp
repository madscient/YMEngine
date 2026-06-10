// main.cpp
// YMEngine 全チップ全チャンネルテスト
//
// 動作:
//   test_patches.json を読み込み、記述されたチップ×チャンネルを順番に
//   1音ずつ鳴らす。各チャンネルの発音→消音→次のチャンネルへ。
//
// JSON パーサー:
//   nlohmann/json (header-only)
//   extern/nlohmann/json.hpp に配置すること。
//   取得: https://github.com/nlohmann/json/releases/latest/download/json.hpp

#include "FmEngineApi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <windows.h>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

// =========================================================
//  ヘルパー
// =========================================================

// 16進文字列 ("0xFF") または 10進数値を uint32_t に変換
static uint32_t parseVal(const json& j) {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        return static_cast<uint32_t>(std::stoul(s, nullptr, 0));
    }
    return j.get<uint32_t>();
}

// エラーチェック
static void check(FmResult r, const char* msg) {
    if (r != FM_OK) {
        fprintf(stderr, "ERROR %s: code=%d\n", msg, (int)r);
        ExitProcess(1);
    }
}

// wchar_t → アクティブコードページ変換
static std::string toMB(const wchar_t* wstr) {
    if (!wstr || wstr[0] == L'\0') return {};
    const UINT cp = GetACP();
    const int len = WideCharToMultiByte(cp, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string buf(len, '\0');
    WideCharToMultiByte(cp, 0, wstr, -1, buf.data(), len, nullptr, nullptr);
    buf.resize(len - 1);
    return buf;
}

// =========================================================
//  レジスタ書き込みリストを実行
// =========================================================
static void applyRegs(FmEngineHandle eng, uint32_t chip_id,
                      const json& regs, uint32_t default_port = 0)
{
    if (!regs.is_array()) return;
    for (const auto& r : regs) {
        const uint32_t reg   = parseVal(r["reg"]);
        const uint32_t val   = parseVal(r["val"]);
        const uint32_t port  = r.contains("port")
                               ? parseVal(r["port"]) : default_port;
        FmEngine_Write(eng, chip_id, static_cast<uint8_t>(reg),
                                     static_cast<uint8_t>(val), port);
    }
}

// =========================================================
//  チップ種別名 → FmChipType / FmChipTypeExt への変換
// =========================================================
struct ChipEntry {
    std::string name;
    bool        isExt;
    int         type;       // FmChipType または FmChipTypeExt の値
};

static const ChipEntry kChipTable[] = {
    // ymfm チップ
    {"Y8950",   false, FM_CHIP_Y8950  },
    {"OPL",     false, FM_CHIP_OPL    },
    {"OPL2",    false, FM_CHIP_OPL2   },
    {"OPL3",    false, FM_CHIP_OPL3   },
    {"OPL4",    false, FM_CHIP_OPL4   },
    {"OPN",     false, FM_CHIP_OPN    },
    {"OPNA",    false, FM_CHIP_OPNA   },
    {"OPNB",    false, FM_CHIP_OPNB   },
    {"OPNBB",   false, FM_CHIP_OPNBB  },
    {"OPN2",    false, FM_CHIP_OPN2   },
    {"OPM",     false, FM_CHIP_OPM    },
    {"OPLL",    false, FM_CHIP_OPLL   },
    {"OPLLP",   false, FM_CHIP_OPLLP  },
    {"OPLLX",   false, FM_CHIP_OPLLX  },
    {"OPZ",     false, FM_CHIP_OPZ    },
    {"VRC7",    false, FM_CHIP_VRC7   },
    // 外部ライブラリチップ
    {"PSG",     true,  FM_CHIP_EXT_PSG     },
    {"SN76489", true,  FM_CHIP_EXT_SN76489 },
    {"SCC",     true,  FM_CHIP_EXT_SCC     },
    {"SAA1099", true,  FM_CHIP_EXT_SAA1099 },
};

static const ChipEntry* findChipEntry(const std::string& name) {
    for (const auto& e : kChipTable)
        if (e.name == name) return &e;
    return nullptr;
}

// =========================================================
//  main
// =========================================================
int main(int argc, char* argv[]) {
    const char* jsonPath = "test_patches.json";
    if (argc >= 2) jsonPath = argv[1];

    // ① JSON 読み込み
    FILE* fp = fopen(jsonPath, "rb");
    if (!fp) {
        fprintf(stderr, "Cannot open %s\n", jsonPath);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    const long fileSize = ftell(fp);
    rewind(fp);
    std::string jsonStr(fileSize, '\0');
    fread(jsonStr.data(), 1, fileSize, fp);
    fclose(fp);

    json root;
    try {
        root = json::parse(jsonStr);
    } catch (const std::exception& e) {
        fprintf(stderr, "JSON parse error: %s\n", e.what());
        return 1;
    }

    const uint32_t DEFAULT_NOTE_MS = root.value("global", json{})
                                         .value("note_ms", 800u);
    const uint32_t DEFAULT_REST_MS = root.value("global", json{})
                                         .value("rest_ms", 200u);
    const uint32_t SAMPLE_RATE     = root.value("sample_rate", 48000u);

    printf("Patch file : %s\n", jsonPath);
    printf("Sample rate: %u Hz\n\n", SAMPLE_RATE);

    // ② エンジン作成
    FmEngineHandle eng = FmEngine_Create(SAMPLE_RATE);
    if (!eng) { fputs("FmEngine_Create failed\n", stderr); return 1; }

    // ③ デバイス選択
    const uint32_t deviceCount = Wasapi_GetDeviceCount();
    wchar_t selectedId[256] = {};
    for (uint32_t i = 0; i < deviceCount; ++i) {
        wchar_t id[256] = {}, name[256] = {};
        Wasapi_GetDeviceId(i, id, 256);
        Wasapi_GetDeviceName(i, name, 256);
        printf("[%u] %s%s\n", i, toMB(name).c_str(),
               Wasapi_IsDefaultDevice(i) ? " (default)" : "");
        if (Wasapi_IsDefaultDevice(i) && selectedId[0] == L'\0')
            wcscpy_s(selectedId, id);
    }
    printf("\n");

    WasapiHandle wasapi = (selectedId[0] != L'\0')
        ? Wasapi_CreateWithDevice(eng, 0, selectedId)
        : Wasapi_Create(eng, 0);
    if (!wasapi) { fputs("Wasapi_Create failed\n", stderr); FmEngine_Destroy(eng); return 1; }

    // ④ JSON の "chips" 以下を順に処理
    const auto& chips = root["chips"];
    for (auto it = chips.begin(); it != chips.end(); ++it) {
        const std::string chipName = it.key();
        const auto& chipDef = it.value();

        // チップを検索して追加
        const ChipEntry* entry = findChipEntry(chipName);
        if (!entry) {
            printf("[SKIP] %s : unknown chip type\n", chipName.c_str());
            continue;
        }

        uint32_t chip_id = 0;
        FmResult addResult;
        if (entry->isExt)
            addResult = FmEngine_AddExtChip(eng, (FmChipTypeExt)entry->type, 0, &chip_id);
        else
            addResult = FmEngine_AddChip(eng, (FmChipType)entry->type, 0, &chip_id);

        if (addResult != FM_OK) {
            printf("[SKIP] %s : AddChip failed (code=%d)\n", chipName.c_str(), (int)addResult);
            continue;
        }

        // ゲイン設定 (任意)
        const float gain = chipDef.value("gain", 1.0f);
        FmEngine_SetGain(eng, chip_id, gain, gain);

        // チップ全体の init レジスタ
        if (chipDef.contains("init"))
            applyRegs(eng, chip_id, chipDef["init"]);

        const uint32_t chipNative = FmEngine_GetNativeRate(eng, chip_id);
        printf("=== %s  [id=%u]  native=%u Hz ===\n",
               FmEngine_GetChipName(eng, chip_id), chip_id, chipNative);

        // ⑤ チャンネルを順番に鳴らす
        if (!chipDef.contains("channels") || !chipDef["channels"].is_array()) {
            printf("  (no channels defined)\n\n");
            continue;
        }

        const auto& channels = chipDef["channels"];

        // WASAPI 開始 (初回チップの初回チャンネルで開始)
        static bool wasapiStarted = false;
        if (!wasapiStarted) {
            check(Wasapi_Start(wasapi), "Wasapi_Start");
            wasapiStarted = true;
        }

        for (const auto& chDef : channels) {
            const int ch = chDef.value("ch", 0);
            const uint32_t note_ms = chDef.value("note_ms", DEFAULT_NOTE_MS);
            const uint32_t rest_ms = chDef.value("rest_ms", DEFAULT_REST_MS);
            const uint32_t chPort  = chDef.value("port", 0u);

            printf("  ch%-2d ", ch);
            fflush(stdout);

            // チャンネル init
            if (chDef.contains("init"))
                applyRegs(eng, chip_id, chDef["init"], chPort);

            // key_on
            if (chDef.contains("key_on"))
                applyRegs(eng, chip_id, chDef["key_on"], chPort);

            printf("ON  (%u ms) ", note_ms);
            fflush(stdout);
            Sleep(note_ms);

            // key_off
            if (chDef.contains("key_off"))
                applyRegs(eng, chip_id, chDef["key_off"], chPort);

            printf("OFF (%u ms)\n", rest_ms);
            fflush(stdout);
            Sleep(rest_ms);
        }
        printf("\n");
    }

    // ⑥ 停止・解放
    Wasapi_Stop(wasapi);
    Wasapi_Destroy(wasapi);
    FmEngine_Destroy(eng);
    printf("Done.\n");
    return 0;
}
