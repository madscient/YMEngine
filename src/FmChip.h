#pragma once
// FmChip.h
// ymfm コアのラッパー。チップ種別ごとの抽象インターフェースと
// ymfm_interface 実装を提供する。
// チップのネイティブサンプルレートとエンジンレートの差は LinearResampler で吸収する。
//
// MSVC 対応:
//   has_write_address_hi をクラスメンバーテンプレートとして定義すると
//   MSVC C3856/C3858 が発生する。namespace detail に移動することで回避する。
//
// 依存: ymfm (https://github.com/aaronsgiles/ymfm)
//       C++17以上

#include "ymfm_opl.h"
#include "ymfm_opn.h"
#include "ymfm_opm.h"
#include "ymfm_opz.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include <array>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <type_traits>

// =========================================================
//  チップ種別列挙
// =========================================================
enum class ChipType {
    Y8950,  // Y8950 (OPL expansion for MSX)
    OPL,    // YM3526 (OPL, used in early Adlib cards)
    OPL2,   // YM3812 (Adlib, Sound Blaster 1.x, etc.)
    OPL3,   // YMF262 (Sound Blaster 16, etc.)
    OPL4,   // YMF278B (OPL4)
    OPN,    // YM2203 (NEC PC-8801mkIISR, PC-9801, etc.)
    OPNA,   // YM2608 (NEC PC-8801mkIISR, PC-9801, etc.)
    OPNB,   // YM2610 (NEO GEO, etc.)
    OPNBB,  // YM2610B (TAITO)
    OPN2,   // YM2612 (Mega Drive, FM TOWNS, etc.)
    OPM,    // YM2151 (SFG-01/05, arcade)
    OPLL,   // YM2413 (MSX2+, Sega Master System, etc.)
    OPLLP,  // YMF281 (Pachinko, Pachislo)
    OPLLX,  // YM2423 (FM Melody Maker, PMC100, etc.)
    OPZ,    // YM2414 (TX81Z)
    VRC7,   // DS1001 (Lagrange Point)
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
    constexpr uint32_t OPL4   = 33'868'800;  // YMF278B 標準クロック (FM sr ≈ 49516 Hz)
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
//
//  MSVC C3856/C3858 回避:
//    クラステンプレートのメンバーとして template 特殊化を書くと
//    MSVC は「現在のスコープでは再宣言できません」エラーを出す。
//    名前空間スコープ (namespace detail) に置くことで回避する。
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
//  LinearResampler<Channels>
//
//  ソースレートは src_num / src_den Hz の分数で受け取る。OPN 系の FM は
//  clock/144 のように整数にならないため。
//
//  チップは呼び出しをまたいで連続した1本の列を出すので、ソースは必要な分
//  だけ生成し、まだ使い終わっていないサンプル (補間の左端と、アップサンプル
//  時は右端も) を m_carry に持ち越す。生成したサンプルを捨てると、捨てた分
//  だけチップの時間が先に進み、音程がずれて呼び出しの境目で波形が跳ぶ。
// =========================================================
template<size_t Channels>
class LinearResampler {
public:
    void setup(uint64_t src_num, uint32_t src_den, uint32_t dst_rate) {
        m_src_num   = src_num;
        m_src_den   = src_den;
        m_dst_rate  = dst_rate;
        m_phase_inc = (src_num << 32) / (static_cast<uint64_t>(src_den) * dst_rate);
        // m_carry は残す。実行中にレートが変わったとき (OPN 系の prescale) に
        // 直前の値から続けるため。
        m_phase     = 0;
    }

    bool isPassthrough() const {
        return m_src_num == static_cast<uint64_t>(m_src_den) * m_dst_rate;
    }

