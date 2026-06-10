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
//  外部ライブラリチップ種別
//  ※ extern "C" の外で定義 (MSVC C2143 回避)
// =========================================================
typedef enum FmChipTypeExt {
    FM_CHIP_EXT_PSG     = 100,  // YM2149 (PSG)   via emu2149
    FM_CHIP_EXT_SN76489 = 101,  // SN76489        via emu76489
    FM_CHIP_EXT_SCC     = 102,  // SCC/K051649    via emu2212
    FM_CHIP_EXT_SAA1099 = 103,  // SAA1099        via SAASound
} FmChipTypeExt;

// =========================================================
//  外部メモリアクセス種別
//  ymfm::access_class と値を一致させる
//  ※ extern "C" の外で定義 (MSVC C2143 回避)
// =========================================================
typedef enum FmMemoryType {
    FM_MEM_IO      = 0,  // 汎用 I/O (通常は使わない)
    FM_MEM_ADPCM_A = 1,  // ADPCM-A ROM (OPNB/OPNBB)
    FM_MEM_ADPCM_B = 2,  // ADPCM-B ROM/RAM (OPNA/OPNB/OPNBB/Y8950)
    FM_MEM_PCM     = 3,  // PCM ROM (OPL4)
} FmMemoryType;

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

// 外部ライブラリチップ追加 (PSG/SN76489/SCC/SAA1099)
FMENGINE_API FmResult       FMENGINE_CALL FmEngine_AddExtChip(
    FmEngineHandle engine, FmChipTypeExt type, uint32_t clock, uint32_t* out_id);
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

// 外部メモリ設定 (ADPCM/PCM ROM/RAM)
// mem_type : FM_MEM_ADPCM_A / FM_MEM_ADPCM_B / FM_MEM_PCM
// data     : ROM データへのポインタ (呼び出し元が寿命を管理すること)
// size     : データサイズ (バイト)
// オーディオ再生開始前 (Wasapi_Start より前) に呼ぶこと。
FMENGINE_API FmResult       FMENGINE_CALL FmEngine_SetMemory(
    FmEngineHandle engine, uint32_t chip_id,
    FmMemoryType mem_type, const uint8_t* data, uint32_t size);

// 設定済みメモリサイズを取得 (未設定の場合は 0)
FMENGINE_API uint32_t       FMENGINE_CALL FmEngine_GetMemorySize(
    FmEngineHandle engine, uint32_t chip_id, FmMemoryType mem_type);
FMENGINE_API FmResult       FMENGINE_CALL FmEngine_Generate(
    FmEngineHandle engine, float* out_l, float* out_r, uint32_t samples);

FMENGINE_API WasapiHandle   FMENGINE_CALL Wasapi_Create(
    FmEngineHandle engine, int exclusive);

// デバイスIDを明示指定して WASAPI 出力を作成する
// device_id: Wasapi_GetDeviceId() で取得した文字列 (UTF-16LE、ワイド文字)
FMENGINE_API WasapiHandle   FMENGINE_CALL Wasapi_CreateWithDevice(
    FmEngineHandle engine, int exclusive, const wchar_t* device_id);

// 利用可能な再生デバイスの数を返す
// CoInitialize 済みのスレッドから呼ぶこと
FMENGINE_API uint32_t       FMENGINE_CALL Wasapi_GetDeviceCount(void);

// index 番目のデバイスIDを buf に書き込む (ワイド文字列)
// buf_len: バッファの wchar_t 単位のサイズ (128以上推奨)
// 戻り値: FM_OK、FM_ERR_INVALID_ARG (index 範囲外 / buf NULL)
FMENGINE_API FmResult       FMENGINE_CALL Wasapi_GetDeviceId(
    uint32_t index, wchar_t* buf, uint32_t buf_len);

// index 番目のデバイス表示名を buf に書き込む (ワイド文字列)
FMENGINE_API FmResult       FMENGINE_CALL Wasapi_GetDeviceName(
    uint32_t index, wchar_t* buf, uint32_t buf_len);

// index 番目のデバイスがデフォルトデバイスかどうかを返す (1=デフォルト, 0=その他)
FMENGINE_API int            FMENGINE_CALL Wasapi_IsDefaultDevice(uint32_t index);

FMENGINE_API void           FMENGINE_CALL Wasapi_Destroy(WasapiHandle wasapi);
FMENGINE_API FmResult       FMENGINE_CALL Wasapi_Start(WasapiHandle wasapi);
FMENGINE_API FmResult       FMENGINE_CALL Wasapi_Stop(WasapiHandle wasapi);
FMENGINE_API uint32_t       FMENGINE_CALL Wasapi_GetSampleRate(WasapiHandle wasapi);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // FMENGINE_API_H
