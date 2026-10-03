// FmEngineApi.cpp
// FmEngineApi.h で宣言した C ファサードの実装。
// このファイルだけが FmEngine の C++ ヘッダを include する。
// DLL 境界をまたぐのは POD 型と不透明ポインタだけ。

#define FMENGINE_EXPORTS
#include "FmEngineApi.h"
#include "FmEngine.h"

#include <new>
#include <stdexcept>

// FmMemoryAccess は ChipMemoryAccess にそのままキャストして渡すので、番号を揃えておく
static_assert(FM_ACCESS_ROM == static_cast<int>(ChipMemoryAccess::ROM), "FM_ACCESS_ROM");
static_assert(FM_ACCESS_RAM == static_cast<int>(ChipMemoryAccess::RAM), "FM_ACCESS_RAM");

// =========================================================
//  内部構造体 (ハンドルの実体)
// =========================================================
struct FmEngineOpaque {
    FmEngine engine;
    explicit FmEngineOpaque(uint32_t sr) : engine(sr) {}
};

// =========================================================
//  ヘルパー
// =========================================================
#define REQUIRE_PTR(h)  if (!(h)) return FM_ERR_INVALID_ARG

// 例外を FM_ERR_* に変換するラッパー
template<typename Fn>
static FmResult safeCall(Fn&& fn) {
    try { fn(); return FM_OK; }
    catch (const std::invalid_argument&) { return FM_ERR_INVALID_ARG; }
    catch (const std::bad_alloc&)        { return FM_ERR_ALLOC; }
    catch (...)                          { return FM_ERR_UNAVAILABLE; }
}

// =========================================================
//  エンジン生成・破棄
// =========================================================
FMENGINE_API FmEngineHandle FMENGINE_CALL
FmEngine_Create(uint32_t sample_rate) {
    return new(std::nothrow) FmEngineOpaque(sample_rate);
}

FMENGINE_API void FMENGINE_CALL
FmEngine_Destroy(FmEngineHandle h) {
    delete static_cast<FmEngineOpaque*>(h);
}

// =========================================================
//  対応チップ問い合わせ
// =========================================================
FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_Inquiry(FmEngineHandle h) {
    if (!h) return 0;
    return static_cast<FmEngineOpaque*>(h)->engine.supportedChipCount();
}

FMENGINE_API const char* FMENGINE_CALL
FmEngine_GetSupportedChip(FmEngineHandle h, uint32_t index) {
    if (!h) return nullptr;
    return static_cast<FmEngineOpaque*>(h)->engine.supportedChipName(index);
}

// =========================================================
//  チップ追加
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_AddChip(FmEngineHandle h, const char* name,
                 uint32_t clock, uint32_t* out_id) {
    REQUIRE_PTR(h);
    if (!name || !out_id || clock == 0) return FM_ERR_INVALID_ARG;
    uint32_t id = UINT32_MAX;
    const FmResult r = safeCall([&] {
        id = static_cast<FmEngineOpaque*>(h)->engine.addChipByName(name, clock);
    });
    if (r != FM_OK) return r;
    if (id == UINT32_MAX) return FM_ERR_UNKNOWN_CHIP;
    *out_id = id;
    return FM_OK;
}

