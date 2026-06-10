#pragma once
// ExternalChip.h
// ymfm 以外の外部エミュレータライブラリを FmChip インターフェースで統一するラッパー。
//
// 対応ライブラリ:
//   emu2149  (YM2149/PSG)    https://github.com/digital-sound-antiques/emu2149
//   emu76489 (SN76489)       https://github.com/digital-sound-antiques/emu76489
//   emu2212  (SCC/K051649)   https://github.com/digital-sound-antiques/emu2212
//   SAASound (SAA1099)       独立DLL (SAASound.dll) を実行時に LoadLibrary でロード
//
// サブモジュール配置想定:
//   extern/emu2149/   extern/emu76489/   extern/emu2212/
//   SAASound.dll は sample_app.exe と同じディレクトリに配置すること。
//
// 各ライブラリのリサンプリングは FmChip と同様に LinearResampler で吸収する。

#include "FmChip.h"   // FmChip, LinearResampler, ChipType, FmClock を再利用

// -------------------------------------------------------
//  emu2149 (YM2149 / PSG)
// -------------------------------------------------------
#include "emu2149.h"

// -------------------------------------------------------
//  emu76489 (SN76489)
// -------------------------------------------------------
#include "emu76489.h"

// -------------------------------------------------------
//  emu2212 (SCC / K051649)
// -------------------------------------------------------
#include "emu2212.h"

// -------------------------------------------------------
//  SAASound は SAASound.h を include しない。
//  LoadLibrary 経由で関数ポインタを取得する。
// -------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <vector>
#include <stdexcept>

// ChipType 列挙に追加分
enum class ChipTypeExt {
    PSG     = 100,  // YM2149 (PSG) via emu2149
    SN76489 = 101,  // SN76489      via emu76489
    SCC     = 102,  // SCC/K051649  via emu2212
    SAA1099 = 103,  // SAA1099      via SAASound.dll (動的ロード)
};

namespace ExtClock {
    constexpr uint32_t PSG     = 3'579'545;  // NTSC
    constexpr uint32_t SN76489 = 3'579'545;  // SMS / GG
    constexpr uint32_t SCC     = 3'579'545;  // MSX
    constexpr uint32_t SAA1099 = 8'000'000;  // SAM Coupe
}

// =========================================================
//  ExtChip — 外部ライブラリ用 FmChip 互換インターフェース
// =========================================================
class ExtChip {
public:
    virtual ~ExtChip() = default;
    virtual void        write(uint32_t port, uint8_t reg, uint8_t value) = 0;
    virtual void        generate(float* out_l, float* out_r, uint32_t dst_samples) = 0;
    virtual void        setTargetRate(uint32_t target_rate) = 0;
    virtual uint32_t    nativeRate() const = 0;
    virtual ChipTypeExt type()  const = 0;
    virtual const char* name()  const = 0;
    virtual uint32_t    clock() const = 0;
};

// =========================================================
//  PSGChip — YM2149 ラッパー (emu2149)
//
//  write(port=0, reg, value):
//    reg=0 → アドレスレジスタ書き込み (PSG_writeIO adr=0)
//    reg=1 → データ書き込み           (PSG_writeIO adr=1)
//    または port に関係なく reg をレジスタ番号、value をデータとして
//    PSG_writeReg(reg, value) を直接呼ぶ。
//  → 実機の動作に合わせ PSG_writeReg を使う。
// =========================================================
class PSGChip final : public ExtChip {
public:
    explicit PSGChip(uint32_t clock, uint32_t target_rate = 44100)
        : m_clock(clock)
    {
        m_psg = PSG_new(clock, clock / 8); // 内部 rate は後で設定
        if (!m_psg) throw std::runtime_error("PSG_new failed");
        PSG_setClockDivider(m_psg, 1);     // YM2149: クロック÷2
        PSG_setVolumeMode(m_psg, 1);       // YM style
        PSG_reset(m_psg);
        setTargetRate(target_rate);
    }

