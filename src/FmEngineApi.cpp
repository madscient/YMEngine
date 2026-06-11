// FmEngineApi.cpp
// FmEngineApi.h で宣言した C ファサードの実装。
// このファイルだけが FmEngine / WasapiOutput の C++ ヘッダを include する。
// DLL 境界をまたぐのは POD 型と不透明ポインタだけ。

#define FMENGINE_EXPORTS  // dllexport として定義
#include "FmEngineApi.h"

#include "FmEngine.h"
#include "WasapiOutput.h"

#include <new>
#include <stdexcept>

// =========================================================
//  内部構造体 (ハンドルの実体)
// =========================================================
struct FmEngineOpaque {
    FmEngine engine;
    explicit FmEngineOpaque(uint32_t sr) : engine(sr) {}
};

struct WasapiOpaque {
    WasapiOutput output;
    WasapiOpaque(FmEngine& e, bool excl)
        : output(e, excl) {}
    WasapiOpaque(FmEngine& e, bool excl, const std::wstring& device_id)
        : output(e, excl, device_id) {}
};

// =========================================================
//  FmChipType → ChipType 変換
//
//  FmEngineApi.h の FmChipType と FmChip.h の ChipType は
//  順序が異なる可能性があるため、switch で明示的にマッピングする。
//  FmChip.h に新チップが追加された場合はここにも追記すること。
// =========================================================
static bool toChipType(FmChipType api_type, ChipType& out) {
    switch (api_type) {
        case FM_CHIP_Y8950:  out = ChipType::Y8950;  return true;
        case FM_CHIP_OPL:    out = ChipType::OPL;    return true;
        case FM_CHIP_OPL2:   out = ChipType::OPL2;   return true;
        case FM_CHIP_OPL3:   out = ChipType::OPL3;   return true;
        case FM_CHIP_OPL4:   out = ChipType::OPL4;   return true;
        case FM_CHIP_OPN:    out = ChipType::OPN;     return true;
        case FM_CHIP_OPNA:   out = ChipType::OPNA;   return true;
        case FM_CHIP_OPNB:   out = ChipType::OPNB;   return true;
        case FM_CHIP_OPNBB:  out = ChipType::OPNBB;  return true;
        case FM_CHIP_OPN2:   out = ChipType::OPN2;   return true;
        case FM_CHIP_OPM:    out = ChipType::OPM;    return true;
        case FM_CHIP_OPLL:   out = ChipType::OPLL;   return true;
        case FM_CHIP_OPLLP:  out = ChipType::OPLLP;  return true;
        case FM_CHIP_OPLLX:  out = ChipType::OPLLX;  return true;
        case FM_CHIP_OPZ:    out = ChipType::OPZ;    return true;
        case FM_CHIP_VRC7:   out = ChipType::VRC7;   return true;
        default:                                       return false;
    }
}

// =========================================================
//  例外を FmResult に変換するヘルパー
// =========================================================
template<typename Fn>
static FmResult safeCall(Fn&& fn) noexcept {
    try {
        fn();
        return FM_OK;
    } catch (const std::invalid_argument& e) {
        fprintf(stderr, "[FmEngineApi] invalid_argument: %s\n", e.what());
        return FM_ERR_INVALID_ARG;
    } catch (const std::runtime_error& e) {
        fprintf(stderr, "[FmEngineApi] runtime_error: %s\n", e.what());
        return FM_ERR_AUDIO;
    } catch (const std::exception& e) {
        fprintf(stderr, "[FmEngineApi] exception: %s\n", e.what());
        return FM_ERR_EXCEPTION;
    } catch (...) {
        fprintf(stderr, "[FmEngineApi] unknown exception\n");
        return FM_ERR_EXCEPTION;
    }
}

#define REQUIRE_PTR(p) do { if (!(p)) return FM_ERR_INVALID_ARG; } while(0)

// =========================================================
//  FmEngine API 実装
// =========================================================

FMENGINE_API FmEngineHandle FMENGINE_CALL
FmEngine_Create(uint32_t sample_rate) {
    return new(std::nothrow) FmEngineOpaque(sample_rate);
}