    // generate_fn(float* const* bufs, uint32_t n): bufs[c] に n サンプル書く
    template<typename GenFn>
    void process(GenFn&& generate_fn, float* const* out, uint32_t dst_samples) {
        if (dst_samples == 0) return;
        if (isPassthrough()) {
            generate_fn(out, dst_samples);
            return;
        }

        // 位置 p (32.32 固定小数) は m_carry の先頭を 0 番とした添字。
        // 補間には floor(p) と floor(p)+1 が要る。
        const uint64_t last_pos = m_phase + static_cast<uint64_t>(dst_samples - 1) * m_phase_inc;
        const uint64_t end_pos  = m_phase + static_cast<uint64_t>(dst_samples) * m_phase_inc;
        // end_pos の整数部は次回の先頭になるので、補間に使わなくても生成する
        const uint32_t last_idx = (std::max)(static_cast<uint32_t>(last_pos >> 32) + 1,
                                             static_cast<uint32_t>(end_pos >> 32));
        const uint32_t total    = last_idx + 1;
        const uint32_t fresh    = (total > m_carry_n) ? total - m_carry_n : 0;

        float* gen_bufs[Channels];
        for (size_t c = 0; c < Channels; ++c) {
            m_work[c].resize((std::max)(total, m_carry_n));
            for (uint32_t j = 0; j < m_carry_n; ++j) m_work[c][j] = m_carry[c][j];
            gen_bufs[c] = m_work[c].data() + m_carry_n;
        }
        if (fresh > 0) generate_fn(gen_bufs, fresh);

        uint64_t p = m_phase;
        for (uint32_t di = 0; di < dst_samples; ++di) {
            const uint32_t i    = static_cast<uint32_t>(p >> 32);
            const float    frac = static_cast<float>(p & 0xFFFFFFFFull) * (1.0f / 4294967296.0f);
            for (size_t c = 0; c < Channels; ++c) {
                const float* w = m_work[c].data();
                out[c][di] = w[i] + (w[i + 1] - w[i]) * frac;
            }
            p += m_phase_inc;
        }

        // [consumed, 手元の末尾] を持ち越す。last_idx <= consumed + 1 なので最大2個
        const uint32_t consumed = static_cast<uint32_t>(end_pos >> 32);
        const uint32_t have     = (std::max)(total, m_carry_n);
        m_carry_n = have - consumed;
        assert(m_carry_n >= 1 && m_carry_n <= 2);
        for (size_t c = 0; c < Channels; ++c)
            for (uint32_t j = 0; j < m_carry_n; ++j) m_carry[c][j] = m_work[c][consumed + j];
        m_phase = end_pos - (static_cast<uint64_t>(consumed) << 32);
    }

private:
    uint64_t m_src_num = 0;
    uint32_t m_src_den = 1, m_dst_rate = 0;
    uint64_t m_phase_inc = 0, m_phase = 0;
    // 初期状態はソースの -1 番目に 0 があるものとして始める
    std::array<std::array<float, 2>, Channels> m_carry{};
    uint32_t                                   m_carry_n = 1;
    std::array<std::vector<float>, Channels>   m_work;
};

// =========================================================
//  出力の部位
//  チップが別々の端子から出す出力。実機ではボード上の回路で混ぜたり、一部の
//  端子だけを配線したりするので、部位ごとにゲインを掛けられるようにする。
//  番号はチップをまたいで重ならない。別のチップの部位を渡されたときに、
//  黙って別の出力を変えずに拒否できるようにするため。
//  出力が1本のチップは部位を持たない。
// =========================================================
enum class ChipPart : uint32_t {
    OPN_FM      = 0,  // OPN/OPNA/OPNB/OPNBB: FM (ADPCM・リズムを含む)
    OPN_SSG     = 1,  //                      SSG
    OPLL_MELODY = 2,  // OPLL/OPLLP/OPLLX/VRC7: メロディ
    OPLL_RHYTHM = 3,  //                       リズム
    OPL3_AB     = 4,  // OPL3: 出力 A (L) / B (R)
    OPL3_CD     = 5,  //       出力 C (L) / D (R)
    OPL4_DO0    = 6,  // OPL4: DO0 (FM の C/D)
    OPL4_DO1    = 7,  //       DO1 (AWM の C/D)
    OPL4_DO2    = 8,  //       DO2 (FM の A/B と AWM の A/B をチップ内で混ぜたもの)
};
constexpr uint32_t kChipPartCount = 9;

// C/D 側は既定で混ぜない。FM の出力先 (C0 の bit4-7) を全部立てたチャンネルは
// A/B と C/D に同じ音を出すので、混ぜると二重に足される。AWM の DO1 もそろえる。
inline float defaultPartGain(ChipPart part) {
    switch (part) {
        case ChipPart::OPL3_CD:
        case ChipPart::OPL4_DO0:
        case ChipPart::OPL4_DO1: return 0.0f;
        default:                 return 1.0f;
    }
}

// l/r は部位ごとのゲイン (チップのゲイン × 部位のゲイン)。
// chip_l/chip_r はチップのゲインで、部位を持たないチップが使う。
struct PartGains {
    float l[kChipPartCount];
    float r[kChipPartCount];
    float chip_l;
    float chip_r;
};

// =========================================================
//  OPN 系の FM と SSG を別々に clock する派生クラス
//
//  ymfm の generate() は FM と SSG を1本の列にまとめるため、同じ値を繰り返して
//  速い方のレートに揃える (ymfm_opn.h の "A note about prescaling and sample
//  rates")。ここでは上流の generate() を呼ばず、protected の clock 関数を直接
//  呼んで、それぞれ本来のレートで1サンプルずつ取り出す。
//
//  clockFm(l, r)   : FM (+ADPCM/リズム) を1サンプル進める
//  clockSsg()      : SSG を1サンプル進め、3チャンネルの和を返す
//  fmDivider()     : FM のレート = 入力クロック / fmDivider()
//  ssgDivider()    : SSG のレート = 入力クロック / ssgDivider()
//
//  SSG の和の係数は上流の ssg_resampler に合わせる (OPN はそのまま、
//  OPNA/OPNB は 2/3)。上流の generate() と同じ値になることは
//  _test/opn_split_test.cpp で確かめる。
// =========================================================
namespace detail {

class Ym2203Split : public ymfm::ym2203 {
public:
    using ymfm::ym2203::ym2203;
    void clockFm(int32_t& l, int32_t& r) {
        clock_fm();
        l = r = m_last_fm.data[0];
    }
    int32_t clockSsg() {
        ymfm::ssg_engine::output_data o;
        m_ssg.clock();
        m_ssg.output(o);
        return o.data[0] + o.data[1] + o.data[2];
    }
    // prescale 6/3/2 → FM /72,/36,/24。SSG は ssg_effective_clock() と同じく
    // prescale*2/3 (整数除算で 4/2/1) を使い /16,/8,/4
    uint32_t fmDivider()  const { return m_fm.clock_prescale() * 12; }
    uint32_t ssgDivider() const { return (m_fm.clock_prescale() * 2 / 3) * 4; }
};

class Ym2608Split : public ymfm::ym2608 {
public:
    using ymfm::ym2608::ym2608;
    void clockFm(int32_t& l, int32_t& r) {
        clock_fm_and_adpcm();
        l = m_last_fm.data[0];
        r = m_last_fm.data[1];
    }
    int32_t clockSsg() {
        ymfm::ssg_engine::output_data o;
        m_ssg.clock();
        m_ssg.output(o);
        return (o.data[0] + o.data[1] + o.data[2]) * 2 / 3;
    }
    // prescale 6/3/2 → FM /144,/72,/48、SSG /32,/16,/8
    uint32_t fmDivider()  const { return m_fm.clock_prescale() * 24; }
    uint32_t ssgDivider() const { return (m_fm.clock_prescale() * 2 / 3) * 8; }
};

// ym2610b は ym2610 の派生なので、同じ実装を基底だけ変えて使う
template<typename Base>
class Ym2610Split : public Base {
public:
    using Base::Base;
    void clockFm(int32_t& l, int32_t& r) {
        this->clock_fm_and_adpcm();
        l = this->m_last_fm.data[0];
        r = this->m_last_fm.data[1];
    }
    int32_t clockSsg() {
        ymfm::ssg_engine::output_data o;
        this->m_ssg.clock();
        this->m_ssg.output(o);
        return (o.data[0] + o.data[1] + o.data[2]) * 2 / 3;
    }
    // YM2610 は prescale を持たない
    uint32_t fmDivider()  const { return 144; }
    uint32_t ssgDivider() const { return 32; }
};

} // namespace detail

// =========================================================
//  FmChip インターフェース
// =========================================================
class FmChip {
public:
    virtual ~FmChip() = default;
    virtual void        write(uint32_t port, uint8_t reg, uint8_t value) = 0;
    // gains の部位ごとのゲインを掛けて足した結果を書く
    virtual void        generate(float* out_l, float* out_r, uint32_t dst_samples,
                                 const PartGains& gains) = 0;
    virtual void        setTargetRate(uint32_t target_rate) = 0;
    // FM 部のネイティブレート (端数切り捨て)。OPN/OPNA は prescale の
    // 書き込みで変わる
    virtual uint32_t    nativeRate() const = 0;
    virtual bool        hasPart(ChipPart part) const { return false; }
    virtual ChipType    type()  const = 0;
    virtual const char* name()  const = 0;
    virtual uint32_t    clock() const = 0;

