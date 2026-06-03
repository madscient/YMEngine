#pragma once
// FmEngineApi.h
// DLL として公開する C 互換ファサード API。
//
// 設計方針:
//   - 不透明ポインタ (FmEngineHandle / WasapiHandle) で実装を隠蔽する。
//     呼び出し側は C++ヘッダ (FmEngine.h 等) を一切 include しなくてよい。
//   - すべての関数は extern "C" + __cdecl。
//     C# (P/Invoke)、Python (ctypes)、VB、Delphi 等から直接呼べる。
//   - エラーは戻り値 (0=成功, 負値=エラーコード) で返す。例外は DLL 境界を越えない。
//   - CRT / STL を ABI 境界に出さない。文字列は const char* のみ。
//
// ビルド定義:
//   FMENGINE_EXPORTS が定義されているとき → dllexport  (DLL 自身のビルド)
//   それ以外                              → dllimport  (利用側のビルド)
//   FMENGINE_STATIC が定義されているとき  → 属性なし   (静的リンク)

#ifndef FMENGINE_API_H
#define FMENGINE_API_H

#include <stdint.h>  // C ヘッダのみ使用

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
//  不透明ハンドル型
// =========================================================
#ifdef __cplusplus
extern "C" {
#endif

typedef struct FmEngineOpaque*  FmEngineHandle;
typedef struct WasapiOpaque*    WasapiHandle;

// =========================================================
//  エラーコード
// =========================================================
typedef enum FmResult {
    FM_OK               =  0,
    FM_ERR_INVALID_ARG  = -1,  // NULL ハンドル・範囲外 ID 等
    FM_ERR_COM          = -2,  // COM / WASAPI の初期化失敗
    FM_ERR_AUDIO        = -3,  // IAudioClient エラー
    FM_ERR_EXCEPTION    = -4,  // 予期しない例外
} FmResult;

// =========================================================
//  チップ種別 (FmChip.h の ChipType と値を合わせる)
// =========================================================
typedef enum FmChipType {
    FM_CHIP_OPL2 = 0,
    FM_CHIP_OPL3 = 1,
    FM_CHIP_OPN2 = 2,
    FM_CHIP_OPM  = 3,
} FmChipType;

// =========================================================
//  FmEngine API
// =========================================================

// エンジン生成。sample_rate: 出力サンプルレート (例: 44100, 48000)
FMENGINE_API FmEngineHandle FMENGINE_CALL
FmEngine_Create(uint32_t sample_rate);

// エンジン破棄
FMENGINE_API void FMENGINE_CALL
FmEngine_Destroy(FmEngineHandle engine);

// チップ追加。clock=0 で各チップの標準クロックを使用。
// 成功時: 0 以上の chip_id を *out_id に書き込む
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_AddChip(FmEngineHandle engine, FmChipType type, uint32_t clock,
                 uint32_t* out_id);

// チップ名を取得 (静的文字列、解放不要)
FMENGINE_API const char* FMENGINE_CALL
FmEngine_GetChipName(FmEngineHandle engine, uint32_t chip_id);

// チップのネイティブサンプルレートを取得
FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetNativeRate(FmEngineHandle engine, uint32_t chip_id);

// エンジンのターゲットサンプルレートを取得
FMENGINE_API uint32_t FMENGINE_CALL
FmEngine_GetSampleRate(FmEngineHandle engine);

// レジスタ書き込み (任意スレッドから安全)
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_Write(FmEngineHandle engine, uint32_t chip_id,
               uint8_t reg, uint8_t value, uint32_t port);

// ゲイン設定 (線形スケール。1.0 = 0 dB)
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_SetGain(FmEngineHandle engine, uint32_t chip_id,
                 float gain_l, float gain_r);

// ゲイン取得
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_GetGain(FmEngineHandle engine, uint32_t chip_id,
                 float* out_gain_l, float* out_gain_r);

// サンプル生成 (オーディオスレッドから呼ぶ)
// out_l, out_r: float[samples] の呼び出し側バッファ (上書き)
FMENGINE_API FmResult FMENGINE_CALL
FmEngine_Generate(FmEngineHandle engine,
                  float* out_l, float* out_r, uint32_t samples);

// =========================================================
//  WasapiOutput API
// =========================================================

// WASAPI 出力を作成して FmEngine と紐付ける
// exclusive: 0=Shared mode, 1=Exclusive mode
FMENGINE_API WasapiHandle FMENGINE_CALL
Wasapi_Create(FmEngineHandle engine, int exclusive);

// 破棄 (stop() も内部で呼ばれる)
FMENGINE_API void FMENGINE_CALL
Wasapi_Destroy(WasapiHandle wasapi);

// 再生開始
FMENGINE_API FmResult FMENGINE_CALL
Wasapi_Start(WasapiHandle wasapi);

// 再生停止
FMENGINE_API FmResult FMENGINE_CALL
Wasapi_Stop(WasapiHandle wasapi);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // FMENGINE_API_H
