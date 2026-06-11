// main.cpp
// YMEngine チップテスト
//
// 使い方:
//   sample_app.exe [オプション] [file1.json] [file2.json] ...
//
//   引数なし         : patches/all.json を使用
//   JSON ファイル指定 : 指定したファイルを順に処理 (複数指定可)
//   -r <rate>        : サンプルレートを指定 (省略時 48000)
//   -d <name>        : デバイス名を部分一致で指定 (省略時デフォルトデバイス)
//
// JSON フォーマット:
//   {
//     "sample_rate": 48000,
//     "global": { "note_ms": 800, "rest_ms": 200 },
//     "chips": {
//       "OPL2": { "gain": 1.0, "init": [...], "channels": [...] }
//     }
//   }
//
// 注意: AddChip は Wasapi_Start より前に全て完了させること。
//       (WASAPI スレッドとの競合防止)

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

static uint32_t parseVal(const json& j) {
    if (j.is_string())
        return static_cast<uint32_t>(std::stoul(j.get<std::string>(), nullptr, 0));
    return j.get<uint32_t>();
}

static void check(FmResult r, const char* msg) {
    if (r != FM_OK) {
        fprintf(stderr, "ERROR %s: code=%d\n", msg, (int)r);
        ExitProcess(1);
    }
}

static std::string toMB(const wchar_t* wstr) {
    if (!wstr || wstr[0] == L'\0') return {};
    const UINT cp = GetACP();
    const int  n  = WideCharToMultiByte(cp, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string buf(n, '\0');
    WideCharToMultiByte(cp, 0, wstr, -1, buf.data(), n, nullptr, nullptr);
    buf.resize(n - 1);
    return buf;
}

static void applyRegs(FmEngineHandle eng, uint32_t chip_id,
                      const json& regs, uint32_t default_port = 0)
{
    if (!regs.is_array()) return;
    for (const auto& r : regs) {
        const uint32_t reg  = parseVal(r["reg"]);
        const uint32_t val  = parseVal(r["val"]);
        const uint32_t port = r.contains("port") ? parseVal(r["port"]) : default_port;
        FmEngine_Write(eng, chip_id,
                       static_cast<uint8_t>(reg),
                       static_cast<uint8_t>(val), port);
    }
}

// =========================================================
//  チップ種別テーブル
// =========================================================
struct ChipEntry { std::string name; bool isExt; int type; };

static const ChipEntry kChipTable[] = {
    {"Y8950",  false, FM_CHIP_Y8950 }, {"OPL",   false, FM_CHIP_OPL   },
    {"OPL2",   false, FM_CHIP_OPL2  }, {"OPL3",  false, FM_CHIP_OPL3  },
    {"OPL4",   false, FM_CHIP_OPL4  }, {"OPN",   false, FM_CHIP_OPN   },
    {"OPNA",   false, FM_CHIP_OPNA  }, {"OPNB",  false, FM_CHIP_OPNB  },
    {"OPNBB",  false, FM_CHIP_OPNBB }, {"OPN2",  false, FM_CHIP_OPN2  },
    {"OPM",    false, FM_CHIP_OPM   }, {"OPLL",  false, FM_CHIP_OPLL  },
    {"OPLLP",  false, FM_CHIP_OPLLP }, {"OPLLX", false, FM_CHIP_OPLLX },
    {"OPZ",    false, FM_CHIP_OPZ   }, {"VRC7",  false, FM_CHIP_VRC7  },
    {"SSG",    true,  FM_CHIP_EXT_SSG  }, {"DCSG", true, FM_CHIP_EXT_DCSG },
    {"SCC",    true,  FM_CHIP_EXT_SCC  }, {"SAA",  true, FM_CHIP_EXT_SAA  },
};

static const ChipEntry* findChipEntry(const std::string& name) {
    for (const auto& e : kChipTable)
        if (e.name == name) return &e;
    return nullptr;
}

// =========================================================
//  JSON ファイル読み込み
// =========================================================
static bool loadJson(const char* path, json& out) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open: %s\n", path); return false; }
    fseek(fp, 0, SEEK_END);
    const long sz = ftell(fp); rewind(fp);
    std::string buf(sz, '\0');
    fread(buf.data(), 1, sz, fp);
    fclose(fp);
    try   { out = json::parse(buf); return true; }
    catch (const std::exception& e) {
        fprintf(stderr, "JSON parse error (%s): %s\n", path, e.what());
        return false;
    }
}

// =========================================================
//  1 ファイル処理 (チップ追加フェーズ)
//  WASAPI_Start より前に呼ぶこと。
// =========================================================
struct FileContext {
    json     root;
    std::string path;
    bool     valid = false;
    struct Slot { uint32_t id = 0; bool valid = false; };
    std::vector<Slot> slots;
};

static void addChipsFromFile(FileContext& ctx, FmEngineHandle eng) {
    if (!ctx.valid) return;
    if (!ctx.root.contains("chips") || !ctx.root["chips"].is_object()) return;

    const auto& chips = ctx.root["chips"];
    ctx.slots.reserve(chips.size());

    for (auto it = chips.begin(); it != chips.end(); ++it) {
        const std::string chipName = it.key();
        const auto& chipDef = it.value();
        const ChipEntry* entry = findChipEntry(chipName);

        if (!entry) {
            printf("  [SKIP] %s : unknown chip type\n", chipName.c_str());
            ctx.slots.push_back({0, false});
            continue;
        }

        uint32_t chip_id = 0;
        FmResult res;
        if (entry->isExt)
            res = FmEngine_AddExtChip(eng, (FmChipTypeExt)entry->type, 0, &chip_id);
        else
            res = FmEngine_AddChip(eng, (FmChipType)entry->type, 0, &chip_id);

        if (res != FM_OK) {
            printf("  [SKIP] %s : AddChip failed (code=%d)\n", chipName.c_str(), (int)res);
            ctx.slots.push_back({0, false});
            continue;
        }

        FmEngine_SetGain(eng, chip_id,
                         chipDef.value("gain", 1.0f),
                         chipDef.value("gain", 1.0f));
        ctx.slots.push_back({chip_id, true});
    }
}