    ~PSGChip() override {
        if (m_psg) PSG_delete(m_psg);
    }

    void write(uint32_t /*port*/, uint8_t reg, uint8_t value) override {
        // PSG_writeReg はアドレスラッチ不要で直接レジスタに書く
        PSG_writeReg(m_psg, reg, value);
    }

    void generate(float* out_l, float* out_r, uint32_t dst_samples) override {
        m_resampler.process(
            [this](float* l, float* r, uint32_t n) {
                constexpr float kScale = 1.0f / 32768.0f;
                for (uint32_t i = 0; i < n; ++i) {
                    float s = static_cast<float>(PSG_calc(m_psg)) * kScale;
                    l[i] = s;
                    r[i] = s;
                }
            },
            out_l, out_r, dst_samples);
    }

    void setTargetRate(uint32_t target_rate) override {
        // emu2149 の推奨: clock/8 をネイティブレートとして外部リサンプルする
        const uint32_t native = m_clock / 8;
        PSG_setRate(m_psg, native);
        PSG_setQuality(m_psg, 0); // 内部リサンプラーをバイパス
        m_native_rate = native;
        m_resampler.setup(native, target_rate);
    }

    uint32_t    nativeRate() const override { return m_native_rate; }
    ChipTypeExt type()       const override { return ChipTypeExt::PSG; }
    uint32_t    clock()      const override { return m_clock; }
    const char* name()       const override { return "PSG (YM2149)"; }

private:
    PSG*           m_psg         = nullptr;
    uint32_t       m_clock;
    uint32_t       m_native_rate = 0;
    LinearResampler m_resampler;
};

// =========================================================
//  SNGChip — SN76489 ラッパー (emu76489)
//
//  SN76489 はシリアルインターフェース: データを1バイトずつ書き込む。
//  write(port, reg, value): reg を無視し、value を SNG_writeIO に渡す。
//  (YM エンジンと異なり reg/value の区別がない)
// =========================================================
class SNGChip final : public ExtChip {
public:
    explicit SNGChip(uint32_t clock, uint32_t target_rate = 44100)
        : m_clock(clock)
    {
        m_sng = SNG_new(clock, clock / 16);
        if (!m_sng) throw std::runtime_error("SNG_new failed");
        SNG_reset(m_sng);
        setTargetRate(target_rate);
    }

    ~SNGChip() override {
        if (m_sng) SNG_delete(m_sng);
    }

    // SN76489 はシリアルバス: value をそのまま書き込む
    // reg は使わない (port=0: write, port=1: GG stereo control)
    void write(uint32_t port, uint8_t /*reg*/, uint8_t value) override {
        if (port == 0)
            SNG_writeIO(m_sng, value);
        else
            SNG_writeGGIO(m_sng, value); // Game Gear ステレオコントロール
    }

    void generate(float* out_l, float* out_r, uint32_t dst_samples) override {
        m_resampler.process(
            [this](float* l, float* r, uint32_t n) {
                constexpr float kScale = 1.0f / 32768.0f;
                int32_t stereo[2] = {};
                for (uint32_t i = 0; i < n; ++i) {
                    SNG_calc_stereo(m_sng, stereo);
                    l[i] = static_cast<float>(stereo[0]) * kScale;
                    r[i] = static_cast<float>(stereo[1]) * kScale;
                }
            },
            out_l, out_r, dst_samples);
    }

    void setTargetRate(uint32_t target_rate) override {
        const uint32_t native = m_clock / 16;
        SNG_set_rate(m_sng, native);
        SNG_set_quality(m_sng, 0);
        m_native_rate = native;
        m_resampler.setup(native, target_rate);
    }

    uint32_t    nativeRate() const override { return m_native_rate; }
    ChipTypeExt type()       const override { return ChipTypeExt::SN76489; }
    uint32_t    clock()      const override { return m_clock; }
    const char* name()       const override { return "SN76489"; }

private:
    SNG*           m_sng         = nullptr;
    uint32_t       m_clock;
    uint32_t       m_native_rate = 0;
    LinearResampler m_resampler;
};

