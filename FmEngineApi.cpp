// FmEngineApi.cpp
// FmEngineApi.h で宣言した C ファサードの実装。
// このファイルだけが FmEngine / WasapiOutput の C++ ヘッダを include する。
// DLL 境界をまたぐのは POD 型と不透明ポインタだけ。

#define FMENGINE_EXPORTS  // dllexport として定義
#include "FmEngineApi.h"

#include "FmEngine.h"
#include "WasapiOutput.h"

#include <new>       // std::nothrow
#include <stdexcept>
#include <cstring>

// =========================================================
//  内部構造体 (ハンドルの実体)
// =========================================================
struct FmEngineOpaque {
    FmEngine engine;

    explicit FmEngineOpaque(uint32_t sr) : engine(sr) {}
};

struct WasapiOpaque {
    WasapiOutput output;

    WasapiOpaque(FmEngine& e, bool excl) : output(e, excl) {}
};

// =========================================================
//  例外を FmResult に変換するヘルパー
// =========================================================
template<typename Fn>
static FmResult safeCall(Fn&& fn) noexcept {
    try {
        fn();
        return FM_OK;
    } catch (const std::invalid_argument&) {
        return FM_ERR_INVALID_ARG;
    } catch (const std::runtime_error&) {
        return FM_ERR_AUDIO;
    } catch (...) {
        return FM_ERR_EXCEPTION;
    }
}

// =========================================================
//  引数チェックヘルパー
// =========================================================
#define REQUIRE(cond) do { if (!(cond)) return FM_ERR_INVALID_ARG; } while(0)
#define REQUIRE_PTR(p) REQUIRE((p) != nullptr)

// =========================================================
//  FmEngine API 実装
// =========================================================

FMENGINE_API FmEngineHandle FMENGINE_CALL
FmEngine_Create(uint32_t sample_rate) {
    auto* p = new(std::nothrow) FmEngineOpaque(sample_rate);
    return static_cast<FmEngineHandle>(p);
}

FMENGINE_API void FMENGINE_CALL
FmEngine_Destroy(FmEngineHandle engine) {
    delete static_cast<FmEngineOpaque*>(engine);
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_AddChip(FmEngineHandle h, FmChipType type, uint32_t clock,
                 uint32_t* out_id) {
    REQUIRE_PTR(h);
    REQUIRE_PTR(out_id);
    return safeCall([&] {
        auto& eng = static_cast<FmEngineOpaque*>(h)->engine;
        *out_id = eng.addChip(static_cast<ChipType>(type), clock);
    });
}

FMENGINE_API const char* FMENGINE_CALL
FmEngine_GetChipName(FmEngineHandle h, uint32_t chip_id) {
    if (!h) return nullptr;
    auto& eng = static_cast<FmEngineOpaque*>(h)->engine;
    const auto* c = eng.chip(chip_id);
    return c ? c->name() : nullptr;
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetNativeRate(FmEngineHandle h, uint32_t chip_id) {
    if (!h) return 0;
    auto& eng = static_cast<FmEngineOpaque*>(h)->engine;
    const auto* c = eng.chip(chip_id);
    return c ? c->nativeRate() : 0;
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetSampleRate(FmEngineHandle h) {
    if (!h) return 0;
    return static_cast<FmEngineOpaque*>(h)->engine.sampleRate();
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_Write(FmEngineHandle h, uint32_t chip_id,
               uint8_t reg, uint8_t value, uint32_t port) {
    REQUIRE_PTR(h);
    return safeCall([&] {
        static_cast<FmEngineOpaque*>(h)->engine.write(chip_id, reg, value, port);
    });
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetGain(FmEngineHandle h, uint32_t chip_id,
                 float gain_l, float gain_r) {
    REQUIRE_PTR(h);
    return safeCall([&] {
        static_cast<FmEngineOpaque*>(h)->engine.setGain(chip_id, gain_l, gain_r);
    });
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_GetGain(FmEngineHandle h, uint32_t chip_id,
                 float* out_gain_l, float* out_gain_r) {
    REQUIRE_PTR(h);
    REQUIRE_PTR(out_gain_l);
    REQUIRE_PTR(out_gain_r);
    return safeCall([&] {
        auto& eng = static_cast<FmEngineOpaque*>(h)->engine;
        *out_gain_l = eng.getGainL(chip_id);
        *out_gain_r = eng.getGainR(chip_id);
    });
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_Generate(FmEngineHandle h,
                  float* out_l, float* out_r, uint32_t samples) {
    REQUIRE_PTR(h);
    REQUIRE_PTR(out_l);
    REQUIRE_PTR(out_r);
    return safeCall([&] {
        static_cast<FmEngineOpaque*>(h)->engine.generate(out_l, out_r, samples);
    });
}

// =========================================================
//  WasapiOutput API 実装
// =========================================================

FMENGINE_API WasapiHandle FMENGINE_CALL
Wasapi_Create(FmEngineHandle h, int exclusive) {
    if (!h) return nullptr;
    auto& eng = static_cast<FmEngineOpaque*>(h)->engine;
    auto* p = new(std::nothrow) WasapiOpaque(eng, exclusive != 0);
    return static_cast<WasapiHandle>(p);
}

FMENGINE_API void FMENGINE_CALL
Wasapi_Destroy(WasapiHandle h) {
    delete static_cast<WasapiOpaque*>(h);
}

FMENGINE_API FmResult FMENGINE_CALL
Wasapi_Start(WasapiHandle h) {
    REQUIRE_PTR(h);
    return safeCall([&] {
        static_cast<WasapiOpaque*>(h)->output.start();
    });
}

FMENGINE_API FmResult FMENGINE_CALL
Wasapi_Stop(WasapiHandle h) {
    REQUIRE_PTR(h);
    return safeCall([&] {
        static_cast<WasapiOpaque*>(h)->output.stop();
    });
}
