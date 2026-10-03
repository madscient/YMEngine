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

    // チップ追加 (ChipType 版)。clock はマスタークロック (Hz)。0 なら UINT32_MAX を返す。
    uint32_t addChip(ChipType type, uint32_t clock) {
        auto chip = createChip(type, clock);
        if (!chip) return UINT32_MAX;
        chip->setTargetRate(m_sample_rate);
        return registerChip(std::move(chip));
    }

    // チップ追加 (文字列版)。未知の名前か clock が 0 なら UINT32_MAX を返す。
    uint32_t addChipByName(const char* name, uint32_t clock) {
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

    // 部位ごとのゲイン (任意スレッドから呼べる)。実際に掛かるのは
    // setGain() のゲイン × 部位のゲイン。チップが持たない部位なら false
    // (出力が1本のチップは部位を持たない)。既定値は defaultPartGain()。
    bool setPartGain(uint32_t chip_id, ChipPart part, float gain_l, float gain_r) {
        if (chip_id >= m_chips.size() || !m_chips[chip_id]->hasPart(part)) return false;
        ChipGain& g = (*m_part_gains[chip_id])[static_cast<size_t>(part)];
        g.gain_l.store(gain_l, std::memory_order_relaxed);
        g.gain_r.store(gain_r, std::memory_order_relaxed);
        return true;
    }

    bool getPartGain(uint32_t chip_id, ChipPart part, float& out_l, float& out_r) const {
        if (chip_id >= m_chips.size() || !m_chips[chip_id]->hasPart(part)) return false;
        const ChipGain& g = (*m_part_gains[chip_id])[static_cast<size_t>(part)];
        out_l = g.gain_l.load(std::memory_order_relaxed);
        out_r = g.gain_r.load(std::memory_order_relaxed);
        return true;
    }

    // チップが持つ部位の列挙と、名前からの検索 (C API は部位を名前で受け取る)。
    // 部位を持たないチップと未知の chip_id は、数が 0、名前が nullptr、検索が false。
    uint32_t partCount(uint32_t chip_id) const {
        if (chip_id >= m_chips.size()) return 0;
        return m_chips[chip_id]->partCount();
    }
    const char* partName(uint32_t chip_id, uint32_t index) const {
        if (chip_id >= m_chips.size()) return nullptr;
        return m_chips[chip_id]->partName(index);
    }
    bool findPart(uint32_t chip_id, const char* name, ChipPart& out) const {
        if (chip_id >= m_chips.size()) return false;
        return m_chips[chip_id]->findPart(name, out);
    }

    uint32_t nativeRate(uint32_t chip_id) const {
        if (chip_id >= m_chips.size()) return 0;
        return m_chips[chip_id]->nativeRate();
    }

    const char* getChipName(uint32_t chip_id) const {
        if (chip_id >= m_chips.size()) return nullptr;
        return m_chips[chip_id]->name();
    }

    // チップが持つ外部メモリの列挙と、名前からの検索。部位と同じ形
    uint32_t memoryCount(uint32_t chip_id) const {
        if (chip_id >= m_chips.size()) return 0;
        return m_chips[chip_id]->memoryCount();
    }
    const char* memoryName(uint32_t chip_id, uint32_t index) const {
        if (chip_id >= m_chips.size()) return nullptr;
        return m_chips[chip_id]->memoryName(index);
    }
    bool findMemory(uint32_t chip_id, const char* name, ChipMemoryType& out) const {
        if (chip_id >= m_chips.size()) return false;
        return m_chips[chip_id]->findMemory(name, out);
    }

    // 外部メモリ。どれもオーディオスレッド起動前に呼ぶこと (スレッドセーフではない)。
    // data は割り当てを外すかエンジンを破棄するまで解放しないこと。
    //
    // mapMemory: C API の FmEngine_SetMemoryEx と同じ。type のメモリの
    // [base, base + size) に data を割り当てる。data が nullptr なら、その範囲と
    // 重なる割り当てをすべて外す。未知の chip_id、チップが持たない type、
    // size が 0、範囲が 2^32 を越える、既存の割り当てと重なる、未知の access
    // なら false。
    bool mapMemory(uint32_t chip_id, ChipMemoryType type, uint32_t base,
                   uint8_t* data, uint32_t size, ChipMemoryAccess access) {
        if (chip_id >= m_chips.size() || !m_chips[chip_id]->hasMemory(type)) return false;
        if (!data) return m_chips[chip_id]->unmapMemory(type, base, size);
        if (access != ChipMemoryAccess::ROM && access != ChipMemoryAccess::RAM) return false;
        return m_chips[chip_id]->mapMemory(type, base, data, size, access);
    }

    // C API の FmEngine_SetMemory と同じ。type の割り当てを [0, size) の data
    // だけにする。チップの書き込みは捨てる。未知の chip_id、チップが持たない
    // type、data が nullptr、size が 0 なら false。
    bool setMemory(uint32_t chip_id, ChipMemoryType type,
                   const uint8_t* data, uint32_t size) {
        if (chip_id >= m_chips.size() || !m_chips[chip_id]->hasMemory(type)) return false;
        if (!data || size == 0) return false;
        m_chips[chip_id]->setMemory(type, data, size);
        return true;
    }

    // 割り当てたブロックの大きさの合計
    uint32_t getMemorySize(uint32_t chip_id, ChipMemoryType type) const {
        if (chip_id >= m_chips.size()) return 0;
        return m_chips[chip_id]->memorySize(type);
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
        //    (FmChip::keyOnTransitionMask() が非0を返す書き込み) のうち、
        //    「同じチャンネルへの変化が直前に未観測のまま溜まっている」場合
        //    にだけ、適用前に前の状態のまま minKeyOnTickSamples() 分を生成する。
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
        //    ビットマスクごとに「最後の生成以降、未観測の変化があるか」を
        //    m_keyDirtyMask (チップごとの64bitビットマスク) で追跡し、同じ
        //    チャンネルが再び変化しようとしたら、その書き込みを m_pending に
        //    保留して前の状態のまま minKeyOnTickSamples() 分を生成してから
        //    適用する。OPL/OPLL 系のビブラートや和音、(OPL/OPLL のビルトイン
        //    リズム音源のように1レジスタに複数の打楽器チャンネルが同居している
        //    ケースを含め) 多チャンネル同時変化では分割しない。
        //    保留中は後続の書き込みも順序を保つため適用しない。
        //
        //    保留の生成は呼び出しをまたいで数える。今回の samples に収まらない
        //    分は次の呼び出しの頭で続きを生成してから適用する。そのため衝突が
        //    多い、または呼び出しが細かいと、書き込みの適用が遅れて累積する
        //    (衝突1回あたり最大 minKeyOnTickSamples())。未観測の状態を捨てずに
        //    観測させることを、発音タイミングの正確さより優先している。
        //    前の状態を1サンプルしか生成しないと、KEY OFF のリリースが聞こえる
        //    前に KEY ON で戻り、アタックも立ち上がる前に次の状態に切り替わり
        //    得るため、最低限の時間を確保する。
        const uint32_t minTick = minKeyOnTickSamples();
        uint32_t produced = 0;
        for (;;) {
            if (m_hasPending) {
                if (m_pendingHold > 0) {
                    const uint32_t room = samples - produced;
                    const uint32_t n = (room < m_pendingHold) ? room : m_pendingHold;
                    if (n == 0) break;
                    renderSpan(out_l + produced, out_r + produced, n);
                    produced += n;
                    m_pendingHold -= n;
                    if (m_pendingHold > 0) break;
                }
                m_chips[m_pending.chip_id]->write(m_pending.port, m_pending.reg, m_pending.value);
                m_keyDirtyMask[m_pending.chip_id] |= m_pendingMask;
                m_hasPending = false;
            }

            RegWriteCmd cmd;
            if (!m_queue.pop(cmd)) break;
            FmChip& chip = *m_chips[cmd.chip_id];
            // 直前値キャッシュを書き込み順に更新するため、保留する場合も
            // ここで1回だけ呼ぶ。
            const uint64_t mask = chip.keyOnTransitionMask(cmd.port, cmd.reg, cmd.value);
            if ((m_keyDirtyMask[cmd.chip_id] & mask) != 0) {
                m_pending     = cmd;
                m_pendingMask = mask;
                m_pendingHold = minTick;
                m_hasPending  = true;
                continue;
            }
            chip.write(cmd.port, cmd.reg, cmd.value);
            m_keyDirtyMask[cmd.chip_id] |= mask;
        }

        // 2. 残りをまとめて生成
        if (produced < samples) {
            renderSpan(out_l + produced, out_r + produced, samples - produced);
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
        auto parts = std::make_unique<PartGainSet>();
        for (uint32_t p = 0; p < kChipPartCount; ++p) {
            const float d = defaultPartGain(static_cast<ChipPart>(p));
            (*parts)[p].gain_l.store(d, std::memory_order_relaxed);
            (*parts)[p].gain_r.store(d, std::memory_order_relaxed);
        }
        m_part_gains.push_back(std::move(parts));
        m_work_bufs.emplace_back();
        m_keyDirtyMask.push_back(0);
        return id;
    }

    static float softClip(float x) {
        if (x >  1.5f) return  1.0f;
        if (x < -1.5f) return -1.0f;
        return x * (1.0f - (x * x) / 9.0f);
    }

    // キーオン衝突時に最低限確保するサンプル数 (約2ms相当)。
    // 1サンプルだけだとアタックエンベロープが立ち上がる前に次の衝突で
    // 切られ、事実上無音のノートになり得るため、最低限の可聴時間を確保する。
    uint32_t minKeyOnTickSamples() const {
        const uint32_t rate = m_sample_rate ? m_sample_rate : 44100;
        return (rate / 500 > 0) ? (rate / 500) : 1; // rate/500 ≈ 2ms分のサンプル数
    }

    // 1サンプルでも生成すれば、それまでに適用したキー状態は全チップで観測
    // 済みになる。未観測の追跡は呼び出しの境目ではなくここで打ち切る
    // (呼び出しの末尾で samples を使い切った後に適用した書き込みは、次の
    // 呼び出しの頭でもまだ未観測のため)。
    void renderSpan(float* out_l, float* out_r, uint32_t count) {
        mixSpan(out_l, out_r, count);
        if (count > 0)
            std::fill(m_keyDirtyMask.begin(), m_keyDirtyMask.end(), uint64_t{0});
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

            const float gl = m_gains[i]->gain_l.load(std::memory_order_relaxed);
            const float gr = m_gains[i]->gain_r.load(std::memory_order_relaxed);
            PartGains g;
            g.chip_l = gl;
            g.chip_r = gr;
            for (uint32_t p = 0; p < kChipPartCount; ++p) {
                const ChipGain& pg = (*m_part_gains[i])[p];
                g.l[p] = gl * pg.gain_l.load(std::memory_order_relaxed);
                g.r[p] = gr * pg.gain_r.load(std::memory_order_relaxed);
            }

            m_chips[i]->generate(wb.l.data(), wb.r.data(), count, g);

            for (uint32_t s = 0; s < count; ++s) {
                out_l[s] += wb.l[s];
                out_r[s] += wb.r[s];
            }
        }
    }

    struct WorkBuf {
        std::vector<float> l;
        std::vector<float> r;
    };

    using PartGainSet = std::array<ChipGain, kChipPartCount>;

    uint32_t                             m_sample_rate;
    std::vector<std::unique_ptr<FmChip>>     m_chips;
    std::vector<std::unique_ptr<ChipGain>>   m_gains;   // unique_ptr: atomic は vector 再確保でムーブ不可
    std::vector<std::unique_ptr<PartGainSet>> m_part_gains;
    std::vector<WorkBuf>                     m_work_bufs;
    SpscQueue<RegWriteCmd, 4096>             m_queue;
    // 以下は generate() (オーディオスレッド) からのみ触る
    std::vector<uint64_t>                    m_keyDirtyMask; // チップごとの未観測キーオン変化スロット
    RegWriteCmd                              m_pending{};
    uint64_t                                 m_pendingMask = 0;
    uint32_t                                 m_pendingHold = 0;
    bool                                     m_hasPending  = false;
};