// =========================================================
//  SCCChip — SCC/K051649 ラッパー (emu2212)
//
//  write(port, reg, value):
//    SCC_write(scc, reg, value) を直接呼ぶ。
//    port は無視 (SCCはバンク切り替えをアドレスで行う)
// =========================================================
class SCCChip final : public ExtChip {
public:
    explicit SCCChip(uint32_t clock, uint32_t target_rate = 44100,
                     uint32_t scc_type = SCC_STANDARD)
        : m_clock(clock)
    {
        m_scc = SCC_new(clock, clock / 8);
        if (!m_scc) throw std::runtime_error("SCC_new failed");
        SCC_set_type(m_scc, scc_type);
        SCC_reset(m_scc);
        setTargetRate(target_rate);
    }

    ~SCCChip() override {
        if (m_scc) SCC_delete(m_scc);
    }

    void write(uint32_t /*port*/, uint8_t reg, uint8_t value) override {
        SCC_write(m_scc, reg, value);
    }

    void generate(float* out_l, float* out_r, uint32_t dst_samples) override {
        m_resampler.process(
            [this](float* l, float* r, uint32_t n) {
                constexpr float kScale = 1.0f / 32768.0f;
                for (uint32_t i = 0; i < n; ++i) {
                    float s = static_cast<float>(SCC_calc(m_scc)) * kScale;
                    l[i] = s;
                    r[i] = s;
                }
            },
            out_l, out_r, dst_samples);
    }

    void setTargetRate(uint32_t target_rate) override {
        const uint32_t native = m_clock / 8;
        SCC_set_rate(m_scc, native);
        SCC_set_quality(m_scc, 0);
        m_native_rate = native;
        m_resampler.setup(native, target_rate);
    }

    uint32_t    nativeRate() const override { return m_native_rate; }
    ChipTypeExt type()       const override { return ChipTypeExt::SCC; }
    uint32_t    clock()      const override { return m_clock; }
    const char* name()       const override { return "SCC (K051649)"; }

private:
    SCC*           m_scc         = nullptr;
    uint32_t       m_clock;
    uint32_t       m_native_rate = 0;
    LinearResampler m_resampler;
};

// =========================================================
//  SAASoundProxy — SAASound.dll を実行時ロードするプロキシ
//
//  SAASound.dll は BYTE マクロ等の名前衝突があり静的リンクが困難なため、
//  LoadLibrary で動的ロードして関数ポインタ経由で呼び出す。
//
//  SAASound の C エクスポート API:
//    void* SAASOUND_API CreateCSAASound(void)
//    void  SAASOUND_API DestroyCSAASound(void*)
//    void  SAASOUND_API SAA_SetSampleRate(void*, unsigned int)
//    void  SAASOUND_API SAA_SetClockRate(void*, unsigned int)
//    void  SAASOUND_API SAA_WriteAddress(void*, unsigned char)
//    void  SAASOUND_API SAA_WriteData(void*, unsigned char)
//    void  SAASOUND_API SAA_WriteAddressData(void*, unsigned char, unsigned char)
//    void  SAASOUND_API SAA_Clear(void*)
//    void  SAASOUND_API SAA_GenerateMany(void*, unsigned char*, unsigned int)
//
//  ※ SAASound の C エクスポートが存在しない場合は C++ vtable 経由になるが、
//     DLL のコンパイラが一致していれば動作する。
//     ここでは C スタイル エクスポートを優先し、なければ C++ を試みる。
// =========================================================
class SAASoundProxy {
public:
    // DLL 名。同じディレクトリになければ PATH から検索される。
    static constexpr const char* DLL_NAME = "SAASound.dll";