    // 外部メモリの設定
    // access_type: ymfm::ACCESS_ADPCM_A / ACCESS_ADPCM_B / ACCESS_PCM
    // data: メモリデータへのポインタ (呼び出し元が寿命を管理すること)
    // size: データサイズ (バイト)
    virtual void        setMemory(ymfm::access_class access_type,
                                  const uint8_t* data, uint32_t size) {}
    virtual uint32_t    memorySize(ymfm::access_class access_type) const { return 0; }

    // このレジスタ書き込みで「キーオン/オフ状態が実際に変化した」チャンネルの
    // 集合を、チャンネルスロットのビットマスク (各ビットが1チャンネルに対応)
    // として返す。変化を伴わない (キーオンと無関係なレジスタ、またはキーオン
    // 関連ビットが前回と同じ) 場合は 0 を返す。
    // 呼ぶたびに直前に書き込まれた値を内部に記憶し、次回以降との差分検出に
    // 使う (副作用があるため const ではない)。
    //
    // ymfm はキーオン/オフの「有効ビット」(m_keyon_live) をレジスタ書き込み時に
    // 即座に更新するが、実際にエンベロープジェネレータへ反映する
    // (clock_keystate 経由で start_attack/start_release を呼ぶ) のは
    // 次のサンプル生成 (prepare()) のタイミングでのみ。そのため、同じ
    // generate() 呼び出し内で「同じチャンネル」がキーオフ→キーオンのように
    // 連続して書き込まれると、中間状態が一度も観測されないまま次の状態で
    // 上書きされ、ノートオンが無音のまま消えることがある。
    // FmEngine::generate() は、返されたビットマスクが「まだ観測されていない
    // (直前に同じチャンネルへの変化が未観測のまま溜まっている)」場合にだけ、
    // その書き込みを保留し、前の状態のまま一定時間 (約2ms) 生成してから適用する。
    // 異なるチャンネル同士 (和音等) は互いに衝突しないため、まとめて適用して
    // 問題ない — ビットを分けているのはそのため。
    //
    // OPL/OPLL 系のようにキーオンビットと F-Number 等が同一レジスタアドレスに
    // 同居しているチップでは、レジスタアドレスだけで判定すると無関係な
    // ビット (周波数等) の書き換えにまで反応してしまい、ビブラート等で
    // 過剰な性能劣化を招く。そのため実装側では「キーオンに関係するビットだけ」
    // を前回書き込み値とマスク比較すること。
    // また OPL/OPLL のリズム音源レジスタのように、1レジスタに複数の独立した
    // チャンネル (打楽器) が同居している場合は、実際に変化したビットごとに
    // 別々のチャンネルスロットを割り当てること (同一ドラムパターン内で複数の
    // 打楽器が同時にオン/オフしても、互いに衝突させないため)。
    virtual uint64_t    keyOnTransitionMask(uint32_t port, uint8_t reg, uint8_t value) { return 0; }
};

// =========================================================
//  MemoryYmfmInterface
//  外部メモリアクセスを実装した ymfm_interface。
//  ADPCM_A / ADPCM_B / PCM の 3種のメモリ領域を保持する。
// =========================================================
class MemoryYmfmInterface : public ymfm::ymfm_interface {
public:
    void    ymfm_set_timer(uint32_t, int32_t) override {}
    void    ymfm_sync_mode_write(uint8_t)     override {}
    void    ymfm_sync_check_interrupts()      override {}
    void    ymfm_update_irq(bool)             override {}

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override {
        const auto& mem = getRegion(type);
        if (mem.data && address < mem.size)
            return mem.data[address];
        return 0;
    }

    void ymfm_external_write(ymfm::access_class type,
                             uint32_t address, uint8_t data) override {
        auto& mem = getRegion(type);
        if (mem.writeable && mem.owned && address < mem.size)
            const_cast<uint8_t*>(mem.data)[address] = data;
    }

    // 外部 ROM/RAM をポインタで設定 (寿命は呼び出し元管理)
    void setMemory(ymfm::access_class type,
                   const uint8_t* data, uint32_t size) {
        auto& mem = getRegion(type);
        mem.data     = data;
        mem.size     = size;
        mem.owned    = false;
        mem.writeable = false;
    }

    // 書き込み可能 RAM を内部確保
    void allocMemory(ymfm::access_class type, uint32_t size) {
        auto& mem = getRegion(type);
        mem.buf.assign(size, 0);
        mem.data      = mem.buf.data();
        mem.size      = size;
        mem.owned     = true;
        mem.writeable = true;
    }

    uint32_t memorySize(ymfm::access_class type) const {
        return getRegion(type).size;
    }

private:
    struct MemRegion {
        const uint8_t*      data      = nullptr;
        uint32_t            size      = 0;
        bool                owned     = false;
        bool                writeable = false;
        std::vector<uint8_t> buf;
    };

    MemRegion m_adpcm_a;
    MemRegion m_adpcm_b;
    MemRegion m_pcm;

    MemRegion& getRegion(ymfm::access_class type) {
        switch (type) {
            case ymfm::ACCESS_ADPCM_A: return m_adpcm_a;
            case ymfm::ACCESS_ADPCM_B: return m_adpcm_b;
            case ymfm::ACCESS_PCM:     return m_pcm;
            default:                   return m_adpcm_b; // fallback
        }
    }
    const MemRegion& getRegion(ymfm::access_class type) const {
        return const_cast<MemoryYmfmInterface*>(this)->getRegion(type);
    }
};

// BasicYmfmInterface: メモリアクセス不要なチップ用の軽量版 (従来通り)
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
    // SSG を持つ OPN 系は FM と SSG を別々のレートで生成する (detail::*Split)
    static constexpr bool kSplit =
        TType == ChipType::OPN  || TType == ChipType::OPNA ||
        TType == ChipType::OPNB || TType == ChipType::OPNBB;
    static constexpr bool kOpll =
        TType == ChipType::OPLL  || TType == ChipType::OPLLP ||
        TType == ChipType::OPLLX || TType == ChipType::VRC7;
    static constexpr bool kOpl3 = TType == ChipType::OPL3;
    static constexpr bool kOpl4 = TType == ChipType::OPL4;
    // 部位を持つチップは、部位ごとの出力を別々に変換してから混ぜる。
    // 0/1 番は呼び出し元の out_l/out_r を兼ね、2 番以降は m_extra_out に置く
    static constexpr size_t kResampleChannels = kOpl4 ? 6 : (kOpl3 ? 4 : 2);

public:
    // コンストラクタ本体は全チップ分を下部で完全特殊化して定義する。
    // 汎用版は定義しない (全チップが特殊化されるため instantiate されない)。
    explicit FmChipImpl(uint32_t clock);

