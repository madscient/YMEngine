// FmEngineApi.cpp
// FmEngineApi.h で宣言した C ファサードの実装。
// このファイルだけが FmEngine の C++ ヘッダを include する。
// DLL 境界をまたぐのは POD 型と不透明ポインタだけ。

#define FMENGINE_EXPORTS
#include "FmEngineApi.h"
#include "FmEngine.h"

#include <new>
#include <stdexcept>

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
    if (!name || !out_id) return FM_ERR_INVALID_ARG;
    auto& eng = static_cast<FmEngineOpaque*>(h)->engine;
    const uint32_t id = eng.addChipByName(name, clock);
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
//  外部メモリ
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetMemory(FmEngineHandle h, uint32_t chip_id,
                   FmMemoryType mem_type,
                   const uint8_t* data, uint32_t size) {
    REQUIRE_PTR(h);
    if (!data || size == 0) return FM_ERR_INVALID_ARG;
    return safeCall([&] {
        static_cast<FmEngineOpaque*>(h)->engine.setMemory(
            chip_id, static_cast<int>(mem_type), data, size);
    });
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetMemorySize(FmEngineHandle h, uint32_t chip_id, FmMemoryType mem_type) {
    if (!h) return 0;
    return static_cast<FmEngineOpaque*>(h)->engine.getMemorySize(
        chip_id, static_cast<int>(mem_type));
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
