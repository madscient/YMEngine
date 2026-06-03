#pragma once
// WasapiOutput.h
// WASAPI (Windows Audio Session API) を使ってリアルタイムに
// FmEngine の出力をサウンドデバイスへ送る。
//
// - Shared mode (排他モード非使用) でシンプルに動く
// - Exclusive mode を使う場合は setExclusive(true) を呼ぶ
// - float32 LPCM, ステレオ, 任意サンプルレート

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <comdef.h>
#include <wrl/client.h>

#include "FmEngine.h"
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include <functional>

using Microsoft::WRL::ComPtr;

// HR チェックマクロ
#define CHECK_HR(hr, msg) \
    if (FAILED(hr)) throw std::runtime_error(std::string(msg) + " (HRESULT=" + std::to_string(hr) + ")")

class WasapiOutput {
public:
    // -------------------------------------------------------
    //  コンストラクタ: デフォルト再生デバイスを開く
    // -------------------------------------------------------
    explicit WasapiOutput(FmEngine& engine, bool exclusive = false)
        : m_engine(engine), m_exclusive(exclusive)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr != RPC_E_CHANGED_MODE) CHECK_HR(hr, "CoInitialize");

        openDefaultDevice();
        initAudioClient();
    }

    ~WasapiOutput() {
        stop();
        CoUninitialize();
    }

    // -------------------------------------------------------
    //  再生開始 / 停止
    // -------------------------------------------------------
    void start() {
        if (m_running.exchange(true)) return;
        CHECK_HR(m_audioClient->Start(), "IAudioClient::Start");
        m_thread = std::thread(&WasapiOutput::renderLoop, this);
        // リアルタイム優先度
        SetThreadPriority(m_thread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    void stop() {
        if (!m_running.exchange(false)) return;
        if (m_thread.joinable()) {
            SetEvent(m_stopEvent.get()); // ループを起こして終了
            m_thread.join();
        }
        m_audioClient->Stop();
        m_audioClient->Reset();
    }

    uint32_t sampleRate() const { return m_sampleRate; }

private:
    // -------------------------------------------------------
    //  デバイスオープン
    // -------------------------------------------------------
    void openDefaultDevice() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        CHECK_HR(
            CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&enumerator)),
            "MMDeviceEnumerator"
        );
        CHECK_HR(
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device),
            "GetDefaultAudioEndpoint"
        );
    }

    // -------------------------------------------------------
    //  IAudioClient の初期化
    // -------------------------------------------------------
    void initAudioClient() {
        CHECK_HR(
            m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                               nullptr, reinterpret_cast<void**>(m_audioClient.GetAddressOf())),
            "Activate IAudioClient"
        );

        // float32 stereo フォーマットを要求
        WAVEFORMATEXTENSIBLE wfex{};
        wfex.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        wfex.Format.nChannels       = 2;
        wfex.Format.nSamplesPerSec  = m_engine.sampleRate();
        wfex.Format.wBitsPerSample  = 32;
        wfex.Format.nBlockAlign     = wfex.Format.nChannels * (wfex.Format.wBitsPerSample / 8);
        wfex.Format.nAvgBytesPerSec = wfex.Format.nSamplesPerSec * wfex.Format.nBlockAlign;
        wfex.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfex.Samples.wValidBitsPerSample = 32;
        wfex.dwChannelMask          = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wfex.SubFormat              = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        REFERENCE_TIME period = 0;

        if (m_exclusive) {
            // 排他モード: ハードウェアの最小周期を取得
            REFERENCE_TIME minPeriod = 0;
            m_audioClient->GetDevicePeriod(nullptr, &minPeriod);
            period = minPeriod;
            CHECK_HR(
                m_audioClient->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                    period, period,
                    reinterpret_cast<WAVEFORMATEX*>(&wfex), nullptr),
                "IAudioClient::Initialize (exclusive)"
            );
        } else {
            CHECK_HR(
                m_audioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, flags,
                    20 * 10000, 0, // 20ms バッファ
                    reinterpret_cast<WAVEFORMATEX*>(&wfex), nullptr),
                "IAudioClient::Initialize (shared)"
            );
        }

        // バッファサイズ記録
        UINT32 bufFrames = 0;
        CHECK_HR(m_audioClient->GetBufferSize(&bufFrames), "GetBufferSize");
        m_bufferFrames = bufFrames;
        m_sampleRate   = wfex.Format.nSamplesPerSec;

        // イベントハンドル
        m_readyEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        m_stopEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        CHECK_HR(m_audioClient->SetEventHandle(m_readyEvent.get()), "SetEventHandle");

        // RenderClient 取得
        CHECK_HR(
            m_audioClient->GetService(IID_PPV_ARGS(&m_renderClient)),
            "GetService IAudioRenderClient"
        );

        // 作業バッファ確保
        m_workL.resize(m_bufferFrames);
        m_workR.resize(m_bufferFrames);
    }

    // -------------------------------------------------------
    //  レンダリングループ (別スレッド)
    // -------------------------------------------------------
    void renderLoop() {
        HANDLE events[2] = {m_readyEvent.get(), m_stopEvent.get()};

        while (m_running.load(std::memory_order_relaxed)) {
            DWORD result = WaitForMultipleObjects(2, events, FALSE, 200);
            if (result == WAIT_OBJECT_0 + 1 || result == WAIT_FAILED) break;
            if (result == WAIT_TIMEOUT) continue;

            // 空きフレーム数を取得
            UINT32 padding = 0;
            if (!m_exclusive) {
                if (FAILED(m_audioClient->GetCurrentPadding(&padding))) break;
            }
            const UINT32 available = m_bufferFrames - padding;
            if (available == 0) continue;

            // FmEngine から生成
            m_engine.generate(m_workL.data(), m_workR.data(), available);

            // WASAPI バッファへ書き込み
            BYTE* pData = nullptr;
            if (FAILED(m_renderClient->GetBuffer(available, &pData))) break;

            auto* dst = reinterpret_cast<float*>(pData);
            for (UINT32 i = 0; i < available; ++i) {
                dst[i * 2 + 0] = m_workL[i];
                dst[i * 2 + 1] = m_workR[i];
            }
            m_renderClient->ReleaseBuffer(available, 0);
        }
    }

    // -------------------------------------------------------
    //  HANDLE の RAII ラッパー
    // -------------------------------------------------------
    struct HandleDeleter {
        void operator()(HANDLE h) { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    };
    using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

    // -------------------------------------------------------
    //  メンバ
    // -------------------------------------------------------
    FmEngine&               m_engine;
    bool                    m_exclusive;
    uint32_t                m_sampleRate   = 44100;
    UINT32                  m_bufferFrames = 0;

    ComPtr<IMMDevice>       m_device;
    ComPtr<IAudioClient>    m_audioClient;
    ComPtr<IAudioRenderClient> m_renderClient;

    UniqueHandle            m_readyEvent;
    UniqueHandle            m_stopEvent;

    std::thread             m_thread;
    std::atomic<bool>       m_running{false};

    std::vector<float>      m_workL;
    std::vector<float>      m_workR;
};

#undef CHECK_HR
