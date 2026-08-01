#pragma once
// FmEngine.h
// 複数の FmChip を管理し、レジスタ書き込み API と
// オーディオコールバック向けのサンプル生成 API を提供する。
//
// MSVC 対応:
//   constexpr 関数内で std::pow() は使えない (C3615)。
//   dBToLinear() を通常の inline static 関数にする。

#include "FmChip.h"
#include <atomic>
#include <array>
#include <memory>
#include <vector>
#include <cassert>
#include <cmath>    // std::pow (実行時呼び出し用)

// =========================================================
//  SPSC コマンドキュー (lock-free)
// =========================================================
struct RegWriteCmd {
    uint32_t chip_id;
    uint32_t port;
    uint8_t  reg;
    uint8_t  value;
};

template<typename T, size_t Cap>
class SpscQueue {
public:
    bool push(const T& item) {
        const auto head = m_head.load(std::memory_order_relaxed);
        const auto next = (head + 1) % Cap;
        if (next == m_tail.load(std::memory_order_acquire))
            return false;
        m_buf[head] = item;
        m_head.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire))
            return false;
        out = m_buf[tail];
        m_tail.store((tail + 1) % Cap, std::memory_order_release);
        return true;
    }
private:
    std::array<T, Cap> m_buf{};
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
};

// =========================================================
//  ChipGain – チップごとのミキシングゲイン
// =========================================================
struct ChipGain {
    std::atomic<float> gain_l{1.0f};
    std::atomic<float> gain_r{1.0f};

    // dB → 線形スケール変換
    // ※ MSVC では std::pow が constexpr でないため constexpr にできない (C3615)。
    //    inline static 関数として定義する。
    static inline float dBToLinear(float dB) {
        return std::pow(10.0f, dB / 20.0f);
    }

    ChipGain() = default;
    ChipGain(const ChipGain& o)
        : gain_l(o.gain_l.load()), gain_r(o.gain_r.load()) {}
};

// =========================================================
//  FmEngine
// =========================================================
class FmEngine {
public:
    explicit FmEngine(uint32_t sample_rate = 44100)
        : m_sample_rate(sample_rate) {}

    // チップ追加 (ChipType 版)。clock=0 で標準クロックを使用。
    uint32_t addChip(ChipType type, uint32_t clock = 0) {
        auto chip = createChip(type, clock);
        chip->setTargetRate(m_sample_rate);
        return registerChip(std::move(chip));
    }

    // チップ追加 (文字列版)。未知の名前なら UINT32_MAX を返す。
    uint32_t addChipByName(const char* name, uint32_t clock = 0) {
        auto chip = createChipByName(name, clock);
        if (!chip) return UINT32_MAX;
        chip->setTargetRate(m_sample_rate);
        return registerChip(std::move(chip));
    }

    // 対応チップ数
    uint32_t supportedChipCount() const {
        return chipTableSize();
    }

    // index 番目の対応チップ名。範囲外は nullptr
    const char* supportedChipName(uint32_t index) const {
        return chipNameByIndex(index);
    }

    uint32_t sampleRate() const { return m_sample_rate; }
    size_t   chipCount()  const { return m_chips.size(); }

    // ゲイン設定 (任意スレッドから呼べる)
    void setGain(uint32_t chip_id, float gain_l, float gain_r) {
        assert(chip_id < m_gains.size());
        m_gains[chip_id]->gain_l.store(gain_l, std::memory_order_relaxed);
        m_gains[chip_id]->gain_r.store(gain_r, std::memory_order_relaxed);
    }
    void setGain(uint32_t chip_id, float gain) { setGain(chip_id, gain, gain); }

    float getGainL(uint32_t chip_id) const {
        assert(chip_id < m_gains.size());
        return m_gains[chip_id]->gain_l.load(std::memory_order_relaxed);
    }
    float getGainR(uint32_t chip_id) const {
        assert(chip_id < m_gains.size());
        return m_gains[chip_id]->gain_r.load(std::memory_order_relaxed);
    }

