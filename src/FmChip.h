#pragma once
// FmChip.h
// ymfm コアのラッパー。
//
// MSVC 対応のポイント:
//   has_write_address_hi_impl をクラスメンバーテンプレートとして定義すると
//   MSVC C3856/C3858 が発生する。ネームスペーススコープの型トレイトに移動する。

#include "ymfm_opl.h"
#include "ymfm_opn.h"
#include "ymfm_opm.h"
#include "ymfm_opz.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <type_traits>

// =========================================================
//  チップ種別列挙
// =========================================================
enum class ChipType {
    Y8950,
    OPL,
    OPL2,
    OPL3,
    OPL4,
    OPN,
    OPNA,
    OPNB,
    OPNBB,
    OPN2,
    OPM,
    OPLL,
    OPLLP,
    OPLLX,
    OPZ,
    VRC7,
};

// =========================================================
//  標準クロック定数
// =========================================================
namespace FmClock {
    constexpr uint32_t Y8950  = 3'579'545;
    constexpr uint32_t OPL    = 3'579'545;
    constexpr uint32_t OPLL   = 3'579'545;
    constexpr uint32_t OPLLP  = 3'579'545;
    constexpr uint32_t OPLLX  = 3'579'545;
    constexpr uint32_t VRC7   = 3'579'545;
    constexpr uint32_t OPL2   = 3'579'545;
    constexpr uint32_t OPL3   = 14'318'180;
    constexpr uint32_t OPL4   = 16'934'400;
    constexpr uint32_t OPN    = 3'993'600;
    constexpr uint32_t OPNA   = 7'987'200;
    constexpr uint32_t OPNB   = 8'000'000;
    constexpr uint32_t OPNBB  = 8'000'000;
    constexpr uint32_t OPN2   = 7'670'453;
    constexpr uint32_t OPM    = 3'579'545;
    constexpr uint32_t OPZ    = 3'579'545;
}

// =========================================================
//  has_write_address_hi 型トレイト
//  ※ MSVC C3856/C3858 回避のためクラス外 (名前空間スコープ) に定義する。
//    クラスメンバーテンプレートとして定義すると MSVC が特殊化を拒否する。
// =========================================================
namespace detail {
    template<typename T, typename = void>
    struct has_write_address_hi : std::false_type {};

    template<typename T>
    struct has_write_address_hi<T,
        std::void_t<decltype(std::declval<T&>().write_address_hi(uint32_t{}))>>
        : std::true_type {};
} // namespace detail

// =========================================================
//  LinearResampler
// =========================================================
class LinearResampler {
public:
    void setup(uint32_t src_rate, uint32_t dst_rate) {
        m_src_rate  = src_rate;
        m_dst_rate  = dst_rate;
        m_phase_inc = (static_cast<uint64_t>(src_rate) << 32) / dst_rate;
        m_phase     = 0;
        m_prev_l = m_prev_r = m_cur_l = m_cur_r = 0.0f;
        m_work_l.clear();
        m_work_r.clear();
    }

    bool isPassthrough() const { return m_src_rate == m_dst_rate; }

    template<typename GenFn>
    void process(GenFn&& generate_fn,
                 float* out_l, float* out_r, uint32_t dst_samples)
    {
        if (isPassthrough()) {
            generate_fn(out_l, out_r, dst_samples);
            return;
        }

        const uint32_t src_needed =
            static_cast<uint32_t>(
                (static_cast<uint64_t>(dst_samples) * m_src_rate) / m_dst_rate) + 2;

        m_work_l.resize(src_needed);
        m_work_r.resize(src_needed);
        generate_fn(m_work_l.data(), m_work_r.data(), src_needed);

        uint32_t src_idx = 0;
        for (uint32_t di = 0; di < dst_samples; ++di) {
            const uint32_t int_part = static_cast<uint32_t>(m_phase >> 32);
            const float    frac     = static_cast<float>(m_phase & 0xFFFFFFFFull)
                                      * (1.0f / 4294967296.0f);

            while (src_idx <= int_part && src_idx < src_needed) {
                m_prev_l = m_cur_l;
                m_prev_r = m_cur_r;
                m_cur_l  = m_work_l[src_idx];
                m_cur_r  = m_work_r[src_idx];
                ++src_idx;
            }

            out_l[di] = m_prev_l + (m_cur_l - m_prev_l) * frac;
            out_r[di] = m_prev_r + (m_cur_r - m_prev_r) * frac;
            m_phase  += m_phase_inc;
        }
        m_phase -= static_cast<uint64_t>(src_idx) << 32;
    }

private:
    uint32_t m_src_rate = 0, m_dst_rate = 0;
    uint64_t m_phase_inc = 0, m_phase = 0;
    float    m_prev_l = 0, m_prev_r = 0, m_cur_l = 0, m_cur_r = 0;
    std::vector<float> m_work_l, m_work_r;
};

