#pragma once
// FmChip.h
// ymfm コアのラッパー。チップ種別ごとの抽象インターフェースと
// ymfm_interface 実装を提供する。
//
// 変更履歴:
// v2: クロック周波数をコンストラクタで指定可能に。
//     チップのネイティブサンプルレートとエンジンレートが異なる場合、
//     線形補間リサンプラー (LinearResampler) で吸収する。
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
//  LinearResampler
// =========================================================
class LinearResampler {
public:
    void setup(uint32_t src_rate, uint32_t dst_rate) {
        m_src_rate  = src_rate;
        m_dst_rate  = dst_rate;
        m_phase_inc = (static_cast<uint64_t>(src_rate) << 32) / dst_rate;
        m_phase     = 0;
        m_work_l.clear();
        m_work_r.clear();
    }

    bool isPassthrough() const { return m_src_rate == m_dst_rate; }

    template<typename GenFn>
    void process(GenFn&& generate_fn, float* out_l, float* out_r, uint32_t dst_samples) {
        if (isPassthrough()) {
            generate_fn(out_l, out_r, dst_samples);
            return;
        }

        // 前回コール末尾からの持ち越しフェーズ整数部 + 今回分 + 余裕 2
        // consumed (= ループ後 m_phase>>32) <= src_needed を保証する
        const uint32_t phase_offset = static_cast<uint32_t>(m_phase >> 32);
        const uint32_t src_needed =
            phase_offset +
            static_cast<uint32_t>(
                (static_cast<uint64_t>(dst_samples) * m_src_rate) / m_dst_rate) + 2;

        m_work_l.resize(src_needed);
        m_work_r.resize(src_needed);
        generate_fn(m_work_l.data(), m_work_r.data(), src_needed);

        for (uint32_t di = 0; di < dst_samples; ++di) {
            const uint32_t int_part = static_cast<uint32_t>(m_phase >> 32);
            const float    frac     = static_cast<float>(m_phase & 0xFFFFFFFFull)
                                      * (1.0f / 4294967296.0f);

            // 範囲チェック付きでソースバッファを読む
            const uint32_t i0 = (int_part     < src_needed) ? int_part     : src_needed - 1;
            const uint32_t i1 = (int_part + 1 < src_needed) ? int_part + 1 : src_needed - 1;

            out_l[di] = m_work_l[i0] + (m_work_l[i1] - m_work_l[i0]) * frac;
            out_r[di] = m_work_r[i0] + (m_work_r[i1] - m_work_r[i0]) * frac;

            m_phase += m_phase_inc;
        }

        // 実際に消費したソースサンプル数だけフェーズ整数部を引く。
        // src_needed ではなく consumed を使うこと。
        // src_needed は +2 の余裕を含むため src_needed << 32 を引くと
        // uint64_t アンダーフローが発生する。
        const uint32_t consumed = static_cast<uint32_t>(m_phase >> 32);
        m_phase -= static_cast<uint64_t>(consumed) << 32;
        // 小数部のみ残り、次回の phase_offset は 0 になる
    }

private:
    uint32_t m_src_rate = 0, m_dst_rate = 0;
    uint64_t m_phase_inc = 0, m_phase = 0;
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

    // 外部メモリの設定
    // access_type: ymfm::ACCESS_ADPCM_A / ACCESS_ADPCM_B / ACCESS_PCM
    // data: メモリデータへのポインタ (呼び出し元が寿命を管理すること)
    // size: データサイズ (バイト)
    virtual void        setMemory(ymfm::access_class access_type,
                                  const uint8_t* data, uint32_t size) {}
    virtual uint32_t    memorySize(ymfm::access_class access_type) const { return 0; }

