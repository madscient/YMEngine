#pragma once
// FmEngineApi.h
// DLL として公開する C 互換ファサード API。
//
// MSVC 対応:
//   typedef enum は extern "C" ブロックの外で定義する。
//   extern "C" 内で enum を定義すると MSVC C2143/C2059 が発生する。

#ifndef FMENGINE_API_H
#define FMENGINE_API_H

#include <stdint.h>

// =========================================================
//  エクスポート属性マクロ
// =========================================================
#if defined(_WIN32) || defined(_WIN64)
#  if defined(FMENGINE_STATIC)
#    define FMENGINE_API
#  elif defined(FMENGINE_EXPORTS)
#    define FMENGINE_API __declspec(dllexport)
#  else
#    define FMENGINE_API __declspec(dllimport)
#  endif
#  define FMENGINE_CALL __cdecl
#else
#  define FMENGINE_API  __attribute__((visibility("default")))
#  define FMENGINE_CALL
#endif

// =========================================================
//  エラーコード
//  ※ extern "C" の外で定義 (MSVC C2143 回避)
// =========================================================
typedef enum FmResult {
    FM_OK               =  0,
    FM_ERR_INVALID_ARG  = -1,
    FM_ERR_COM          = -2,
    FM_ERR_AUDIO        = -3,
    FM_ERR_EXCEPTION    = -4,
} FmResult;

// =========================================================
//  チップ種別
//  ※ extern "C" の外で定義 (MSVC C2143 回避)
//  FmChip.h の ChipType と順序・値を一致させること。
// =========================================================
typedef enum FmChipType {
    FM_CHIP_Y8950  =  0,
    FM_CHIP_OPL    =  1,
    FM_CHIP_OPL2   =  2,
    FM_CHIP_OPL3   =  3,
    FM_CHIP_OPL4   =  4,
    FM_CHIP_OPN    =  5,
    FM_CHIP_OPNA   =  6,
    FM_CHIP_OPNB   =  7,
    FM_CHIP_OPNBB  =  8,
    FM_CHIP_OPN2   =  9,
    FM_CHIP_OPM    = 10,
    FM_CHIP_OPLL   = 11,
    FM_CHIP_OPLLP  = 12,
    FM_CHIP_OPLLX  = 13,
    FM_CHIP_OPZ    = 14,
    FM_CHIP_VRC7   = 15,
} FmChipType;

// =========================================================
//  不透明ハンドル前方宣言
//  ※ extern "C" の外に置く
// =========================================================
struct FmEngineOpaque;
struct WasapiOpaque;

// =========================================================
//  ハンドル typedef と関数宣言のみ extern "C" に入れる
// =========================================================
#ifdef __cplusplus
extern "C" {
#endif

typedef struct FmEngineOpaque*  FmEngineHandle;
typedef struct WasapiOpaque*    WasapiHandle;

FMENGINE_API FmEngineHandle FMENGINE_CALL FmEngine_Create(uint32_t sample_rate);
FMENGINE_API void           FMENGINE_CALL FmEngine_Destroy(FmEngineHandle engine);
FMENGINE_API FmResult       FMENGINE_CALL FmEngine_AddChip(
    FmEngineHandle engine, FmChipType type, uint32_t clock, uint32_t* out_id);
FMENGINE_API const char*    FMENGINE_CALL FmEngine_GetChipName(
    FmEngineHandle engine, uint32_t chip_id);
FMENGINE_API uint32_t       FMENGINE_CALL FmEngine_GetNativeRate(
    FmEngineHandle engine, uint32_t chip_id);
FMENGINE_API uint32_t       FMENGINE_CALL FmEngine_GetSampleRate(
    FmEngineHandle engine);
FMENGINE_API FmResult       FMENGINE_CALL FmEngine_Write(
    FmEngineHandle engine, uint32_t chip_id, uint8_t reg, uint8_t value, uint32_t port);
FMENGINE_API FmResult       FMENGINE_CALL FmEngine_SetGain(
    FmEngineHandle engine, uint32_t chip_id, float gain_l, float gain_r);
FMENGINE_API FmResult       FMENGINE_CALL FmEngine_GetGain(
    FmEngineHandle engine, uint32_t chip_id, float* out_gain_l, float* out_gain_r);
FMENGINE_API FmResult       FMENGINE_CALL FmEngine_Generate(
    FmEngineHandle engine, float* out_l, float* out_r, uint32_t samples);

FMENGINE_API WasapiHandle   FMENGINE_CALL Wasapi_Create(
    FmEngineHandle engine, int exclusive);
FMENGINE_API void           FMENGINE_CALL Wasapi_Destroy(WasapiHandle wasapi);
FMENGINE_API FmResult       FMENGINE_CALL Wasapi_Start(WasapiHandle wasapi);
FMENGINE_API FmResult       FMENGINE_CALL Wasapi_Stop(WasapiHandle wasapi);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // FMENGINE_API_H
