#pragma once
// WasapiOutput.h
// WASAPI リアルタイム出力
//
// MSVC 対応:
//   UniqueHandle (std::unique_ptr<void, HandleDeleter>) の定義を
//   クラス先頭付近に移動する。private セクション末尾での using 宣言は
//   直前の struct 定義と合わさって MSVC のパーサーが誤解析する。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include "FmEngine.h"
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK_HR(hr, msg) \
    if (FAILED(hr)) throw std::runtime_error(std::string(msg) \
        + " (HRESULT=" + std::to_string(static_cast<long>(hr)) + ")")

class WasapiOutput {
public:
    // -------------------------------------------------------
    //  HANDLE RAII ラッパー
    //  ※ MSVC C3927 回避のため private セクション末尾ではなく
    //    クラス先頭 (public より前) で定義する。
    // -------------------------------------------------------
    struct HandleDeleter {
        void operator()(HANDLE h) const {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    };
    using UniqueHandle = std::unique_ptr<std::remove_pointer<HANDLE>::type, HandleDeleter>;

    // -------------------------------------------------------
    //  コンストラクタ / デストラクタ
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
        SetThreadPriority(m_thread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    void stop() {
        if (!m_running.exchange(false)) return;
        if (m_thread.joinable()) {
            SetEvent(m_stopEvent.get());
            m_thread.join();
        }
        m_audioClient->Stop();
        m_audioClient->Reset();
    }

    uint32_t sampleRate() const { return m_sampleRate; }

private:
    void openDefaultDevice() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        CHECK_HR(
            CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&enumerator)),
            "MMDeviceEnumerator");
        CHECK_HR(
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device),
            "GetDefaultAudioEndpoint");
    }

    void initAudioClient() {
        CHECK_HR(
            m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                               nullptr,
                               reinterpret_cast<void**>(m_audioClient.GetAddressOf())),
            "Activate IAudioClient");

        WAVEFORMATEXTENSIBLE wfex{};
        wfex.Format.wFormatTag       = WAVE_FORMAT_EXTENSIBLE;
        wfex.Format.nChannels        = 2;
        wfex.Format.nSamplesPerSec   = m_engine.sampleRate();
        wfex.Format.wBitsPerSample   = 32;
        wfex.Format.nBlockAlign      = wfex.Format.nChannels
                                       * (wfex.Format.wBitsPerSample / 8);
        wfex.Format.nAvgBytesPerSec  = wfex.Format.nSamplesPerSec
                                       * wfex.Format.nBlockAlign;
        wfex.Format.cbSize           = sizeof(WAVEFORMATEXTENSIBLE)
                                       - sizeof(WAVEFORMATEX);
        wfex.Samples.wValidBitsPerSample = 32;
        wfex.dwChannelMask           = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wfex.SubFormat               = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        if (m_exclusive) {
            REFERENCE_TIME minPeriod = 0;
            m_audioClient->GetDevicePeriod(nullptr, &minPeriod);
            CHECK_HR(
                m_audioClient->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                    minPeriod, minPeriod,
                    reinterpret_cast<WAVEFORMATEX*>(&wfex), nullptr),
                "IAudioClient::Initialize (exclusive)");
        } else {
            CHECK_HR(
                m_audioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, flags,
                    20 * 10000, 0,
                    reinterpret_cast<WAVEFORMATEX*>(&wfex), nullptr),
                "IAudioClient::Initialize (shared)");
        }

        UINT32 bufFrames = 0;
        CHECK_HR(m_audioClient->GetBufferSize(&bufFrames), "GetBufferSize");
        m_bufferFrames = bufFrames;
        m_sampleRate   = wfex.Format.nSamplesPerSec;

        // イベントハンドル作成
        m_readyEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        m_stopEvent.reset (CreateEventW(nullptr, FALSE, FALSE, nullptr));
        CHECK_HR(m_audioClient->SetEventHandle(m_readyEvent.get()), "SetEventHandle");

        CHECK_HR(
            m_audioClient->GetService(IID_PPV_ARGS(&m_renderClient)),
            "GetService IAudioRenderClient");

        m_workL.resize(m_bufferFrames);
        m_workR.resize(m_bufferFrames);
    }

    void renderLoop() {
        HANDLE events[2] = { m_readyEvent.get(), m_stopEvent.get() };

        while (m_running.load(std::memory_order_relaxed)) {
            DWORD result = WaitForMultipleObjects(2, events, FALSE, 200);
            if (result == WAIT_OBJECT_0 + 1 || result == WAIT_FAILED) break;
            if (result == WAIT_TIMEOUT) continue;

            UINT32 padding = 0;
            if (!m_exclusive) {
                if (FAILED(m_audioClient->GetCurrentPadding(&padding))) break;
            }
            const UINT32 available = m_bufferFrames - padding;
            if (available == 0) continue;

            m_engine.generate(m_workL.data(), m_workR.data(), available);

            BYTE* pData = nullptr;
            if (FAILED(m_renderClient->GetBuffer(available, &pData))) break;

            float* dst = reinterpret_cast<float*>(pData);
            for (UINT32 i = 0; i < available; ++i) {
                dst[i * 2 + 0] = m_workL[i];
                dst[i * 2 + 1] = m_workR[i];
            }
            m_renderClient->ReleaseBuffer(available, 0);
        }
    }

    // メンバ変数
    FmEngine&                  m_engine;
    bool                       m_exclusive;
    uint32_t                   m_sampleRate   = 44100;
    UINT32                     m_bufferFrames = 0;

    ComPtr<IMMDevice>          m_device;
    ComPtr<IAudioClient>       m_audioClient;
    ComPtr<IAudioRenderClient> m_renderClient;

    UniqueHandle               m_readyEvent;
    UniqueHandle               m_stopEvent;

    std::thread                m_thread;
    std::atomic<bool>          m_running{false};

    std::vector<float>         m_workL;
    std::vector<float>         m_workR;
};

#undef CHECK_HR
