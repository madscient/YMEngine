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
//       "OPL2": { "gain": 1.0, "init": [...], "channels": [...] },
//       "OPNA": { "gain_l": 0.8, "gain_r": 1.0, "init": [...], "channels": [...] },
//       "OPM":  { "$ref": "opm.json" }
//     }
//   }
//
//   gain        : L/R 共通ゲイン (省略時 1.0)
//   gain_l/gain_r: 左右個別ゲイン (指定時 "gain" より優先)
//   "$ref"      : 他の JSON ファイル内の同名チップ定義を参照する。
//                 参照先パスは参照元ファイルからの相対パス。
//                 例: all.json の "OPM": {"$ref": "opm.json"} は
//                     opm.json 内の "chips"."OPM" を読み込んで使用する。
//                 これにより all.json は各チップ用 JSON への参照だけで構成できる。
//
// 注意: AddChip はオーディオストリーム開始前に全て完了させること。
//       (オーディオコールバックスレッドとの競合防止)

#include "FmEngineApi.h"
#include "RtAudio.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#ifdef _WIN32
#  include <windows.h>   // ExitProcess, Sleep, GetModuleFileNameA
#else
#  include <unistd.h>    // readlink
#  include <time.h>      // nanosleep
#endif
#include "nlohmann/json.hpp"

using json = nlohmann::json;

// =========================================================
//  RtAudio コールバック
//  RtAudio がオーディオバッファを必要とするたびに呼ばれる。
//  outputBuffer : インターリーブ float32 L/R
// =========================================================
struct AudioState {
    FmEngineHandle eng    = nullptr;
    uint32_t       frames = 0;   // コールバック 1 回あたりのフレーム数
    // 作業用の非インターリーブバッファ (コールバック内で使い回す)
    std::vector<float> workL;
    std::vector<float> workR;
};

static int rtAudioCallback(void* outBuf, void* /*inBuf*/,
                            unsigned int nFrames,
                            double /*streamTime*/,
                            RtAudioStreamStatus /*status*/,
                            void* userData) {
    auto* st = static_cast<AudioState*>(userData);
    if (!st->eng) return 0;

    // 作業バッファを必要なサイズに拡張
    if (st->workL.size() < nFrames) st->workL.resize(nFrames, 0.0f);
    if (st->workR.size() < nFrames) st->workR.resize(nFrames, 0.0f);

    FmEngine_Generate(st->eng, st->workL.data(), st->workR.data(), nFrames);

    // 非インターリーブ L/R → インターリーブ stereo
    auto* dst = static_cast<float*>(outBuf);
    for (unsigned int i = 0; i < nFrames; ++i) {
        dst[i * 2    ] = st->workL[i];
        dst[i * 2 + 1] = st->workR[i];
    }
    return 0;
}

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
#ifdef _WIN32
        ExitProcess(1);
#else
        exit(1);
#endif
    }
}

static void sleepMs(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts{ static_cast<time_t>(ms / 1000),
                        static_cast<long>((ms % 1000) * 1000000L) };
    nanosleep(&ts, nullptr);
#endif
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
//  ROM ファイルロード
//  実行ファイルと同じフォルダから検索する。
//  見つからなくても続行する (ROM が必要なチップは無音になる)。
// =========================================================

// 実行ファイルのフォルダを取得
static std::string getExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
#else
    // Linux/macOS: /proc/self/exe または argv[0] ベースのフォールバック
    char buf[4096] = {};
#  if defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0) len = 0;
    buf[len] = '\0';
#  else
    buf[0] = '\0';  // macOS 等は別途対応が必要
#  endif
    std::string path(buf);
#endif
    const auto pos = path.find_last_of("\\/");
    return (pos != std::string::npos) ? path.substr(0, pos + 1) : "";
}

// ROM ファイルをロードしてバッファに格納。失敗時は空を返す。
static std::vector<uint8_t> loadRomFile(const std::string& filename) {
    const std::string path = getExeDir() + filename;
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return {};
    fseek(fp, 0, SEEK_END);
    const long sz = ftell(fp);
    rewind(fp);
    if (sz <= 0) { fclose(fp); return {}; }
    std::vector<uint8_t> buf(sz);
    fread(buf.data(), 1, sz, fp);
    fclose(fp);
    return buf;
}

// チップ種別ごとの ROM 情報テーブル
struct RomEntry {
    std::string chipName;       // kChipTable の name と一致
    FmMemoryType memType;
    std::string filename;       // 実行ファイルと同じフォルダに置くファイル名
    std::string description;    // ログ用説明
};

static const RomEntry kRomTable[] = {
    // OPNA (YM2608): ADPCM-A rhythm ROM
    { "OPNA",  FM_MEM_ADPCM_A, "ym2608_adpcm_rom.bin", "YM2608 ADPCM-A ROM" },
    // OPNB (YM2610): ADPCM-A + ADPCM-B ROM
    { "OPNB",  FM_MEM_ADPCM_A, "ym2610.rom", "YM2610 ADPCM-A ROM" },
    { "OPNB",  FM_MEM_ADPCM_B, "ym2610b.rom","YM2610 ADPCM-B ROM" },
    { "OPNBB", FM_MEM_ADPCM_A, "ym2610.rom", "YM2610 ADPCM-A ROM" },
    { "OPNBB", FM_MEM_ADPCM_B, "ym2610b.rom","YM2610 ADPCM-B ROM" },
};





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