    // 関数ポインタ型定義
    using FnCreate          = void* (__cdecl*)();
    using FnDestroy         = void  (__cdecl*)(void*);
    using FnSetSampleRate   = void  (__cdecl*)(void*, unsigned int);
    using FnSetClockRate    = void  (__cdecl*)(void*, unsigned int);
    using FnWriteAddress    = void  (__cdecl*)(void*, unsigned char);
    using FnWriteData       = void  (__cdecl*)(void*, unsigned char);
    using FnWriteAddrData   = void  (__cdecl*)(void*, unsigned char, unsigned char);
    using FnClear           = void  (__cdecl*)(void*);
    using FnGenerateMany    = void  (__cdecl*)(void*, unsigned char*, unsigned int);

    FnCreate        Create        = nullptr;
    FnDestroy       Destroy       = nullptr;
    FnSetSampleRate SetSampleRate = nullptr;
    FnSetClockRate  SetClockRate  = nullptr;
    FnWriteAddrData WriteAddrData = nullptr;
    FnClear         Clear         = nullptr;
    FnGenerateMany  GenerateMany  = nullptr;

    HMODULE hDll = nullptr;

    // シングルトン: 1度ロードしたら使いまわす
    static SAASoundProxy& instance() {
        static SAASoundProxy inst;
        return inst;
    }

    bool isLoaded() const { return hDll != nullptr && Create != nullptr; }

    // ロード試行 (失敗しても例外を投げない)
    bool tryLoad() {
        if (hDll) return isLoaded();
        hDll = LoadLibraryA(DLL_NAME);
        if (!hDll) return false;

        Create        = reinterpret_cast<FnCreate>       (GetProcAddress(hDll, "CreateCSAASound"));
        Destroy       = reinterpret_cast<FnDestroy>      (GetProcAddress(hDll, "DestroyCSAASound"));
        SetSampleRate = reinterpret_cast<FnSetSampleRate>(GetProcAddress(hDll, "SAA_SetSampleRate"));
        SetClockRate  = reinterpret_cast<FnSetClockRate> (GetProcAddress(hDll, "SAA_SetClockRate"));
        WriteAddrData = reinterpret_cast<FnWriteAddrData>(GetProcAddress(hDll, "SAA_WriteAddressData"));
        Clear         = reinterpret_cast<FnClear>        (GetProcAddress(hDll, "SAA_Clear"));
        GenerateMany  = reinterpret_cast<FnGenerateMany> (GetProcAddress(hDll, "SAA_GenerateMany"));

        if (!Create || !Destroy || !GenerateMany || !WriteAddrData) {
            FreeLibrary(hDll);
            hDll = nullptr;
            return false;
        }
        return true;
    }

private:
    SAASoundProxy() = default;
    ~SAASoundProxy() {
        if (hDll) { FreeLibrary(hDll); hDll = nullptr; }
    }
    SAASoundProxy(const SAASoundProxy&) = delete;
    SAASoundProxy& operator=(const SAASoundProxy&) = delete;
};

// =========================================================
//  SAAChip — SAA1099 ラッパー (SAASound.dll 動的ロード)
// =========================================================
class SAAChip final : public ExtChip {
public:
    explicit SAAChip(uint32_t clock, uint32_t target_rate = 44100)
        : m_clock(clock), m_target_rate(target_rate)
    {
        auto& proxy = SAASoundProxy::instance();
        if (!proxy.tryLoad())
            throw std::runtime_error(
                "SAASound.dll not found or missing exports. "
                "Place SAASound.dll next to FmEngineApi.dll.");

        m_inst = proxy.Create();
        if (!m_inst) throw std::runtime_error("CreateCSAASound failed");

        if (proxy.SetClockRate)  proxy.SetClockRate(m_inst, clock);
        if (proxy.SetSampleRate) proxy.SetSampleRate(m_inst, target_rate);
        if (proxy.Clear)         proxy.Clear(m_inst);

        m_native_rate = target_rate;
    }

    ~SAAChip() override {
        if (m_inst) {
            SAASoundProxy::instance().Destroy(m_inst);
            m_inst = nullptr;
        }
    }