// =========================================================
//  チップ情報取得
// =========================================================
FMENGINE_API const char* FMENGINE_CALL
FmEngine_GetChipName(FmEngineHandle h, uint32_t chip_id) {
    if (!h) return nullptr;
    return static_cast<FmEngineOpaque*>(h)->engine.getChipName(chip_id);
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetNativeRate(FmEngineHandle h, uint32_t chip_id) {
    if (!h) return 0;
    return static_cast<FmEngineOpaque*>(h)->engine.nativeRate(chip_id);
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetSampleRate(FmEngineHandle h) {
    if (!h) return 0;
    return static_cast<FmEngineOpaque*>(h)->engine.sampleRate();
}

// =========================================================
//  レジスタ書き込み
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_Write(FmEngineHandle h, uint32_t chip_id,
               uint8_t reg, uint8_t value, uint32_t port) {
    REQUIRE_PTR(h);
    return safeCall([&] {
        static_cast<FmEngineOpaque*>(h)->engine.write(chip_id, reg, value, port);
    });
}

// =========================================================
//  ゲイン
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetGain(FmEngineHandle h, uint32_t chip_id,
                 float gain_l, float gain_r) {
    REQUIRE_PTR(h);
    static_cast<FmEngineOpaque*>(h)->engine.setGain(chip_id, gain_l, gain_r);
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_GetGain(FmEngineHandle h, uint32_t chip_id,
                 float* out_gain_l, float* out_gain_r) {
    REQUIRE_PTR(h);
    if (!out_gain_l || !out_gain_r) return FM_ERR_INVALID_ARG;
    static_cast<FmEngineOpaque*>(h)->engine.getGain(chip_id, *out_gain_l, *out_gain_r);
    return FM_OK;
}

// =========================================================
//  部位ごとのゲイン
// =========================================================
FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetPartCount(FmEngineHandle h, uint32_t chip_id) {
    if (!h) return 0;
    return static_cast<FmEngineOpaque*>(h)->engine.partCount(chip_id);
}

FMENGINE_API const char* FMENGINE_CALL
FmEngine_GetPartName(FmEngineHandle h, uint32_t chip_id, uint32_t index) {
    if (!h) return nullptr;
    return static_cast<FmEngineOpaque*>(h)->engine.partName(chip_id, index);
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetPartGain(FmEngineHandle h, uint32_t chip_id, const char* part,
                     float gain_l, float gain_r) {
    REQUIRE_PTR(h);
    FmEngine& engine = static_cast<FmEngineOpaque*>(h)->engine;
    ChipPart p;
    const bool ok = engine.findPart(chip_id, part, p) &&
                    engine.setPartGain(chip_id, p, gain_l, gain_r);
    return ok ? FM_OK : FM_ERR_INVALID_ARG;
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_GetPartGain(FmEngineHandle h, uint32_t chip_id, const char* part,
                     float* out_gain_l, float* out_gain_r) {
    REQUIRE_PTR(h);
    if (!out_gain_l || !out_gain_r) return FM_ERR_INVALID_ARG;
    const FmEngine& engine = static_cast<FmEngineOpaque*>(h)->engine;
    ChipPart p;
    const bool ok = engine.findPart(chip_id, part, p) &&
                    engine.getPartGain(chip_id, p, *out_gain_l, *out_gain_r);
    return ok ? FM_OK : FM_ERR_INVALID_ARG;
}

// =========================================================
//  外部メモリ
// =========================================================
FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetMemoryCount(FmEngineHandle h, uint32_t chip_id) {
    if (!h) return 0;
    return static_cast<FmEngineOpaque*>(h)->engine.memoryCount(chip_id);
}

FMENGINE_API const char* FMENGINE_CALL
FmEngine_GetMemoryName(FmEngineHandle h, uint32_t chip_id, uint32_t index) {
    if (!h) return nullptr;
    return static_cast<FmEngineOpaque*>(h)->engine.memoryName(chip_id, index);
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetMemory(FmEngineHandle h, uint32_t chip_id,
                   const char* memory,
                   const uint8_t* data, uint32_t size) {
    REQUIRE_PTR(h);
    bool ok = false;
    const FmResult r = safeCall([&] {
        FmEngine& engine = static_cast<FmEngineOpaque*>(h)->engine;
        ChipMemoryType type;
        ok = engine.findMemory(chip_id, memory, type) &&
             engine.setMemory(chip_id, type, data, size);
    });
    if (r != FM_OK) return r;
    return ok ? FM_OK : FM_ERR_INVALID_ARG;
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetMemoryEx(FmEngineHandle h, uint32_t chip_id,
                     const char* memory, uint32_t base,
                     uint8_t* data, uint32_t size, FmMemoryAccess access) {
    REQUIRE_PTR(h);
    bool ok = false;
    const FmResult r = safeCall([&] {
        FmEngine& engine = static_cast<FmEngineOpaque*>(h)->engine;
        ChipMemoryType type;
        ok = engine.findMemory(chip_id, memory, type) &&
             engine.mapMemory(chip_id, type, base, data, size,
                              static_cast<ChipMemoryAccess>(access));
    });
    if (r != FM_OK) return r;
    return ok ? FM_OK : FM_ERR_INVALID_ARG;
}

// =========================================================
//  波形生成
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_Generate(FmEngineHandle h, float* out_l, float* out_r, uint32_t samples) {
    REQUIRE_PTR(h);
    if (!out_l || !out_r || samples == 0) return FM_ERR_INVALID_ARG;
    static_cast<FmEngineOpaque*>(h)->engine.generate(out_l, out_r, samples);
    return FM_OK;
}