// basePath と同じディレクトリを基準に相対パス refPath を解決する
static std::string resolveRefPath(const std::string& basePath, const std::string& refPath) {
    const auto pos = basePath.find_last_of("\\/");
    if (pos == std::string::npos) return refPath;
    return basePath.substr(0, pos + 1) + refPath;
}

// "chips" オブジェクト内の各チップ定義について
// {"$ref": "other.json"} 形式の参照を解決し、参照先ファイル内の
// "chips".<同名チップ> の定義に置き換える。
// 参照解決の循環防止のため visitedPaths に自分自身のパスを積んでから呼ぶこと。
static void resolveChipRefs(json& chips, const std::string& currentPath,
                             std::vector<std::string>& visitedPaths,
                             int depth = 0) {
    if (depth > 8) {
        fprintf(stderr, "  [WARN] $ref nesting too deep (>8), aborting resolution\n");
        return;
    }
    for (auto it = chips.begin(); it != chips.end(); ++it) {
        const std::string chipName = it.key();
        json& chipDef = it.value();
        if (!chipDef.is_object() || !chipDef.contains("$ref")) continue;

        const std::string refRelPath = chipDef["$ref"].get<std::string>();
        const std::string refFullPath = resolveRefPath(currentPath, refRelPath);

        // 循環参照チェック
        bool cyclic = false;
        for (const auto& p : visitedPaths) {
            if (p == refFullPath) { cyclic = true; break; }
        }
        if (cyclic) {
            fprintf(stderr, "  [WARN] %s: circular $ref to %s, skipping\n",
                    chipName.c_str(), refFullPath.c_str());
            chipDef = json::object();
            continue;
        }

        json refRoot;
        if (!loadJson(refFullPath.c_str(), refRoot)) {
            fprintf(stderr, "  [WARN] %s: failed to load $ref %s\n",
                    chipName.c_str(), refFullPath.c_str());
            chipDef = json::object();
            continue;
        }
        if (!refRoot.contains("chips") || !refRoot["chips"].contains(chipName)) {
            fprintf(stderr, "  [WARN] %s: $ref %s has no chips.\"%s\"\n",
                    chipName.c_str(), refFullPath.c_str(), chipName.c_str());
            chipDef = json::object();
            continue;
        }

        // 参照先がさらに $ref を含む場合に備えて再帰的に解決
        visitedPaths.push_back(refFullPath);
        resolveChipRefs(refRoot["chips"], refFullPath, visitedPaths, depth + 1);
        visitedPaths.pop_back();

        chipDef = refRoot["chips"][chipName];
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
    // ROM バッファ: エンジンが参照している間 (プロセス終了まで) 保持する
    std::vector<std::vector<uint8_t>> romBuffers;
};

static void addChipsFromFile(FileContext& ctx, FmEngineHandle eng) {
    if (!ctx.valid) return;
    if (!ctx.root.contains("chips") || !ctx.root["chips"].is_object()) return;

    const auto& chips = ctx.root["chips"];
    ctx.slots.reserve(chips.size());

    for (auto it = chips.begin(); it != chips.end(); ++it) {
        const std::string chipName = it.key();
        const auto& chipDef = it.value();

        uint32_t chip_id = 0;
        const FmResult res = FmEngine_AddChip(eng, chipName.c_str(), 0, &chip_id);
        if (res == FM_ERR_UNKNOWN_CHIP) {
            printf("  [SKIP] %s : unknown chip type\n", chipName.c_str());
            ctx.slots.push_back({0, false});
            continue;
        }
        if (res != FM_OK) {
            printf("  [SKIP] %s : AddChip failed (code=%d)\n", chipName.c_str(), (int)res);
            ctx.slots.push_back({0, false});
            continue;
        }

        // gain: 単一値 "gain" (L/R 共通、後方互換) または
        //       "gain_l" / "gain_r" で左右個別に指定可能。
        //       両方ある場合は gain_l/gain_r が優先。
        {
            const float baseGain = chipDef.value("gain", 1.0f);
            const float gainL = chipDef.value("gain_l", baseGain);
            const float gainR = chipDef.value("gain_r", baseGain);
            FmEngine_SetGain(eng, chip_id, gainL, gainR);
        }
        // ROM が必要なチップは実行ファイルと同じフォルダから読み込む
        for (const auto& entry : kRomTable) {
            if (entry.chipName != chipName) continue;
            ctx.romBuffers.push_back(loadRomFile(entry.filename));
            const auto& rom = ctx.romBuffers.back();
            if (rom.empty()) {
                printf("    [ROM] %s: not found (%s) — ADPCM will be silent\n",
                       entry.description.c_str(), entry.filename.c_str());
                continue;
            }
            const FmResult res = FmEngine_SetMemory(
                eng, chip_id, entry.memType, rom.data(), (uint32_t)rom.size());
            if (res == FM_OK)
                printf("    [ROM] %s: loaded %zu bytes\n",
                       entry.description.c_str(), rom.size());
            else
                printf("    [ROM] %s: SetMemory failed (code=%d)\n",
                       entry.description.c_str(), (int)res);
        }
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

            sleepMs(note_ms);

            if (chDef.contains("key_off"))
                applyRegs(eng, chip_id, chDef["key_off"], chPort);

            sleepMs(rest_ms);
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

    // 対応チップ一覧を表示
    {
        const uint32_t n = FmEngine_Inquiry(eng);
        printf("Supported chips (%u):", n);
        for (uint32_t i = 0; i < n; ++i)
            printf(" %s", FmEngine_GetSupportedChip(eng, i));
        printf("\n\n");
    }

    // ② RtAudio 初期化・デバイス列挙
    RtAudio audio;
    if (audio.getDeviceCount() == 0) {
        fputs("RtAudio: no audio devices found\n", stderr);
        FmEngine_Destroy(eng);
        return 1;
    }
    {
        std::vector<unsigned int> ids = audio.getDeviceIds();
        unsigned int defaultId = audio.getDefaultOutputDevice();
        for (unsigned int id : ids) {
            RtAudio::DeviceInfo info = audio.getDeviceInfo(id);
            if (info.outputChannels < 2) continue;
            const bool isDef = (id == defaultId);
            printf("  [%u] %s%s\n", id, info.name.c_str(), isDef ? " (default)" : "");
        }
        printf("\n");
    }

    // デバイス選択: -d で部分一致、なければデフォルト
    unsigned int selectedId = audio.getDefaultOutputDevice();
    if (selectedId == 0) {
        // getDefaultOutputDevice() が 0 を返す場合は最初の出力デバイスを使う
        for (unsigned int id : audio.getDeviceIds()) {
            if (audio.getDeviceInfo(id).outputChannels >= 2) {
                selectedId = id;
                break;
            }
        }
    }
    if (deviceName) {
        for (unsigned int id : audio.getDeviceIds()) {
            RtAudio::DeviceInfo info = audio.getDeviceInfo(id);
            if (info.outputChannels >= 2 &&
                info.name.find(deviceName) != std::string::npos) {
                selectedId = id;
                break;
            }
        }
    }
    printf("Using device id=%u\n\n", selectedId);

    // ③ 全ファイルを読み込み、全チップを先に追加 (ストリーム開始前)
    std::vector<FileContext> ctxs(jsonFiles.size());
    for (size_t i = 0; i < jsonFiles.size(); ++i) {
        ctxs[i].path  = jsonFiles[i];
        ctxs[i].valid = loadJson(jsonFiles[i], ctxs[i].root);
        if (ctxs[i].valid && ctxs[i].root.contains("chips") &&
            ctxs[i].root["chips"].is_object()) {
            std::vector<std::string> visited{ ctxs[i].path };
            resolveChipRefs(ctxs[i].root["chips"], ctxs[i].path, visited);
        }
        addChipsFromFile(ctxs[i], eng);
    }

    // ④ RtAudio ストリーム開始 (全 AddChip 完了後)
    AudioState audioState;
    audioState.eng = eng;

    RtAudio::StreamParameters outParams;
    outParams.deviceId     = selectedId;
    outParams.nChannels    = 2;
    outParams.firstChannel = 0;

    unsigned int bufferFrames = 512;

    printf("Opening stream (device=%u, rate=%u, frames=%u)...\n",
           selectedId, sampleRate, bufferFrames);
    fflush(stdout);

    RtAudioErrorType err = audio.openStream(
        &outParams, nullptr,
        RTAUDIO_FLOAT32, sampleRate,
        &bufferFrames,
        rtAudioCallback, &audioState);
    if (err != RTAUDIO_NO_ERROR) {
        fprintf(stderr, "RtAudio::openStream failed: %s\n", audio.getErrorText().c_str());
        FmEngine_Destroy(eng);
        return 1;
    }
    printf("Stream opened (bufferFrames=%u)\n", bufferFrames);
    fflush(stdout);

    err = audio.startStream();
    if (err != RTAUDIO_NO_ERROR) {
        fprintf(stderr, "RtAudio::startStream failed: %s\n", audio.getErrorText().c_str());
        audio.closeStream();
        FmEngine_Destroy(eng);
        return 1;
    }
    printf("Stream started\n\n");
    fflush(stdout);

    // ⑤ 各ファイルを発音テスト
    for (const auto& ctx : ctxs)
        playChipsFromFile(ctx, eng);

    // ⑥ 停止・解放
    if (audio.isStreamRunning()) audio.stopStream();
    if (audio.isStreamOpen())    audio.closeStream();
    FmEngine_Destroy(eng);
    printf("Done.\n");
    return 0;
}