    void write(uint32_t port, uint8_t reg, uint8_t value) override {
        // ポートセットごとに2オフセット (アドレス/データ) を割り当てる。
        // ymfm 側の offset 分岐 (offset & N) に対応: port=0→0/1, port=1→2/3,
        // port=2→4/5, ... (例: ymf278b(OPL4) は port2 が PCM/波形レジスタ)
        const uint32_t addr_offset = port * 2;
        const uint32_t data_offset = addr_offset + 1;
        m_chip.write(addr_offset, reg);
        m_chip.write(data_offset, value);

        // prescale (reg 0x2D-0x2F) はアドレスの書き込みだけで切り替わる
        if constexpr (kSplit) {
            if (m_chip.fmDivider() != m_fm_div || m_chip.ssgDivider() != m_ssg_div)
                updateRates();
        }
    }

    void generate(float* out_l, float* out_r, uint32_t dst_samples,
                  const PartGains& g) override {
        if constexpr (kSplit) {
            constexpr size_t kFm  = static_cast<size_t>(ChipPart::OPN_FM);
            constexpr size_t kSsg = static_cast<size_t>(ChipPart::OPN_SSG);
            float* fm_out[2] = { out_l, out_r };
            m_resampler.process(
                [this](float* const* b, uint32_t n){ generateFmNative(b[0], b[1], n); },
                fm_out, dst_samples);
            m_ssg_out.resize(dst_samples);
            float* ssg_out[1] = { m_ssg_out.data() };
            m_ssg_resampler.process(
                [this](float* const* b, uint32_t n){ generateSsgNative(b[0], n); },
                ssg_out, dst_samples);
            for (uint32_t i = 0; i < dst_samples; ++i) {
                out_l[i] = out_l[i] * g.l[kFm] + m_ssg_out[i] * g.l[kSsg];
                out_r[i] = out_r[i] * g.r[kFm] + m_ssg_out[i] * g.r[kSsg];
            }
        } else {
            float* out[kResampleChannels] = { out_l, out_r };
            for (size_t c = 2; c < kResampleChannels; ++c) {
                m_extra_out[c - 2].resize(dst_samples);
                out[c] = m_extra_out[c - 2].data();
            }
            m_resampler.process(
                [this](float* const* b, uint32_t n){ generateNative(b, n); },
                out, dst_samples);
            mixParts(out, dst_samples, g);
        }
    }

    void setTargetRate(uint32_t target_rate) override {
        m_target_rate = target_rate;
        setupResamplers();
    }

    bool hasPart(ChipPart part) const override {
        switch (part) {
            case ChipPart::OPN_FM:
            case ChipPart::OPN_SSG:     return kSplit;
            case ChipPart::OPLL_MELODY:
            case ChipPart::OPLL_RHYTHM: return kOpll;
            case ChipPart::OPL3_AB:
            case ChipPart::OPL3_CD:     return kOpl3;
            case ChipPart::OPL4_DO0:
            case ChipPart::OPL4_DO1:
            case ChipPart::OPL4_DO2:    return kOpl4;
        }
        return false;
    }

    // 外部メモリ設定 (ROM/RAM ポインタを渡す場合)
    void setMemory(ymfm::access_class access_type,
                   const uint8_t* data, uint32_t size) override {
        m_iface.setMemory(access_type, data, size);
    }

    uint32_t memorySize(ymfm::access_class access_type) const override {
        return m_iface.memorySize(access_type);
    }

    // 直前にこのレジスタへ書き込まれた値と比較し、「キーオン/オフに関係する
    // ビットだけ」が実際に変化したかどうかを判定する。変化していなければ
    // (例: OPL系で F-Number だけが書き換わった場合) 0 を返し、
    // FmEngine::generate() 側での衝突判定の対象から外す。
    // 変化していれば、実際に変化したビットごとに対応するチャンネルスロットを
    // 割り当てたビットマスクを返す (リズム音源レジスタのように1レジスタに
    // 複数の独立したチャンネルが同居する場合、変化したチャンネルの分だけ
    // ビットが立つ)。
    //
    // チップファミリごとのキーオン関連ビット位置は keyBitMask() を、
    // チャンネルスロットの割り当ては keyChannelSlotMask() を参照。
    uint64_t keyOnTransitionMask(uint32_t port, uint8_t reg, uint8_t value) override {
        const uint8_t mask = keyBitMask(port, reg);
        if (mask == 0) return 0; // キーオンと無関係なレジスタ

        const size_t idx = shadowIndex(port, reg);
        const uint8_t prevMasked = m_lastKeyRegValue[idx] & mask;
        const uint8_t newMasked  = value & mask;
        m_lastKeyRegValue[idx] = value;
        if (prevMasked == newMasked) return 0; // 実際には変化していない

        const uint8_t changedBits = static_cast<uint8_t>(prevMasked ^ newMasked);
        return keyChannelSlotMask(port, reg, value, changedBits);
    }

    uint32_t    nativeRate() const override { return m_native_rate.load(std::memory_order_relaxed); }
    ChipType    type()       const override { return TType; }
    uint32_t    clock()      const override { return m_clock; }
    const char* name()       const override;

private:
    // コンストラクタの末尾と、OPN 系で prescale が変わったときに呼ぶ
    void updateRates() {
        if constexpr (kSplit) {
            m_fm_div  = m_chip.fmDivider();
            m_ssg_div = m_chip.ssgDivider();
            m_native_rate.store(m_clock / m_fm_div, std::memory_order_relaxed);
        } else {
            m_native_rate.store(m_chip.sample_rate(m_clock), std::memory_order_relaxed);
        }
        setupResamplers();
    }

    void setupResamplers() {
        if (m_target_rate == 0) return; // setTargetRate() 前
        if constexpr (kSplit) {
            m_resampler.setup(m_clock, m_fm_div, m_target_rate);
            m_ssg_resampler.setup(m_clock, m_ssg_div, m_target_rate);
        } else {
            m_resampler.setup(m_native_rate.load(std::memory_order_relaxed), 1, m_target_rate);
        }
    }