// =========================================================
//  FmChip インターフェース
// =========================================================
class FmChip {
public:
    virtual ~FmChip() = default;
    virtual void        write(uint32_t port, uint8_t reg, uint8_t value) = 0;
    virtual void        generate(float* out_l, float* out_r, uint32_t dst_samples) = 0;
    virtual void        setTargetRate(uint32_t target_rate) = 0;
    virtual uint32_t    nativeRate() const = 0;
    virtual ChipType    type()  const = 0;
    virtual const char* name()  const = 0;
    virtual uint32_t    clock() const = 0;
};

// =========================================================
//  ymfm_interface 実装 (no-op)
// =========================================================
class BasicYmfmInterface : public ymfm::ymfm_interface {
public:
    void    ymfm_set_timer(uint32_t, int32_t) override {}
    void    ymfm_sync_mode_write(uint8_t)     override {}
    void    ymfm_sync_check_interrupts()      override {}
    void    ymfm_update_irq(bool)             override {}
    uint8_t ymfm_external_read(ymfm::access_class, uint32_t) override { return 0; }
    void    ymfm_external_write(ymfm::access_class, uint32_t, uint8_t) override {}
};

// =========================================================
//  FmChipImpl<ChipImpl, TType>
// =========================================================
template<typename ChipImpl, ChipType TType>
class FmChipImpl final : public FmChip {
public:
    explicit FmChipImpl(uint32_t clock)
        : m_chip(m_iface, clock), m_clock(clock)
    {
        m_chip.reset();
        m_native_rate = m_chip.sample_rate(clock);
    }

    void write(uint32_t port, uint8_t reg, uint8_t value) override {
        // ネームスペーススコープの型トレイトを使用 (MSVC C3856 回避)
        if constexpr (detail::has_write_address_hi<ChipImpl>::value) {
            m_chip.write_address_hi(port != 0 ? 1 : 0);
        }
        m_chip.write(reg, value);
    }

    void generate(float* out_l, float* out_r, uint32_t dst_samples) override {
        m_resampler.process(
            [this](float* l, float* r, uint32_t n){ generateNative(l, r, n); },
            out_l, out_r, dst_samples);
    }

    void setTargetRate(uint32_t target_rate) override {
        m_target_rate = target_rate;
        m_resampler.setup(m_native_rate, target_rate);
    }

