#pragma once
// FmChip.h
// ymfm コアのラッパー。チップ種別ごとの抽象インターフェースと
// ymfm_interface 実装を提供する。
//
// 変更履歴:
//   v2: クロック周波数をコンストラクタで指定可能に。
//       チップのネイティブサンプルレートとエンジンレートが異なる場合、
//       線形補間リサンプラー (LinearResampler) で吸収する。
//
// 依存: ymfm (https://github.com/aaronsgiles/ymfm)
//       C++17以上

#include "ymfm_opl.h"
#include "ymfm_opn.h"
#include "ymfm_opm.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <cassert>
#include <cmath>
#include <algorithm>

// =========================================================
//  チップ種別列挙
// =========================================================
enum class ChipType {
    OPL2,   // YM3812
    OPL3,   // YMF262
    OPN2,   // YM2612  (Mega Drive)
    OPM,    // YM2151  (arcade)
};

// =========================================================
//  標準クロック定数
// =========================================================
namespace FmClock {
    constexpr uint32_t OPL2  = 3'579'545;   // 3.58 MHz (NTSC)
    constexpr uint32_t OPL3  = 14'318'180;  // 14.3 MHz
    constexpr uint32_t OPN2  = 7'670'453;   // Mega Drive
    constexpr uint32_t OPM   = 3'579'545;   // arcade board
}

// =========================================================
//  LinearResampler
//  チップのネイティブレート → エンジンのターゲットレートへ
//  線形補間でリサンプリングする。
//
//  使い方:
//    resampler.setup(chip_rate, target_rate);
//    resampler.process(generate_fn, out_l, out_r, n_out_samples);
//
//  generate_fn: void(float* l, float* r, uint32_t n) 形式で
//               チップのネイティブレートで n サンプル生成する関数。
// =========================================================
class LinearResampler {
public:
    void setup(uint32_t src_rate, uint32_t dst_rate) {
        m_src_rate = src_rate;
        m_dst_rate = dst_rate;
        // フェーズ加算量: src サンプル単位で dst 1 サンプル分進む量
        // 固定小数点 (32.32) で保持
        m_phase_inc = (static_cast<uint64_t>(src_rate) << 32) / dst_rate;
        m_phase     = 0;
        m_prev_l    = 0.0f;
        m_prev_r    = 0.0f;
        m_cur_l     = 0.0f;
        m_cur_r     = 0.0f;
        m_work_l.clear();
        m_work_r.clear();
    }

    bool isPassthrough() const { return m_src_rate == m_dst_rate; }

    // generate_fn を呼びながら dst_samples 分のサンプルを生成し
    // out_l/out_r に書き込む (上書き、加算ではない)
    template<typename GenFn>
    void process(GenFn&& generate_fn, float* out_l, float* out_r, uint32_t dst_samples) {
        if (isPassthrough()) {
            generate_fn(out_l, out_r, dst_samples);
            return;
        }

        // 必要な src サンプル数を見積もる (余裕 +2)
        const uint32_t src_needed =
            static_cast<uint32_t>((static_cast<uint64_t>(dst_samples) * m_src_rate) / m_dst_rate) + 2;

        m_work_l.resize(src_needed);
        m_work_r.resize(src_needed);
        generate_fn(m_work_l.data(), m_work_r.data(), src_needed);

        uint32_t src_idx = 0;
        for (uint32_t di = 0; di < dst_samples; ++di) {
            // フェーズの整数部 = 読み出す src インデックス
            const uint32_t int_part  = static_cast<uint32_t>(m_phase >> 32);
            const float    frac      = static_cast<float>(m_phase & 0xFFFFFFFFull) * (1.0f / 4294967296.0f);

            // int_part が今の src_idx を超えていたら prev/cur を進める
            while (src_idx <= int_part && src_idx < src_needed) {
                m_prev_l = m_cur_l;
                m_prev_r = m_cur_r;
                m_cur_l  = (src_idx < src_needed) ? m_work_l[src_idx] : 0.0f;
                m_cur_r  = (src_idx < src_needed) ? m_work_r[src_idx] : 0.0f;
                ++src_idx;
            }

            // 線形補間
            out_l[di] = m_prev_l + (m_cur_l - m_prev_l) * frac;
            out_r[di] = m_prev_r + (m_cur_r - m_prev_r) * frac;

            m_phase += m_phase_inc;
        }

        // 次回に持ち越すフェーズは src_idx 分を引く
        m_phase -= static_cast<uint64_t>(src_idx) << 32;
    }

private:
    uint32_t m_src_rate = 0;
    uint32_t m_dst_rate = 0;
    uint64_t m_phase_inc = 0;
    uint64_t m_phase     = 0;
    float    m_prev_l    = 0.0f;
    float    m_prev_r    = 0.0f;
    float    m_cur_l     = 0.0f;
    float    m_cur_r     = 0.0f;
    std::vector<float> m_work_l;
    std::vector<float> m_work_r;
};

// =========================================================
//  FmChip – 1つのYamahaチップを表すインターフェース
// =========================================================
class FmChip {
public:
    virtual ~FmChip() = default;

    // レジスタ書き込み (port: OPL3/OPN2 では bank 選択に使う)
    virtual void write(uint32_t port, uint8_t reg, uint8_t value) = 0;

    // dst_samples フレーム分だけ生成して out_l/out_r に書き込む
    // (上書き。ゲイン適用・ミックスは FmEngine 側で行う)
    // リサンプリングが必要な場合は実装内で吸収する。
    virtual void generate(float* out_l, float* out_r, uint32_t dst_samples) = 0;