    void generateFmNative(float* out_l, float* out_r, uint32_t n) {
        constexpr float kScale = 1.0f / 32768.0f;
        for (uint32_t i = 0; i < n; ++i) {
            int32_t l, r;
            m_chip.clockFm(l, r);
            out_l[i] = static_cast<float>(l) * kScale;
            out_r[i] = static_cast<float>(r) * kScale;
        }
    }

    void generateSsgNative(float* out, uint32_t n) {
        constexpr float kScale = 1.0f / 32768.0f;
        for (uint32_t i = 0; i < n; ++i)
            out[i] = static_cast<float>(m_chip.clockSsg()) * kScale;
    }

    // b[0..kResampleChannels) に ymfm の出力を並べ替えて書く:
    //
    //   OPL4     : b[0..1]=DO2, b[2..3]=DO0 (FM の C/D), b[4..5]=DO1 (AWM の C/D)
    //              ymf278b::generate() は data[0..1]=DO0, [2..3]=DO1, [4..5]=DO2。
    //              DO2 を 0/1 番に置き、out_l/out_r にそのまま混ぜられるようにする
    //   OPL3     : b[0..3]=A/B/C/D
    //   OPLL 系  : b[0]=メロディ, b[1]=リズム
    //   OPM/OPN2 : b[0]=L, b[1]=R
    //   その他   : b[0]=b[1]=data[0]。OPL/OPL2/Y8950 は OUTPUTS=1 (リズムと
    //              ADPCM も data[0] に入る)。OPZ も data[0] だけを使う
    //
    //   (OPN/OPNA/OPNB/OPNBB はここを通らない。generateFmNative /
    //    generateSsgNative を参照)
    void generateNative(float* const* b, uint32_t n) {
        typename ChipImpl::output_data out_data{};
        constexpr float kScale = 1.0f / 32768.0f;
        constexpr uint32_t kOutputs =
            sizeof(out_data.data) / sizeof(out_data.data[0]);
        constexpr bool kStereo = TType == ChipType::OPM || TType == ChipType::OPN2;
        // 上流の出力の並びが変わったら、部位の割り当てを見直すまで通さない
        static_assert(!kOpl4 || kOutputs == 6, "ymf278b output layout changed");
        static_assert(!kOpl3 || kOutputs == 4, "ymf262 output layout changed");
        static_assert(!kOpll || kOutputs == 2, "opll output layout changed");
        static_assert(!kStereo || kOutputs == 2, "stereo output layout changed");

        const auto& d = out_data.data;
        for (uint32_t i = 0; i < n; ++i) {
            m_chip.generate(&out_data);
            if constexpr (kOpl4) {
                b[0][i] = static_cast<float>(d[4]) * kScale;
                b[1][i] = static_cast<float>(d[5]) * kScale;
                b[2][i] = static_cast<float>(d[0]) * kScale;
                b[3][i] = static_cast<float>(d[1]) * kScale;
                b[4][i] = static_cast<float>(d[2]) * kScale;
                b[5][i] = static_cast<float>(d[3]) * kScale;
            } else if constexpr (kOpl3 || kOpll || kStereo) {
                for (size_t c = 0; c < kResampleChannels; ++c)
                    b[c][i] = static_cast<float>(d[c]) * kScale;
            } else {
                b[0][i] = b[1][i] = static_cast<float>(d[0]) * kScale;
            }
        }
    }

    // generateNative() の並びに部位ごとのゲインを掛け、b[0]/b[1] (= out_l/out_r) に混ぜる
    void mixParts(float* const* b, uint32_t n, const PartGains& g) {
        constexpr auto at = [](ChipPart p) { return static_cast<size_t>(p); };
        if constexpr (kOpll) {
            const float ml = g.l[at(ChipPart::OPLL_MELODY)], mr = g.r[at(ChipPart::OPLL_MELODY)];
            const float rl = g.l[at(ChipPart::OPLL_RHYTHM)], rr = g.r[at(ChipPart::OPLL_RHYTHM)];
            for (uint32_t i = 0; i < n; ++i) {
                const float m = b[0][i], r = b[1][i];
                b[0][i] = m * ml + r * rl;
                b[1][i] = m * mr + r * rr;
            }
        } else if constexpr (kOpl3) {
            const float abl = g.l[at(ChipPart::OPL3_AB)], abr = g.r[at(ChipPart::OPL3_AB)];
            const float cdl = g.l[at(ChipPart::OPL3_CD)], cdr = g.r[at(ChipPart::OPL3_CD)];
            for (uint32_t i = 0; i < n; ++i) {
                b[0][i] = b[0][i] * abl + b[2][i] * cdl;
                b[1][i] = b[1][i] * abr + b[3][i] * cdr;
            }
        } else if constexpr (kOpl4) {
            const float d2l = g.l[at(ChipPart::OPL4_DO2)], d2r = g.r[at(ChipPart::OPL4_DO2)];
            const float d0l = g.l[at(ChipPart::OPL4_DO0)], d0r = g.r[at(ChipPart::OPL4_DO0)];
            const float d1l = g.l[at(ChipPart::OPL4_DO1)], d1r = g.r[at(ChipPart::OPL4_DO1)];
            for (uint32_t i = 0; i < n; ++i) {
                b[0][i] = b[0][i] * d2l + b[2][i] * d0l + b[4][i] * d1l;
                b[1][i] = b[1][i] * d2r + b[3][i] * d0r + b[5][i] * d1r;
            }
        } else {
            for (uint32_t i = 0; i < n; ++i) {
                b[0][i] *= g.chip_l;
                b[1][i] *= g.chip_r;
            }
        }
    }

    // ※ has_write_address_hi はクラス外 (namespace detail) で定義
    //    ここには型トレイトを一切書かない (MSVC C3856 回避)