    uint32_t    nativeRate() const override { return m_native_rate; }
    ChipType    type()       const override { return TType; }
    uint32_t    clock()      const override { return m_clock; }
    const char* name()       const override;

private:
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
template<> inline const char* FmChipImpl<ymfm::y8950,   ChipType::Y8950 >::name() const { return "Y8950";         }
template<> inline const char* FmChipImpl<ymfm::ym3526,  ChipType::OPL   >::name() const { return "OPL (YM3526)";  }
template<> inline const char* FmChipImpl<ymfm::ym3812,  ChipType::OPL2  >::name() const { return "OPL2 (YM3812)"; }
template<> inline const char* FmChipImpl<ymfm::ymf262,  ChipType::OPL3  >::name() const { return "OPL3 (YMF262)"; }
template<> inline const char* FmChipImpl<ymfm::ymf278b, ChipType::OPL4  >::name() const { return "OPL4 (YMF278B)";}
template<> inline const char* FmChipImpl<ymfm::ym2203,  ChipType::OPN   >::name() const { return "OPN (YM2203)";  }
template<> inline const char* FmChipImpl<ymfm::ym2608,  ChipType::OPNA  >::name() const { return "OPNA (YM2608)"; }
template<> inline const char* FmChipImpl<ymfm::ym2610,  ChipType::OPNB  >::name() const { return "OPNB (YM2610)"; }
template<> inline const char* FmChipImpl<ymfm::ym2610b, ChipType::OPNBB >::name() const { return "OPNBB (YM2610B)";}
template<> inline const char* FmChipImpl<ymfm::ym2612,  ChipType::OPN2  >::name() const { return "OPN2 (YM2612)"; }
template<> inline const char* FmChipImpl<ymfm::ym2151,  ChipType::OPM   >::name() const { return "OPM (YM2151)";  }
template<> inline const char* FmChipImpl<ymfm::ym2413,  ChipType::OPLL  >::name() const { return "OPLL (YM2413)"; }
template<> inline const char* FmChipImpl<ymfm::ymf281,  ChipType::OPLLP >::name() const { return "OPLLP (YMF281)";}
template<> inline const char* FmChipImpl<ymfm::ym2423,  ChipType::OPLLX >::name() const { return "OPLLX (YM2423)";}
template<> inline const char* FmChipImpl<ymfm::ym2414,  ChipType::OPZ   >::name() const { return "OPZ (YM2414)";  }
template<> inline const char* FmChipImpl<ymfm::ds1001,  ChipType::VRC7  >::name() const { return "VRC7 (DS1001)"; }

// =========================================================
//  ファクトリ関数
// =========================================================
inline std::unique_ptr<FmChip> createChip(ChipType type, uint32_t clock = 0) {
    auto resolve = [](uint32_t c, uint32_t def) { return c ? c : def; };
    switch (type) {
        case ChipType::Y8950:  return std::make_unique<FmChipImpl<ymfm::y8950,   ChipType::Y8950 >>(resolve(clock, FmClock::Y8950));
        case ChipType::OPL:    return std::make_unique<FmChipImpl<ymfm::ym3526,  ChipType::OPL   >>(resolve(clock, FmClock::OPL));
        case ChipType::OPL2:   return std::make_unique<FmChipImpl<ymfm::ym3812,  ChipType::OPL2  >>(resolve(clock, FmClock::OPL2));
        case ChipType::OPL3:   return std::make_unique<FmChipImpl<ymfm::ymf262,  ChipType::OPL3  >>(resolve(clock, FmClock::OPL3));
        case ChipType::OPL4:   return std::make_unique<FmChipImpl<ymfm::ymf278b, ChipType::OPL4  >>(resolve(clock, FmClock::OPL4));
        case ChipType::OPN:    return std::make_unique<FmChipImpl<ymfm::ym2203,  ChipType::OPN   >>(resolve(clock, FmClock::OPN));
        case ChipType::OPNA:   return std::make_unique<FmChipImpl<ymfm::ym2608,  ChipType::OPNA  >>(resolve(clock, FmClock::OPNA));
        case ChipType::OPNB:   return std::make_unique<FmChipImpl<ymfm::ym2610,  ChipType::OPNB  >>(resolve(clock, FmClock::OPNB));
        case ChipType::OPNBB:  return std::make_unique<FmChipImpl<ymfm::ym2610b, ChipType::OPNBB >>(resolve(clock, FmClock::OPNBB));
        case ChipType::OPN2:   return std::make_unique<FmChipImpl<ymfm::ym2612,  ChipType::OPN2  >>(resolve(clock, FmClock::OPN2));
        case ChipType::OPM:    return std::make_unique<FmChipImpl<ymfm::ym2151,  ChipType::OPM   >>(resolve(clock, FmClock::OPM));
        case ChipType::OPLL:   return std::make_unique<FmChipImpl<ymfm::ym2413,  ChipType::OPLL  >>(resolve(clock, FmClock::OPLL));
        case ChipType::OPLLP:  return std::make_unique<FmChipImpl<ymfm::ymf281,  ChipType::OPLLP >>(resolve(clock, FmClock::OPLLP));
        case ChipType::OPLLX:  return std::make_unique<FmChipImpl<ymfm::ym2423,  ChipType::OPLLX >>(resolve(clock, FmClock::OPLLX));
        case ChipType::OPZ:    return std::make_unique<FmChipImpl<ymfm::ym2414,  ChipType::OPZ   >>(resolve(clock, FmClock::OPZ));
        case ChipType::VRC7:   return std::make_unique<FmChipImpl<ymfm::ds1001,  ChipType::VRC7  >>(resolve(clock, FmClock::VRC7));
    }
    return nullptr;
}