    // このレジスタ書き込みが「キーオン/オフ状態の実際の変化」を伴うかどうかを
    // 判定し、伴う場合はそのチャンネルを識別するスロット番号 (0以上) を返す。
    // 変化を伴わない (キーオンと無関係なレジスタ、またはキーオン関連ビットが
    // 前回と同じ) 場合は -1 を返す。
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
    // FmEngine::generate() は、返されたスロット番号が「まだ観測されていない
    // (直前に同じチャンネルへの変化が未観測のまま溜まっている)」場合にだけ、
    // 直前に最低1サンプル生成してチップの内部クロックを1tick進める。
    // 異なるチャンネル同士 (和音等) は互いに衝突しないため、まとめて適用して
    // 問題ない — スロット番号を分けているのはそのため。
    //
    // OPL/OPLL 系のようにキーオンビットと F-Number 等が同一レジスタアドレスに
    // 同居しているチップでは、レジスタアドレスだけで判定すると無関係な
    // ビット (周波数等) の書き換えにまで反応してしまい、ビブラート等で
    // 過剰な性能劣化を招く。そのため実装側では「キーオンに関係するビットだけ」
    // を前回書き込み値とマスク比較し、それが実際に変化した場合にのみ有効な
    // スロット番号を返すこと。
    virtual int32_t     keyOnTransitionSlot(uint32_t port, uint8_t reg, uint8_t value) { return -1; }
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
    // (例: OPL系で F-Number だけが書き換わった場合) -1 を返し、
    // FmEngine::generate() 側での強制1サンプル生成をスキップさせる。
    // 変化していれば、対象チャンネルを識別するスロット番号を返す
    // (異なるチャンネル同士の書き込みを FmEngine 側で区別できるようにするため)。
    //
    // チップファミリごとのキーオン関連ビット位置は keyBitMask() を、
    // チャンネルスロットの割り当ては keyChannelSlot() を参照。
    int32_t keyOnTransitionSlot(uint32_t port, uint8_t reg, uint8_t value) override {
        const uint8_t mask = keyBitMask(port, reg);
        if (mask == 0) return -1; // キーオンと無関係なレジスタ

        const size_t idx = shadowIndex(port, reg);
        const uint8_t prevMasked = m_lastKeyRegValue[idx] & mask;
        const uint8_t newMasked  = value & mask;
        m_lastKeyRegValue[idx] = value;
        if (prevMasked == newMasked) return -1; // 実際には変化していない

        return static_cast<int32_t>(keyChannelSlot(port, reg, value));
    }