    // (port, reg) に対する「キーオン関連ビットのマスク」を返す。
    // 0 ならキーオン/オフとは無関係なレジスタ。
    // ymfm の実レジスタマップに基づく (extern/ymfm/src の各 *_registers::write() /
    // ymfm_pcm.cpp の pcm_engine::write() を参照):
    //   OPN系  (OPN/OPNA/OPNB/OPNBB/OPN2) : port0 の reg 0x28 (レジスタ全体がコマンド、
    //                                       チャンネル選択+オペレータマスクで専用)
    //   OPM/OPZ                          : reg 0x08 (同上、専用レジスタ)
    //   OPL系  (OPL/OPL2/OPL3/Y8950)      : reg 0xB0-0xBF の bit5 (チャンネルキーオン、
    //                                       他ビットは block/F-Number 上位で無関係) /
    //                                       reg 0xBD の bit0-5 (リズム gate+楽器選択)
    //   OPL4                              : FM部(port0/1)は上記OPL系と同じ。
    //                                       AWM/PCM部(port2)は reg 0x68-0x7F の bit7
    //                                       (ymfm_pcm.h ch_keyon() 参照。同一レジスタの
    //                                       他ビットは damp/lfo_reset/panpot でキーオンとは
    //                                       無関係)。AWM側を見逃すと、同一オーディオバッファ
    //                                       内でのkeyoff→keyon連続書き込みが一度も観測され
    //                                       ず、ノートオンが無音のまま消える取りこぼしが
    //                                       発生する。
    //   OPLL系 (OPLL/OPLLP/OPLLX/VRC7)    : reg 0x20-0x2F の bit4 (チャンネルキーオン、
    //                                       他ビットは block/F-Number上位/sustainで無関係) /
    //                                       reg 0x0E の bit0-5 (リズム gate+楽器選択)
    //
    // OPL/OPLL 系はキーオンビットと F-Number 等が同一レジスタアドレスに同居する
    // ため、アドレスだけで判定すると無関係な周波数書き換え (ビブラート等) にまで
    // 反応し、過剰な性能劣化を招く。ビットマスクで絞ることでこれを避ける。
    static uint8_t keyBitMask(uint32_t port, uint8_t reg) {
        if constexpr (TType == ChipType::OPN  || TType == ChipType::OPNA ||
                      TType == ChipType::OPNB || TType == ChipType::OPNBB ||
                      TType == ChipType::OPN2) {
            return (port == 0 && reg == 0x28) ? 0xFF : 0;
        } else if constexpr (TType == ChipType::OPM || TType == ChipType::OPZ) {
            return (reg == 0x08) ? 0xFF : 0;
        } else if constexpr (TType == ChipType::OPL4) {
            if (port == 2) return (reg >= 0x68 && reg <= 0x7f) ? 0x80 : 0;
            return keyBitMaskOpl(reg);
        } else if constexpr (TType == ChipType::OPL  || TType == ChipType::OPL2 ||
                              TType == ChipType::OPL3 || TType == ChipType::Y8950) {
            return keyBitMaskOpl(reg);
        } else if constexpr (TType == ChipType::OPLL  || TType == ChipType::OPLLP ||
                              TType == ChipType::OPLLX || TType == ChipType::VRC7) {
            if (reg == 0x0e) return 0x3F;             // リズム: gate(bit5)+楽器選択(bit0-4)
            if ((reg & 0xf0) == 0x20) return 0x10;    // チャンネルキーオン: bit4
            return 0;
        } else {
            return 0;
        }
    }

    static uint8_t keyBitMaskOpl(uint8_t reg) {
        if (reg == 0xbd) return 0x3F;              // リズム: gate(bit5)+楽器選択(bit0-4)
        if ((reg & 0xf0) == 0xb0) return 0x20;     // チャンネルキーオン: bit5
        return 0;
    }

    // m_lastKeyRegValue 内のインデックス。port は 0-3 を想定 (現行チップは
    // 最大でも OPL4 の port2 まで)。それ以上の port は畳み込まれる (実害なし)。
    static size_t shadowIndex(uint32_t port, uint8_t reg) {
        constexpr size_t kShadowPorts = 4;
        return (static_cast<size_t>(port) % kShadowPorts) * 256 + reg;
    }

    // リズム音源レジスタ (OPL の 0xBD, OPLL の 0x0E) 用。
    // bit0-4 は5つの独立した打楽器 (BD/SD/TOM/TC/HH) のキー、bit5 はリズム
    // モード全体のマスターゲート。打楽器ごとに baseSlot..baseSlot+4 の
    // 個別スロットを割り当てることで、複数の打楽器が同時にオン/オフしても
    // 互いに衝突させない (通常のドラムパターンで毎回ティックが発生するのを防ぐ)。
    // ただしマスターゲート (bit5) 自体が変化した場合は、全打楽器の可聴性が
    // 一括で変わるため、5スロット全部を対象に含める。
    static uint64_t rhythmSlotMask(uint8_t changedBits, uint32_t baseSlot) {
        uint64_t result = 0;
        for (uint32_t b = 0; b < 5; ++b)
            if (changedBits & (1u << b)) result |= (uint64_t{1} << (baseSlot + b));
        if (changedBits & 0x20u) result |= (uint64_t{0x1F} << baseSlot);
        return result;
    }

    // keyBitMask() が非0 (=このレジスタ書き込みはキーオン関連) と判定した後に
    // 呼ばれる。実際に変化があったチャンネル (打楽器を含む) 分だけビットを
    // 立てたビットマスクを返す (FmEngine 側で「未観測のまま重なっていないか」
    // を追跡するのに使う)。
    //   OPN 系 (3ch)            : value の bit0-1 → チャンネル 0-2
    //   OPNA/OPNB/OPNBB/OPN2(6ch): value の bit0-1 + bit2*3 → チャンネル 0-5
    //   OPM/OPZ                 : value の bit0-2 → チャンネル 0-7
    //   OPL系 (0xB0-0xBF)        : reg 下位4bit + (port!=0 ? 9 : 0) → チャンネル 0-17
    //   OPL系 (0xBD, リズム)     : 打楽器ごとにスロット 18-22 (rhythmSlotMask 参照)
    //   OPL4 AWM (port2)        : reg-0x68 → チャンネル 0-23 をスロット32-55に配置
    //   OPLL系 (0x20-0x2F)       : reg 下位4bit → チャンネル 0-8
    //   OPLL系 (0x0E, リズム)    : 打楽器ごとにスロット 9-13 (rhythmSlotMask 参照)
    static uint64_t keyChannelSlotMask(uint32_t port, uint8_t reg, uint8_t value, uint8_t changedBits) {
        if constexpr (TType == ChipType::OPN) {
            return uint64_t{1} << (value & 0x03u);
        } else if constexpr (TType == ChipType::OPNA || TType == ChipType::OPNB ||
                              TType == ChipType::OPNBB || TType == ChipType::OPN2) {
            return uint64_t{1} << ((value & 0x03u) + ((value >> 2) & 0x01u) * 3u);
        } else if constexpr (TType == ChipType::OPM || TType == ChipType::OPZ) {
            return uint64_t{1} << (value & 0x07u);
        } else if constexpr (TType == ChipType::OPL4) {
            if (port == 2) return uint64_t{1} << (32u + (static_cast<uint32_t>(reg) - 0x68u));
            if (reg == 0xbd) return rhythmSlotMask(changedBits, 18u);
            return uint64_t{1} << ((reg & 0x0fu) + (port != 0 ? 9u : 0u));
        } else if constexpr (TType == ChipType::OPL  || TType == ChipType::OPL2 ||
                              TType == ChipType::OPL3 || TType == ChipType::Y8950) {
            if (reg == 0xbd) return rhythmSlotMask(changedBits, 18u);
            return uint64_t{1} << ((reg & 0x0fu) + (port != 0 ? 9u : 0u));
        } else if constexpr (TType == ChipType::OPLL  || TType == ChipType::OPLLP ||
                              TType == ChipType::OPLLX || TType == ChipType::VRC7) {
            if (reg == 0x0e) return rhythmSlotMask(changedBits, 9u);
            return uint64_t{1} << (reg & 0x0fu);
        } else {
            return 0;
        }
    }