    void write(uint32_t /*port*/, uint8_t reg, uint8_t value) override {
        SAASoundProxy::instance().WriteAddrData(m_inst, reg, value);
    }

    void generate(float* out_l, float* out_r, uint32_t dst_samples) override {
        m_intBuf.resize(dst_samples * 2);
        SAASoundProxy::instance().GenerateMany(
            m_inst,
            reinterpret_cast<unsigned char*>(m_intBuf.data()),
            dst_samples);

        constexpr float kScale = 1.0f / 32768.0f;
        for (uint32_t i = 0; i < dst_samples; ++i) {
            out_l[i] = static_cast<float>(m_intBuf[i * 2 + 0]) * kScale;
            out_r[i] = static_cast<float>(m_intBuf[i * 2 + 1]) * kScale;
        }
    }

    void setTargetRate(uint32_t target_rate) override {
        m_target_rate = target_rate;
        m_native_rate = target_rate;
        auto& proxy = SAASoundProxy::instance();
        if (proxy.SetSampleRate) proxy.SetSampleRate(m_inst, target_rate);
    }

    uint32_t    nativeRate() const override { return m_native_rate; }
    ChipTypeExt type()       const override { return ChipTypeExt::SAA1099; }
    uint32_t    clock()      const override { return m_clock; }
    const char* name()       const override { return "SAA1099"; }

private:
    void*    m_inst         = nullptr;
    uint32_t m_clock;
    uint32_t m_target_rate  = 44100;
    uint32_t m_native_rate  = 44100;
    std::vector<int16_t> m_intBuf;
};

// =========================================================
//  ファクトリ関数
// =========================================================
inline std::unique_ptr<ExtChip> createExtChip(
    ChipTypeExt type, uint32_t clock = 0, uint32_t target_rate = 44100)
{
    auto resolve = [](uint32_t c, uint32_t def) { return c ? c : def; };
    switch (type) {
        case ChipTypeExt::PSG:
            return std::make_unique<PSGChip>(resolve(clock, ExtClock::PSG), target_rate);
        case ChipTypeExt::SN76489:
            return std::make_unique<SNGChip>(resolve(clock, ExtClock::SN76489), target_rate);
        case ChipTypeExt::SCC:
            return std::make_unique<SCCChip>(resolve(clock, ExtClock::SCC), target_rate);
        case ChipTypeExt::SAA1099:
            return std::make_unique<SAAChip>(resolve(clock, ExtClock::SAA1099), target_rate);
    }
    return nullptr;
}

// =========================================================
//  ExtChipAdapter — ExtChip を FmChip インターフェースに変換するアダプタ
//  FmEngine は FmChip* を保持するため、このアダプタ経由で ExtChip を追加する。
// =========================================================
class ExtChipAdapter final : public FmChip {
public:
    explicit ExtChipAdapter(std::unique_ptr<ExtChip> chip)
        : m_chip(std::move(chip)) {}

    void write(uint32_t port, uint8_t reg, uint8_t value) override {
        m_chip->write(port, reg, value);
    }
    void generate(float* out_l, float* out_r, uint32_t dst_samples) override {
        m_chip->generate(out_l, out_r, dst_samples);
    }
    void setTargetRate(uint32_t target_rate) override {
        m_chip->setTargetRate(target_rate);
    }
    uint32_t    nativeRate() const override { return m_chip->nativeRate(); }
    ChipType    type()       const override {
        // ChipType と ChipTypeExt を統合する変換
        // FmEngine 側では ChipType は使われないため 0 を返す
        return static_cast<ChipType>(0);
    }
    const char* name()       const override { return m_chip->name(); }
    uint32_t    clock()      const override { return m_chip->clock(); }

    // ExtChip への直接アクセス
    ExtChip* extChip() { return m_chip.get(); }

private:
    std::unique_ptr<ExtChip> m_chip;
};