    void getGain(uint32_t chip_id, float& out_l, float& out_r) const {
        assert(chip_id < m_gains.size());
        out_l = m_gains[chip_id]->gain_l.load(std::memory_order_relaxed);
        out_r = m_gains[chip_id]->gain_r.load(std::memory_order_relaxed);
    }

    uint32_t nativeRate(uint32_t chip_id) const {
        if (chip_id >= m_chips.size()) return 0;
        return m_chips[chip_id]->nativeRate();
    }

    const char* getChipName(uint32_t chip_id) const {
        if (chip_id >= m_chips.size()) return nullptr;
        return m_chips[chip_id]->name();
    }

    // 外部メモリ設定 (ADPCM/PCM ROM/RAM)
    // chip_id: addChip() で取得した ID
    // access_type: ymfm::ACCESS_ADPCM_A(1) / ACCESS_ADPCM_B(2) / ACCESS_PCM(3)
    // data: ROM データへのポインタ (呼び出し元が寿命を管理すること)
    // size: データサイズ (バイト)
    // ※ オーディオスレッド起動前に呼ぶこと (スレッドセーフではない)
    void setMemory(uint32_t chip_id, int access_type,
                   const uint8_t* data, uint32_t size) {
        assert(chip_id < m_chips.size());
        m_chips[chip_id]->setMemory(
            static_cast<ymfm::access_class>(access_type), data, size);
    }

    uint32_t getMemorySize(uint32_t chip_id, int access_type) const {
        if (chip_id >= m_chips.size()) return 0;
        return m_chips[chip_id]->memorySize(
            static_cast<ymfm::access_class>(access_type));
    }

    // レジスタ書き込み (任意スレッドから呼べる)
    void write(uint32_t chip_id, uint8_t reg, uint8_t value, uint32_t port = 0) {
        assert(chip_id < m_chips.size());
        m_queue.push({chip_id, port, reg, value});
    }

    // サンプル生成 (オーディオスレッドから呼ぶ)
    void generate(float* out_l, float* out_r, uint32_t samples) {
        // 1. キュー消化。
        //    キーオン/オフに関係するビットが実際に変化した書き込み
        //    (FmChip::keyOnTransitionSlot() が非負を返す書き込み) のうち、
        //    「同じチャンネルへの変化が直前に未観測のまま溜まっている」場合
        //    にだけ、適用前に最低1サンプル生成してチップの内部クロックを
        //    1tick進める。
        //
        //    ymfm の m_keyon_live (fm_operator::keyonoff) はレジスタ書き込み時に
        //    即座に更新されるが、実際にエンベロープジェネレータへ反映される
        //    (clock_keystate 経由で start_attack/start_release が呼ばれる) のは
        //    次の prepare() = 次のサンプル生成時のみ。まとめてキューを吐き出して
        //    から N サンプルを一括生成すると、バッチ内の中間状態 (例: 直前の音の
        //    キーオフ→次の音のキーオン) が一度も prepare() に観測されず、
        //    ノートオンが無音のまま消えることがある。
        //
        //    ただし「異なるチャンネル」同士の変化は互いに衝突しない (例: 和音で
        //    多数のチャンネルが同時にキーオンする場合、まとめて適用しても
        //    問題ない — むしろ本来同時に鳴るべき音なので、まとめて適用する方が
        //    正しい)。そのため keyOnTransitionMask() が返すチャンネルスロットの
        //    ビットマスクごとに「直前のtick以降、未観測の変化があるか」を
        //    m_keyDirtyMask (チップごとの64bitビットマスク) で追跡し、本当に
        //    同じチャンネルが再度変化しようとしたときにだけティックを強制する。
        //    これにより OPL/OPLL 系のビブラートだけでなく、和音や (OPL/OPLL の
        //    ビルトインリズム音源のように1レジスタに複数の打楽器チャンネルが
        //    同居しているケースを含め) 多チャンネル同時変化でも不要な1サンプル
        //    生成を避けられる。
        std::fill(m_keyDirtyMask.begin(), m_keyDirtyMask.end(), uint64_t{0});

        uint32_t produced = 0;
        RegWriteCmd cmd;
        while (m_queue.pop(cmd)) {
            FmChip& chip = *m_chips[cmd.chip_id];
            const uint64_t mask = chip.keyOnTransitionMask(cmd.port, cmd.reg, cmd.value);
            if (mask != 0) {
                if ((m_keyDirtyMask[cmd.chip_id] & mask) != 0 && produced + 1 < samples) {
                    // 同じチャンネルへの変化が未観測のまま重なる → 先に確定させる
                    mixSpan(out_l + produced, out_r + produced, 1);
                    ++produced;
                    std::fill(m_keyDirtyMask.begin(), m_keyDirtyMask.end(), uint64_t{0});
                }
                chip.write(cmd.port, cmd.reg, cmd.value);
                m_keyDirtyMask[cmd.chip_id] |= mask;
            } else {
                chip.write(cmd.port, cmd.reg, cmd.value);
            }
        }

        // 2. 残りをまとめて生成
        if (produced < samples) {
            mixSpan(out_l + produced, out_r + produced, samples - produced);
        }

        // 3. ソフトクリップ (バッファ全体に対して1回)
        for (uint32_t s = 0; s < samples; ++s) {
            out_l[s] = softClip(out_l[s]);
            out_r[s] = softClip(out_r[s]);
        }
    }