    MemoryYmfmInterface m_iface;  // 外部メモリアクセス対応インターフェース
    ChipImpl           m_chip;
    uint32_t           m_clock;
    // prescale の書き込み (オーディオスレッド) で変わり、nativeRate() は任意スレッドから読まれる
    std::atomic<uint32_t> m_native_rate{0};
    uint32_t           m_target_rate = 0;
    LinearResampler<kResampleChannels> m_resampler;  // OPN 系では FM 用
    std::array<std::vector<float>, kResampleChannels - 2> m_extra_out;
    // 以下は OPN 系 (kSplit) だけが使う
    LinearResampler<1> m_ssg_resampler;
    std::vector<float> m_ssg_out;
    uint32_t           m_fm_div  = 0;
    uint32_t           m_ssg_div = 0;
    std::array<uint8_t, 4 * 256> m_lastKeyRegValue{}; // keyOnTransitionMask() 用の直前値キャッシュ
};

// =========================================================
//  name() 特殊化
//  各チップに対応する正しい ymfm 型を使うこと
// =========================================================
template<> inline const char* FmChipImpl<ymfm::y8950,   ChipType::Y8950 >::name() const { return "Y8950";          }
template<> inline const char* FmChipImpl<ymfm::ym3526,  ChipType::OPL   >::name() const { return "OPL (YM3526)";   }
template<> inline const char* FmChipImpl<ymfm::ym3812,  ChipType::OPL2  >::name() const { return "OPL2 (YM3812)";  }
template<> inline const char* FmChipImpl<ymfm::ymf262,  ChipType::OPL3  >::name() const { return "OPL3 (YMF262)";  }
template<> inline const char* FmChipImpl<ymfm::ymf278b, ChipType::OPL4  >::name() const { return "OPL4 (YMF278B)"; }
template<> inline const char* FmChipImpl<detail::Ym2203Split, ChipType::OPN >::name() const { return "OPN (YM2203)";   }
template<> inline const char* FmChipImpl<detail::Ym2608Split, ChipType::OPNA>::name() const { return "OPNA (YM2608)";  }
template<> inline const char* FmChipImpl<detail::Ym2610Split<ymfm::ym2610>,  ChipType::OPNB >::name() const { return "OPNB (YM2610)";  }
template<> inline const char* FmChipImpl<detail::Ym2610Split<ymfm::ym2610b>, ChipType::OPNBB>::name() const { return "OPNBB (YM2610B)";}
template<> inline const char* FmChipImpl<ymfm::ym2612,  ChipType::OPN2  >::name() const { return "OPN2 (YM2612)";  }
template<> inline const char* FmChipImpl<ymfm::ym2151,  ChipType::OPM   >::name() const { return "OPM (YM2151)";   }
template<> inline const char* FmChipImpl<ymfm::ym2413,  ChipType::OPLL  >::name() const { return "OPLL (YM2413)";  }
template<> inline const char* FmChipImpl<ymfm::ymf281,  ChipType::OPLLP >::name() const { return "OPLLP (YMF281)"; }
template<> inline const char* FmChipImpl<ymfm::ym2423,  ChipType::OPLLX >::name() const { return "OPLLX (YM2423)"; }
template<> inline const char* FmChipImpl<ymfm::ym2414,  ChipType::OPZ   >::name() const { return "OPZ (YM2414)";   }
template<> inline const char* FmChipImpl<ymfm::ds1001,  ChipType::VRC7  >::name() const { return "VRC7 (DS1001)";  }

// =========================================================
//  コンストラクタ完全特殊化
//
//  ymfm の全チップを調査した結果、(interface&, uint32_t clock) を取る
//  チップは存在しない。全16チップに特殊化が必要。
//
//  パターン A: (interface&) のみ
//    y8950, ym3526, ym3812, ymf262, ymf278b,
//    ym2203, ym2608, ym2610b, ym2612, ym2414
//    (OPN 系は detail::*Split 経由。コンストラクタは基底のものを継承する)
//
//  パターン B: (interface&, const uint8_t* instrument_data = nullptr)
//    ym2413, ym2423, ymf281, ds1001
//
//  パターン C: (interface&, opm_variant)
//    ym2151
//
//  パターン D: (interface&, uint8_t channel_mask = 0x36)
//    ym2610
// =========================================================

// マクロで繰り返しを省略
#define FMCHIP_SPEC_A(Cls, TType, Clk) template<> inline FmChipImpl<Cls, ChipType::TType>::FmChipImpl(uint32_t clock)     : m_chip(m_iface), m_clock(clock ? clock : FmClock::Clk) { m_chip.reset(); updateRates(); }

#define FMCHIP_SPEC_B(Cls, TType, Clk) template<> inline FmChipImpl<Cls, ChipType::TType>::FmChipImpl(uint32_t clock)     : m_chip(m_iface, static_cast<uint8_t const*>(nullptr))     , m_clock(clock ? clock : FmClock::Clk) { m_chip.reset(); updateRates(); }