// =========================================================
//  1 ファイル処理 (発音フェーズ)
//  WASAPI_Start 後に呼ぶこと。
// =========================================================
static void playChipsFromFile(const FileContext& ctx, FmEngineHandle eng) {
    if (!ctx.valid) return;
    if (!ctx.root.contains("chips")) return;

    const uint32_t NOTE_MS = ctx.root.value("global", json{}).value("note_ms", 800u);
    const uint32_t REST_MS = ctx.root.value("global", json{}).value("rest_ms", 200u);

    printf("=== %s ===\n", ctx.path.c_str());

    const auto& chips = ctx.root["chips"];
    uint32_t slotIdx = 0;
    for (auto it = chips.begin(); it != chips.end(); ++it, ++slotIdx) {
        if (!ctx.slots[slotIdx].valid) continue;

        const std::string chipName = it.key();
        const auto&  chipDef = it.value();
        const uint32_t chip_id = ctx.slots[slotIdx].id;

        if (chipDef.contains("init"))
            applyRegs(eng, chip_id, chipDef["init"]);

        printf("  %s [id=%u]  native=%u Hz\n",
               FmEngine_GetChipName(eng, chip_id),
               chip_id, FmEngine_GetNativeRate(eng, chip_id));

        if (!chipDef.contains("channels") || !chipDef["channels"].is_array()) {
            printf("    (no channels)\n");
            continue;
        }

        for (const auto& chDef : chipDef["channels"]) {
            const int      ch      = chDef.value("ch", 0);
            const uint32_t note_ms = chDef.value("note_ms", NOTE_MS);
            const uint32_t rest_ms = chDef.value("rest_ms", REST_MS);
            const uint32_t chPort  = chDef.value("port", 0u);
            const std::string cmt  = chDef.value("_comment", "");

            printf("    ch%-2d %s\n", ch, cmt.c_str());
            fflush(stdout);

            if (chDef.contains("init"))
                applyRegs(eng, chip_id, chDef["init"], chPort);
            if (chDef.contains("key_on"))
                applyRegs(eng, chip_id, chDef["key_on"], chPort);

            Sleep(note_ms);

            if (chDef.contains("key_off"))
                applyRegs(eng, chip_id, chDef["key_off"], chPort);

            Sleep(rest_ms);
        }
    }
    printf("\n");
}

// =========================================================
//  main
// =========================================================
int main(int argc, char* argv[]) {

    // 引数解析
    std::vector<const char*> jsonFiles;
    uint32_t    sampleRate = 48000;
    const char* deviceName = nullptr;

    for (int i = 1; i < argc; ++i) {
        if      (strcmp(argv[i], "-r") == 0 && i+1 < argc) sampleRate = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) deviceName = argv[++i];
        else                                                jsonFiles.push_back(argv[i]);
    }
    if (jsonFiles.empty())
        jsonFiles.push_back("patches/all.json");

    printf("YMEngine Test\n");
    printf("Sample rate: %u Hz\n\n", sampleRate);

    // ① エンジン作成
    FmEngineHandle eng = FmEngine_Create(sampleRate);
    if (!eng) { fputs("FmEngine_Create failed\n", stderr); return 1; }

    // ② デバイス列挙・選択
    const uint32_t devCount = Wasapi_GetDeviceCount();
    wchar_t selectedId[256] = {};

    for (uint32_t i = 0; i < devCount; ++i) {
        wchar_t id[256] = {}, name[256] = {};
        Wasapi_GetDeviceId(i, id, 256);
        Wasapi_GetDeviceName(i, name, 256);
        const bool isDef = Wasapi_IsDefaultDevice(i) != 0;
        const std::string mb = toMB(name);
        printf("  [%u] %s%s\n", i, mb.c_str(), isDef ? " (default)" : "");

        if (deviceName) {
            if (mb.find(deviceName) != std::string::npos)
                wcscpy_s(selectedId, id);
        } else if (isDef && selectedId[0] == L'\0') {
            wcscpy_s(selectedId, id);
        }
    }
    printf("\n");

    WasapiHandle wasapi = (selectedId[0] != L'\0')
        ? Wasapi_CreateWithDevice(eng, 0, selectedId)
        : Wasapi_Create(eng, 0);
    if (!wasapi) {
        fputs("Wasapi_Create failed\n", stderr);
        FmEngine_Destroy(eng);
        return 1;
    }

    // ③ 全ファイルを読み込み、全チップを先に追加 (WASAPI 開始前)
    std::vector<FileContext> ctxs(jsonFiles.size());
    for (size_t i = 0; i < jsonFiles.size(); ++i) {
        ctxs[i].path  = jsonFiles[i];
        ctxs[i].valid = loadJson(jsonFiles[i], ctxs[i].root);
        addChipsFromFile(ctxs[i], eng);
    }

    // ④ WASAPI 開始 (全 AddChip 完了後)
    check(Wasapi_Start(wasapi), "Wasapi_Start");

    // ⑤ 各ファイルを発音テスト
    for (const auto& ctx : ctxs)
        playChipsFromFile(ctx, eng);

    // ⑥ 停止・解放
    Wasapi_Stop(wasapi);
    Wasapi_Destroy(wasapi);
    FmEngine_Destroy(eng);
    printf("Done.\n");
    return 0;
}