    // エンジンのターゲットサンプルレートをセット (addChip 時に呼ばれる)
    virtual void setTargetRate(uint32_t target_rate) = 0;

    // チップのネイティブサンプルレート (クロック÷分周比)
    virtual uint32_t nativeRate() const = 0;

    virtual ChipType    type() const = 0;
    virtual const char* name() const = 0;
    virtual uint32_t    clock() const = 0;
};

// =========================================================
//  ymfm_interface 実装 (タイマー・IRQ は最小限 no-op)
// =========================================================
class BasicYmfmInterface : public ymfm::ymfm_interface {
public:
    void ymfm_set_timer(uint32_t, int32_t) override {}
    void ymfm_sync_mode_write(uint8_t) override {}
    void ymfm_sync_check_interrupts() override {}
    void ymfm_update_irq(bool) override {}

    uint8_t ymfm_external_read(ymfm::access_class, uint32_t) override { return 0; }
    void ymfm_external_write(ymfm::access_class, uint32_t, uint8_t) override {}
};

// =========================================================
//  FmChipImpl<ChipImpl, TType>
//  テンプレートで全チップを統一実装。
//  リサンプリングは LinearResampler が担当する。
// =========================================================
template<typename ChipImpl, ChipType TType>
class FmChipImpl final : public FmChip {
public:
    // clock: チップのマスタークロック Hz
    explicit FmChipImpl(uint32_t clock)
        : m_chip(m_iface, clock), m_clock(clock)
    {
        m_chip.reset();
        m_native_rate = m_chip.sample_rate(clock);
    }

    void write(uint32_t port, uint8_t reg, uint8_t value) override {
        if constexpr (has_write_address_hi<ChipImpl>) {
            m_chip.write_address_hi(port != 0 ? 1 : 0);
        }
        m_chip.write(reg, value);
    }

    void generate(float* out_l, float* out_r, uint32_t dst_samples) override {
        m_resampler.process(
            [this](float* l, float* r, uint32_t n) {
                generateNative(l, r, n);
            },
            out_l, out_r, dst_samples
        );
    }

    void setTargetRate(uint32_t target_rate) override {
        m_target_rate = target_rate;
        m_resampler.setup(m_native_rate, target_rate);
    }

    uint32_t nativeRate() const override { return m_native_rate; }
    ChipType    type()  const override { return TType; }
    uint32_t    clock() const override { return m_clock; }
    const char* name()  const override;  // 特殊化で定義

private:
    // チップのネイティブレートで n サンプル生成 (float, -1..+1, 上書き)
    void generateNative(float* out_l, float* out_r, uint32_t n) {
        typename ChipImpl::output_data out_data{};
        constexpr float kScale = 1.0f / 32768.0f;
        for (uint32_t i = 0; i < n; ++i) {
            m_chip.generate(&out_data);
            out_l[i] = static_cast<float>(out_data.data[0]) * kScale;
            if constexpr (ChipImpl::output_data::outputs >= 2)
                out_r[i] = static_cast<float>(out_data.data[1]) * kScale;
            else
                out_r[i] = out_l[i];
        }
    }

    // write_address_hi を持つかどうかの型トレイト
    template<typename T, typename = void>
    struct has_write_address_hi_impl : std::false_type {};
    template<typename T>
    struct has_write_address_hi_impl<T,
        std::void_t<decltype(std::declval<T>().write_address_hi(0u))>>
        : std::true_type {};
    template<typename T>
    static constexpr bool has_write_address_hi = has_write_address_hi_impl<T>::value;

    BasicYmfmInterface m_iface;
    ChipImpl           m_chip;
    uint32_t           m_clock;
    uint32_t           m_native_rate = 0;
    uint32_t           m_target_rate = 0;
    LinearResampler    m_resampler;
};

// =========================================================
//  name() 特殊化
// =========================================================
template<> inline const char* FmChipImpl<ymfm::ym3812, ChipType::OPL2>::name() const { return "OPL2 (YM3812)"; }
template<> inline const char* FmChipImpl<ymfm::ymf262, ChipType::OPL3>::name() const { return "OPL3 (YMF262)"; }
template<> inline const char* FmChipImpl<ymfm::ym2612, ChipType::OPN2>::name() const { return "OPN2 (YM2612)"; }
template<> inline const char* FmChipImpl<ymfm::ym2151, ChipType::OPM>::name()  const { return "OPM (YM2151)";  }

// =========================================================
//  ファクトリ関数
//  clock=0 のとき各チップの標準クロックを使う
// =========================================================
inline std::unique_ptr<FmChip> createChip(ChipType type, uint32_t clock = 0) {
    auto resolve = [](uint32_t c, uint32_t def) { return c ? c : def; };
    switch (type) {
        case ChipType::OPL2:
            return std::make_unique<FmChipImpl<ymfm::ym3812, ChipType::OPL2>>(
                resolve(clock, FmClock::OPL2));
        case ChipType::OPL3:
            return std::make_unique<FmChipImpl<ymfm::ymf262, ChipType::OPL3>>(
                resolve(clock, FmClock::OPL3));
        case ChipType::OPN2:
            return std::make_unique<FmChipImpl<ymfm::ym2612, ChipType::OPN2>>(
                resolve(clock, FmClock::OPN2));
        case ChipType::OPM:
            return std::make_unique<FmChipImpl<ymfm::ym2151, ChipType::OPM>>(
                resolve(clock, FmClock::OPM));
    }
    return nullptr;
}