// パターン A
FMCHIP_SPEC_A(ymfm::y8950,   Y8950,  Y8950)
FMCHIP_SPEC_A(ymfm::ym3526,  OPL,    OPL)
FMCHIP_SPEC_A(ymfm::ym3812,  OPL2,   OPL2)
FMCHIP_SPEC_A(ymfm::ymf262,  OPL3,   OPL3)
FMCHIP_SPEC_A(ymfm::ymf278b, OPL4,   OPL4)
FMCHIP_SPEC_A(detail::Ym2203Split, OPN,  OPN)
FMCHIP_SPEC_A(detail::Ym2608Split, OPNA, OPNA)
FMCHIP_SPEC_A(detail::Ym2610Split<ymfm::ym2610b>, OPNBB, OPNBB)
FMCHIP_SPEC_A(ymfm::ym2612,  OPN2,   OPN2)
FMCHIP_SPEC_A(ymfm::ym2414,  OPZ,    OPZ)

// パターン B
FMCHIP_SPEC_B(ymfm::ym2413, OPLL,  OPLL)
FMCHIP_SPEC_B(ymfm::ym2423, OPLLX, OPLLX)
FMCHIP_SPEC_B(ymfm::ymf281, OPLLP, OPLLP)
FMCHIP_SPEC_B(ymfm::ds1001, VRC7,  VRC7)

#undef FMCHIP_SPEC_A
#undef FMCHIP_SPEC_B

// パターン C: ym2151 (interface&) — public コンストラクタを使う
// (interface&, opm_variant) は protected のため直接呼べない
template<>
inline FmChipImpl<ymfm::ym2151, ChipType::OPM>::FmChipImpl(uint32_t clock)
    : m_chip(m_iface)
    , m_clock(clock ? clock : FmClock::OPM)
{ m_chip.reset(); updateRates(); }

// パターン D: ym2610 (interface&, uint8_t channel_mask = 0x36)
// clock を channel_mask として渡さないようデフォルト値で構築
template<>
inline FmChipImpl<detail::Ym2610Split<ymfm::ym2610>, ChipType::OPNB>::FmChipImpl(uint32_t clock)
    : m_chip(m_iface)
    , m_clock(clock ? clock : FmClock::OPNB)
{ m_chip.reset(); updateRates(); }

// =========================================================
//  ファクトリ関数 (ChipType 版)
// =========================================================
inline std::unique_ptr<FmChip> createChip(ChipType type, uint32_t clock = 0) {
    auto resolve = [](uint32_t c, uint32_t def) { return c ? c : def; };
    switch (type) {
        case ChipType::Y8950:  return std::make_unique<FmChipImpl<ymfm::y8950,   ChipType::Y8950 >>(resolve(clock, FmClock::Y8950));
        case ChipType::OPL:    return std::make_unique<FmChipImpl<ymfm::ym3526,  ChipType::OPL   >>(resolve(clock, FmClock::OPL));
        case ChipType::OPL2:   return std::make_unique<FmChipImpl<ymfm::ym3812,  ChipType::OPL2  >>(resolve(clock, FmClock::OPL2));
        case ChipType::OPL3:   return std::make_unique<FmChipImpl<ymfm::ymf262,  ChipType::OPL3  >>(resolve(clock, FmClock::OPL3));
        case ChipType::OPL4:   return std::make_unique<FmChipImpl<ymfm::ymf278b, ChipType::OPL4  >>(resolve(clock, FmClock::OPL4));
        case ChipType::OPN:    return std::make_unique<FmChipImpl<detail::Ym2203Split, ChipType::OPN >>(resolve(clock, FmClock::OPN));
        case ChipType::OPNA:   return std::make_unique<FmChipImpl<detail::Ym2608Split, ChipType::OPNA>>(resolve(clock, FmClock::OPNA));
        case ChipType::OPNB:   return std::make_unique<FmChipImpl<detail::Ym2610Split<ymfm::ym2610>,  ChipType::OPNB >>(resolve(clock, FmClock::OPNB));
        case ChipType::OPNBB:  return std::make_unique<FmChipImpl<detail::Ym2610Split<ymfm::ym2610b>, ChipType::OPNBB>>(resolve(clock, FmClock::OPNBB));
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

// =========================================================
//  文字列ベースファクトリ / チップ名列挙
//  (createChip の後に置くこと — createChipByName が createChip を呼ぶため)
// =========================================================

struct ChipEntry {
    const char* name;
    ChipType    type;
    uint32_t    defaultClock;
};

inline const ChipEntry* chipTable() {
    static const ChipEntry kTable[] = {
        { "Y8950",  ChipType::Y8950,  FmClock::Y8950  },
        { "OPL",    ChipType::OPL,    FmClock::OPL    },
        { "OPL2",   ChipType::OPL2,   FmClock::OPL2   },
        { "OPL3",   ChipType::OPL3,   FmClock::OPL3   },
        { "OPL4",   ChipType::OPL4,   FmClock::OPL4   },
        { "OPN",    ChipType::OPN,    FmClock::OPN    },
        { "OPNA",   ChipType::OPNA,   FmClock::OPNA   },
        { "OPNB",   ChipType::OPNB,   FmClock::OPNB   },
        { "OPNBB",  ChipType::OPNBB,  FmClock::OPNBB  },
        { "OPN2",   ChipType::OPN2,   FmClock::OPN2   },
        { "OPM",    ChipType::OPM,    FmClock::OPM    },
        { "OPLL",   ChipType::OPLL,   FmClock::OPLL   },
        { "OPLLP",  ChipType::OPLLP,  FmClock::OPLLP  },
        { "OPLLX",  ChipType::OPLLX,  FmClock::OPLLX  },
        { "OPZ",    ChipType::OPZ,    FmClock::OPZ    },
        { "VRC7",   ChipType::VRC7,   FmClock::VRC7   },
        { nullptr,  ChipType::Y8950,  0               },  // sentinel
    };
    return kTable;
}

inline uint32_t chipTableSize() {
    uint32_t n = 0;
    for (const ChipEntry* e = chipTable(); e->name; ++e) ++n;
    return n;
}

// 名前からチップを作成。未知の名前なら nullptr
inline std::unique_ptr<FmChip> createChipByName(const char* name, uint32_t clock = 0) {
    if (!name) return nullptr;
    for (const ChipEntry* e = chipTable(); e->name; ++e) {
        if (std::strcmp(e->name, name) == 0)
            return createChip(e->type, clock ? clock : e->defaultClock);
    }
    return nullptr;
}

// index 番目のチップ名を返す。範囲外は nullptr
inline const char* chipNameByIndex(uint32_t index) {
    const ChipEntry* e = chipTable();
    uint32_t i = 0;
    for (; e->name; ++e, ++i)
        if (i == index) return e->name;
    return nullptr;
}