FMENGINE_API void FMENGINE_CALL
FmEngine_Destroy(FmEngineHandle h) {
    delete static_cast<FmEngineOpaque*>(h);
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_AddChip(FmEngineHandle h, FmChipType api_type, uint32_t clock,
                 uint32_t* out_id) {
    REQUIRE_PTR(h);
    REQUIRE_PTR(out_id);
    ChipType ct;
    if (!toChipType(api_type, ct)) return FM_ERR_INVALID_ARG;
    return safeCall([&] {
        *out_id = static_cast<FmEngineOpaque*>(h)->engine.addChip(ct, clock);
    });
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_AddExtChip(FmEngineHandle h, FmChipTypeExt api_type, uint32_t clock,
                    uint32_t* out_id) {
    REQUIRE_PTR(h);
    REQUIRE_PTR(out_id);
    return safeCall([&] {
        auto ct = static_cast<ChipTypeExt>(api_type);
        *out_id = static_cast<FmEngineOpaque*>(h)->engine.addExtChip(ct, clock);
    });
}

FMENGINE_API const char* FMENGINE_CALL
FmEngine_GetChipName(FmEngineHandle h, uint32_t chip_id) {
    if (!h) return nullptr;
    const auto* c = static_cast<FmEngineOpaque*>(h)->engine.chip(chip_id);
    return c ? c->name() : nullptr;
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetNativeRate(FmEngineHandle h, uint32_t chip_id) {
    if (!h) return 0;
    const auto* c = static_cast<FmEngineOpaque*>(h)->engine.chip(chip_id);
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
                 float* out_l, float* out_r) {
    REQUIRE_PTR(h);
    REQUIRE_PTR(out_l);
    REQUIRE_PTR(out_r);
    return safeCall([&] {
        auto& eng = static_cast<FmEngineOpaque*>(h)->engine;
        *out_l = eng.getGainL(chip_id);
        *out_r = eng.getGainR(chip_id);
    });
}

FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetMemory(FmEngineHandle h, uint32_t chip_id,
                   FmMemoryType mem_type, const uint8_t* data, uint32_t size) {
    REQUIRE_PTR(h);
    REQUIRE_PTR(data);
    return safeCall([&] {
        auto ac = static_cast<ymfm::access_class>(mem_type);
        static_cast<FmEngineOpaque*>(h)->engine.setMemory(chip_id, ac, data, size);
    });
}

FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetMemorySize(FmEngineHandle h, uint32_t chip_id, FmMemoryType mem_type) {
    if (!h) return 0;
    auto ac = static_cast<ymfm::access_class>(mem_type);
    return static_cast<FmEngineOpaque*>(h)->engine.memorySize(chip_id, ac);
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
    return new(std::nothrow) WasapiOpaque(eng, exclusive != 0);
}

// =========================================================
//  デバイスキャッシュ (Wasapi_GetDevice* 系の内部状態)
//  Wasapi_GetDeviceCount() を呼ぶたびに列挙し直す。
// =========================================================
static std::vector<WasapiDeviceInfo> s_deviceCache;

FMENGINE_API WasapiHandle FMENGINE_CALL
Wasapi_CreateWithDevice(FmEngineHandle h, int exclusive, const wchar_t* device_id) {
    if (!h || !device_id) return nullptr;
    auto& eng = static_cast<FmEngineOpaque*>(h)->engine;
    try {
        auto* p = new(std::nothrow) WasapiOpaque(eng, exclusive != 0,
                                                  std::wstring(device_id));
        return static_cast<WasapiHandle>(p);
    } catch (...) {
        return nullptr;
    }
}

FMENGINE_API uint32_t FMENGINE_CALL
Wasapi_GetDeviceCount(void) {
    try {
        s_deviceCache = enumerateWasapiDevices();
    } catch (...) {
        s_deviceCache.clear();
    }
    return static_cast<uint32_t>(s_deviceCache.size());
}

FMENGINE_API FmResult FMENGINE_CALL
Wasapi_GetDeviceId(uint32_t index, wchar_t* buf, uint32_t buf_len) {
    if (!buf || index >= s_deviceCache.size()) return FM_ERR_INVALID_ARG;
    const std::wstring& id = s_deviceCache[index].id;
    if (buf_len == 0) return FM_ERR_INVALID_ARG;
    wcsncpy_s(buf, buf_len, id.c_str(), _TRUNCATE);
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL
Wasapi_GetDeviceName(uint32_t index, wchar_t* buf, uint32_t buf_len) {
    if (!buf || index >= s_deviceCache.size()) return FM_ERR_INVALID_ARG;
    const std::wstring& name = s_deviceCache[index].name;
    if (buf_len == 0) return FM_ERR_INVALID_ARG;
    wcsncpy_s(buf, buf_len, name.c_str(), _TRUNCATE);
    return FM_OK;
}

FMENGINE_API int FMENGINE_CALL
Wasapi_IsDefaultDevice(uint32_t index) {
    if (index >= s_deviceCache.size()) return 0;
    return s_deviceCache[index].isDefault ? 1 : 0;
}

FMENGINE_API void FMENGINE_CALL
Wasapi_Destroy(WasapiHandle h) {
    delete static_cast<WasapiOpaque*>(h);
}

FMENGINE_API FmResult FMENGINE_CALL
Wasapi_Start(WasapiHandle h) {
    REQUIRE_PTR(h);
    return safeCall([&] { static_cast<WasapiOpaque*>(h)->output.start(); });
}

FMENGINE_API FmResult FMENGINE_CALL
Wasapi_Stop(WasapiHandle h) {
    REQUIRE_PTR(h);
    return safeCall([&] { static_cast<WasapiOpaque*>(h)->output.stop(); });
}

FMENGINE_API uint32_t FMENGINE_CALL
Wasapi_GetSampleRate(WasapiHandle h) {
    if (!h) return 0;
    return static_cast<WasapiOpaque*>(h)->output.sampleRate();
}