    uint32_t    nativeRate() const override { return m_native_rate; }
    ChipType    type()       const override { return TType; }
    uint32_t    clock()      const override { return m_clock; }
    const char* name()       const override;

private:
    void generateNative(float* out_l, float* out_r, uint32_t n) {
        typename ChipImpl::output_data out_data{};
        constexpr float kScale = 1.0f / 32768.0f;
        constexpr uint32_t kOutputs =
            sizeof(out_data.data) / sizeof(out_data.data[0]);

        // 出力モードを TType で分類:
        //
        //   TrueStereo : OPM/OPN2/OPL3
        //                data[0]=L, data[1]=R
        //
        //   Opl4Stereo : OPL4 (OUTPUTS=6, ymfm_opl.cpp ymf278b::generate 参照)
        //                data[0..1]=DO0(FM ch2+3), data[2..3]=DO1(wave ch2+3),
        //                data[4..5]=DO2(FM ch0+1 と wave ch0+1 のミックス済み L/R)
        //                DO2 がチップのメイン出力なので data[4]/data[5] を使う
        //
        //   OpnaStereo : OPNA/OPNB/OPNBB
        //                data[0]=FM-L, data[1]=FM-R, data[2]=SSG(mix)
        //                out_l = data[0]+data[2], out_r = data[1]+data[2]
        //
        //   OpnMono    : OPN (IsOpnA=false)
        //                data[0]=FM, data[1]=SSG-A, data[2]=SSG-B, data[3]=SSG-C
        //                out_l = out_r = data[0]+data[1]+data[2]+data[3]
        //
        //   MixMono    : OPL/OPL2/Y8950/OPLL/OPLLP/OPLLX/VRC7
        //                data[0]=melody, data[1]=rhythm
        //                out_l = out_r = data[0]+data[1]
        //
        //   Mono       : OUTPUTS=1
        //                out_l = out_r = data[0]
        constexpr bool isTrueStereo =
            kOutputs >= 2 &&
            (TType == ChipType::OPM   ||
             TType == ChipType::OPN2  ||
             TType == ChipType::OPL3);
        constexpr bool isOpl4Stereo =
            kOutputs >= 6 &&
            TType == ChipType::OPL4;
        constexpr bool isOpnaStereo =
            TType == ChipType::OPNA  ||
            TType == ChipType::OPNB  ||
            TType == ChipType::OPNBB;
        constexpr bool isOpnMono =
            TType == ChipType::OPN;
        constexpr bool isMixMono =
            kOutputs >= 2 &&
            (TType == ChipType::OPL   ||
             TType == ChipType::OPL2  ||
             TType == ChipType::Y8950 ||
             TType == ChipType::OPLL  ||
             TType == ChipType::OPLLP ||
             TType == ChipType::OPLLX ||
             TType == ChipType::VRC7);

        for (uint32_t i = 0; i < n; ++i) {
            m_chip.generate(&out_data);
            if constexpr (isTrueStereo) {
                // OPM/OPN2/OPL3: data[0]=L, data[1]=R
                out_l[i] = static_cast<float>(out_data.data[0]) * kScale;
                out_r[i] = static_cast<float>(out_data.data[1]) * kScale;
            } else if constexpr (isOpl4Stereo) {
                // OPL4: DO2 (data[4]=L, data[5]=R) がミックス済みメイン出力
                out_l[i] = static_cast<float>(out_data.data[4]) * kScale;
                out_r[i] = static_cast<float>(out_data.data[5]) * kScale;
            } else if constexpr (isOpnaStereo) {
                // OPNA/OPNB/OPNBB: data[0]=FM-L, data[1]=FM-R, data[2]=SSG(mix)
                out_l[i] = static_cast<float>(out_data.data[0] + out_data.data[2]) * kScale;
                out_r[i] = static_cast<float>(out_data.data[1] + out_data.data[2]) * kScale;
            } else if constexpr (isOpnMono) {
                // OPN: data[0]=FM, data[1-3]=SSG(A,B,C) → 全合算
                const int32_t mix = out_data.data[0]
                    + (kOutputs > 1 ? out_data.data[1] : 0)
                    + (kOutputs > 2 ? out_data.data[2] : 0)
                    + (kOutputs > 3 ? out_data.data[3] : 0);
                out_l[i] = out_r[i] = static_cast<float>(mix) * kScale;
            } else if constexpr (isMixMono) {
                // OPL/OPLL 系: data[0]=melody, data[1]=rhythm → 合算
                out_l[i] = out_r[i] = static_cast<float>(
                    out_data.data[0] + out_data.data[1]) * kScale;
            } else {
                out_l[i] = out_r[i] =
                    static_cast<float>(out_data.data[0]) * kScale;
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
    //                                       発生する(2026年7月、繰り返しノートオンでAWMが
    //                                       徐々に無音化する不具合の調査で発覚)。
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

    // keyBitMask() が非0 (=このレジスタ書き込みはキーオン関連) と判定した後に
    // 呼ばれる。実際に変化があったチャンネルを識別する、チップ内で一意な
    // コンパクトな番号 (0-63、FmEngine 側で uint64_t のビットマスクとして
    // 「未観測のまま重なっていないか」を追跡するのに使う) を返す。
    //   OPN 系 (3ch)            : value の bit0-1 (チャンネル 0-2)
    //   OPNA/OPNB/OPNBB/OPN2(6ch): value の bit0-1 + bit2*3 (チャンネル 0-5)
    //   OPM/OPZ                 : value の bit0-2 (チャンネル 0-7)
    //   OPL系 (0xB0-0xBF/0xBD)   : reg 下位4bit + (port!=0 ? 9 : 0) (チャンネル
    //                              0-17)、0xBD (リズム) は固定スロット31
    //   OPL4 AWM (port2)        : reg-0x68 (チャンネル 0-23) をスロット32-55に配置
    //   OPLL系 (0x20-0x2F/0x0E)  : reg 下位4bit (チャンネル 0-8)、0x0E (リズム)
    //                              は固定スロット31
    static uint32_t keyChannelSlot(uint32_t port, uint8_t reg, uint8_t value) {
        if constexpr (TType == ChipType::OPN) {
            return value & 0x03u;
        } else if constexpr (TType == ChipType::OPNA || TType == ChipType::OPNB ||
                              TType == ChipType::OPNBB || TType == ChipType::OPN2) {
            return (value & 0x03u) + ((value >> 2) & 0x01u) * 3u;
        } else if constexpr (TType == ChipType::OPM || TType == ChipType::OPZ) {
            return value & 0x07u;
        } else if constexpr (TType == ChipType::OPL4) {
            if (port == 2) return 32u + (static_cast<uint32_t>(reg) - 0x68u);
            if (reg == 0xbd) return 31u;
            return (reg & 0x0fu) + (port != 0 ? 9u : 0u);
        } else if constexpr (TType == ChipType::OPL  || TType == ChipType::OPL2 ||
                              TType == ChipType::OPL3 || TType == ChipType::Y8950) {
            if (reg == 0xbd) return 31u;
            return (reg & 0x0fu) + (port != 0 ? 9u : 0u);
        } else if constexpr (TType == ChipType::OPLL  || TType == ChipType::OPLLP ||
                              TType == ChipType::OPLLX || TType == ChipType::VRC7) {
            if (reg == 0x0e) return 31u;
            return reg & 0x0fu;
        } else {
            return 0u;
        }
    }

    MemoryYmfmInterface m_iface;  // 外部メモリアクセス対応インターフェース
    ChipImpl           m_chip;
    uint32_t           m_clock;
    uint32_t           m_native_rate = 0;
    uint32_t           m_target_rate = 0;
    LinearResampler    m_resampler;
    std::array<uint8_t, 4 * 256> m_lastKeyRegValue{}; // keyOnTransitionSlot() 用の直前値キャッシュ
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
template<> inline const char* FmChipImpl<ymfm::ym2203,  ChipType::OPN   >::name() const { return "OPN (YM2203)";   }
template<> inline const char* FmChipImpl<ymfm::ym2608,  ChipType::OPNA  >::name() const { return "OPNA (YM2608)";  }
template<> inline const char* FmChipImpl<ymfm::ym2610,  ChipType::OPNB  >::name() const { return "OPNB (YM2610)";  }
template<> inline const char* FmChipImpl<ymfm::ym2610b, ChipType::OPNBB >::name() const { return "OPNBB (YM2610B)";}
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
#define FMCHIP_SPEC_A(Cls, TType, Clk) \
template<> inline FmChipImpl<ymfm::Cls, ChipType::TType>::FmChipImpl(uint32_t clock) \
    : m_chip(m_iface), m_clock(clock ? clock : FmClock::Clk) \
{ m_chip.reset(); m_native_rate = m_chip.sample_rate(m_clock); }

#define FMCHIP_SPEC_B(Cls, TType, Clk) \
template<> inline FmChipImpl<ymfm::Cls, ChipType::TType>::FmChipImpl(uint32_t clock) \
    : m_chip(m_iface, static_cast<uint8_t const*>(nullptr)) \
    , m_clock(clock ? clock : FmClock::Clk) \
{ m_chip.reset(); m_native_rate = m_chip.sample_rate(m_clock); }

// パターン A
FMCHIP_SPEC_A(y8950,   Y8950,  Y8950)
FMCHIP_SPEC_A(ym3526,  OPL,    OPL)
FMCHIP_SPEC_A(ym3812,  OPL2,   OPL2)
FMCHIP_SPEC_A(ymf262,  OPL3,   OPL3)
FMCHIP_SPEC_A(ymf278b, OPL4,   OPL4)
FMCHIP_SPEC_A(ym2203,  OPN,    OPN)
FMCHIP_SPEC_A(ym2608,  OPNA,   OPNA)
FMCHIP_SPEC_A(ym2610b, OPNBB,  OPNBB)
FMCHIP_SPEC_A(ym2612,  OPN2,   OPN2)
FMCHIP_SPEC_A(ym2414,  OPZ,    OPZ)

// パターン B
FMCHIP_SPEC_B(ym2413, OPLL,  OPLL)
FMCHIP_SPEC_B(ym2423, OPLLX, OPLLX)
FMCHIP_SPEC_B(ymf281, OPLLP, OPLLP)
FMCHIP_SPEC_B(ds1001, VRC7,  VRC7)

#undef FMCHIP_SPEC_A
#undef FMCHIP_SPEC_B

// パターン C: ym2151 (interface&) — public コンストラクタを使う
// (interface&, opm_variant) は protected のため直接呼べない
template<>
inline FmChipImpl<ymfm::ym2151, ChipType::OPM>::FmChipImpl(uint32_t clock)
    : m_chip(m_iface)
    , m_clock(clock ? clock : FmClock::OPM)
{ m_chip.reset(); m_native_rate = m_chip.sample_rate(m_clock); }

// パターン D: ym2610 (interface&, uint8_t channel_mask = 0x36)
// clock を channel_mask として渡さないようデフォルト値で構築
template<>
inline FmChipImpl<ymfm::ym2610, ChipType::OPNB>::FmChipImpl(uint32_t clock)
    : m_chip(m_iface)
    , m_clock(clock ? clock : FmClock::OPNB)
{ m_chip.reset(); m_native_rate = m_chip.sample_rate(m_clock); }

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
