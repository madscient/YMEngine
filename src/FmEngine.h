#pragma once
// FmEngine.h
// 複数の FmChip を管理し、レジスタ書き込み API と
// オーディオコールバック向けのサンプル生成 API を提供する。
//
// MSVC 対応:
//   constexpr 関数内で std::pow() は使えない (C3615)。
//   dBToLinear() を通常の inline static 関数にする。

#include "FmChip.h"
#include "ExternalChip.h"
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

    // ymfmチップ追加。clock=0 で標準クロックを使用。
    uint32_t addChip(ChipType type, uint32_t clock = 0) {
        auto chip = createChip(type, clock);
        chip->setTargetRate(m_sample_rate);
        return registerChip(std::move(chip));
    }

    // 外部ライブラリチップ追加 (PSG/SN76489/SCC/SAA1099)
    uint32_t addExtChip(ChipTypeExt type, uint32_t clock = 0) {
        auto ext  = createExtChip(type, clock, m_sample_rate);
        auto chip = std::make_unique<ExtChipAdapter>(std::move(ext));
        return registerChip(std::move(chip));
    }

    uint32_t sampleRate() const { return m_sample_rate; }
    size_t   chipCount()  const { return m_chips.size(); }

    // ゲイン設定 (任意スレッドから呼べる)
    void setGain(uint32_t chip_id, float gain_l, float gain_r) {
        assert(chip_id < m_gains.size());
        m_gains[chip_id].gain_l.store(gain_l, std::memory_order_relaxed);
        m_gains[chip_id].gain_r.store(gain_r, std::memory_order_relaxed);
    }
    void setGain(uint32_t chip_id, float gain) { setGain(chip_id, gain, gain); }

    float getGainL(uint32_t chip_id) const {
        assert(chip_id < m_gains.size());
        return m_gains[chip_id].gain_l.load(std::memory_order_relaxed);
    }
    float getGainR(uint32_t chip_id) const {
        assert(chip_id < m_gains.size());
        return m_gains[chip_id].gain_r.load(std::memory_order_relaxed);
    }

    // レジスタ書き込み (任意スレッドから呼べる)
    void write(uint32_t chip_id, uint8_t reg, uint8_t value, uint32_t port = 0) {
        assert(chip_id < m_chips.size());
        m_queue.push({chip_id, port, reg, value});
    }

    // サンプル生成 (オーディオスレッドから呼ぶ)
    void generate(float* out_l, float* out_r, uint32_t samples) {
        // 1. キュー消化
        RegWriteCmd cmd;
        while (m_queue.pop(cmd)) {
            m_chips[cmd.chip_id]->write(cmd.port, cmd.reg, cmd.value);
        }

        // 2. バッファクリア
        std::fill(out_l, out_l + samples, 0.0f);
        std::fill(out_r, out_r + samples, 0.0f);

        // 3. 各チップ生成 → ゲイン付きミックス
        for (size_t i = 0; i < m_chips.size(); ++i) {
            WorkBuf& wb = m_work_bufs[i];
            wb.l.resize(samples);
            wb.r.resize(samples);

            m_chips[i]->generate(wb.l.data(), wb.r.data(), samples);

            const float gl = m_gains[i].gain_l.load(std::memory_order_relaxed);
            const float gr = m_gains[i].gain_r.load(std::memory_order_relaxed);
            for (uint32_t s = 0; s < samples; ++s) {
                out_l[s] += wb.l[s] * gl;
                out_r[s] += wb.r[s] * gr;
            }
        }

        // 4. ソフトクリップ
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
        m_gains.emplace_back();
        m_work_bufs.emplace_back();
        return id;
    }

    static float softClip(float x) {
        if (x >  1.5f) return  1.0f;
        if (x < -1.5f) return -1.0f;
        return x * (1.0f - (x * x) / 9.0f);
    }

    struct WorkBuf {
        std::vector<float> l;
        std::vector<float> r;
    };

    uint32_t                             m_sample_rate;
    std::vector<std::unique_ptr<FmChip>> m_chips;
    std::vector<ChipGain>                m_gains;
    std::vector<WorkBuf>                 m_work_bufs;
    SpscQueue<RegWriteCmd, 4096>         m_queue;
};