    const FmChip* chip(uint32_t id) const {
        if (id < m_chips.size()) return m_chips[id].get();
        return nullptr;
    }

private:
    uint32_t registerChip(std::unique_ptr<FmChip> chip) {
        const uint32_t id = static_cast<uint32_t>(m_chips.size());
        m_chips.push_back(std::move(chip));
        m_gains.push_back(std::make_unique<ChipGain>());
        m_work_bufs.emplace_back();
        m_keyDirtyMask.push_back(0);
        return id;
    }

    static float softClip(float x) {
        if (x >  1.5f) return  1.0f;
        if (x < -1.5f) return -1.0f;
        return x * (1.0f - (x * x) / 9.0f);
    }

    // 区間 [out_l, out_l+count) に対して、全チップ生成→ゲイン付きミックスを
    // 行う (クリアも含む)。ソフトクリップは呼び出し元でまとめて行う。
    void mixSpan(float* out_l, float* out_r, uint32_t count) {
        std::fill(out_l, out_l + count, 0.0f);
        std::fill(out_r, out_r + count, 0.0f);

        assert(m_chips.size() == m_gains.size());
        assert(m_chips.size() == m_work_bufs.size());
        for (size_t i = 0; i < m_chips.size(); ++i) {
            WorkBuf& wb = m_work_bufs[i];
            wb.l.resize(count);
            wb.r.resize(count);

            m_chips[i]->generate(wb.l.data(), wb.r.data(), count);

            const float gl = m_gains[i]->gain_l.load(std::memory_order_relaxed);
            const float gr = m_gains[i]->gain_r.load(std::memory_order_relaxed);
            for (uint32_t s = 0; s < count; ++s) {
                out_l[s] += wb.l[s] * gl;
                out_r[s] += wb.r[s] * gr;
            }
        }
    }

    struct WorkBuf {
        std::vector<float> l;
        std::vector<float> r;
    };

    uint32_t                             m_sample_rate;
    std::vector<std::unique_ptr<FmChip>>     m_chips;
    std::vector<std::unique_ptr<ChipGain>>   m_gains;   // unique_ptr: atomic は vector 再確保でムーブ不可
    std::vector<WorkBuf>                     m_work_bufs;
    SpscQueue<RegWriteCmd, 4096>             m_queue;
    std::vector<uint64_t>                    m_keyDirtyMask; // generate() 内でのみ使用 (チップごとの未観測キーオン変化スロット)
};
