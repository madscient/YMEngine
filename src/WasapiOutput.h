#pragma once
// WasapiOutput.h
// WASAPI リアルタイム出力
//
// 設計方針:
//   AUTOCONVERTPCM に依存しない。
//   GetMixFormat でデバイスのネイティブフォーマットを取得して Initialize し、
//   renderLoop 内で FmEngine の float32 出力をデバイスフォーマットに自前変換する。
//   これにより複数デバイス環境や AUTOCONVERTPCM 非対応環境でも確実に動作する。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
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
#include <cstdint>
#include <algorithm>

using Microsoft::WRL::ComPtr;

#define CHECK_HR(hr, msg) \
    if (FAILED(hr)) throw std::runtime_error(std::string(msg) \
        + " (HRESULT=0x" + toHex(static_cast<uint32_t>(hr)) + ")")

static std::string toHex(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", v);
    return std::string(buf);
}

class WasapiOutput {
public:
    struct HandleDeleter {
        void operator()(HANDLE h) const {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    };
    using UniqueHandle = std::unique_ptr<std::remove_pointer<HANDLE>::type, HandleDeleter>;

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
    // -------------------------------------------------------
    //  デバイスフォーマット種別
    // -------------------------------------------------------
    enum class DevFmt { Float32, Int16, Int24, Int32, Unknown };

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
            m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(m_audioClient.GetAddressOf())),
            "Activate IAudioClient");

        // GetMixFormat でデバイスのネイティブフォーマットを取得する。
        // Exclusive / Shared どちらも基本フォーマットはここから得る。
        WAVEFORMATEX* pMixFormat = nullptr;
        CHECK_HR(m_audioClient->GetMixFormat(&pMixFormat), "GetMixFormat");

        m_devChannels  = pMixFormat->nChannels;
        m_devSampleRate = pMixFormat->nSamplesPerSec;
        m_devBitsPerSample = pMixFormat->wBitsPerSample;
        m_devBlockAlign = pMixFormat->nBlockAlign;

        // サブフォーマットを判定する
        m_devFmt = detectFormat(pMixFormat);

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        if (m_exclusive) {
            // Exclusive mode: デバイスフォーマットで IsFormatSupported を確認してから Initialize
            HRESULT hrSup = m_audioClient->IsFormatSupported(
                AUDCLNT_SHAREMODE_EXCLUSIVE, pMixFormat, nullptr);
            if (FAILED(hrSup)) {
                // 排他モード非対応 → Shared mode に降格
                m_exclusive = false;
            }
        }

        HRESULT hr;
        if (m_exclusive) {
            REFERENCE_TIME minPeriod = 0;
            m_audioClient->GetDevicePeriod(nullptr, &minPeriod);
            hr = m_audioClient->Initialize(
                AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                minPeriod, minPeriod, pMixFormat, nullptr);
        } else {
            hr = m_audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED, flags,
                20 * 10000, 0, pMixFormat, nullptr);
        }

        CoTaskMemFree(pMixFormat);
        CHECK_HR(hr, "IAudioClient::Initialize");

        UINT32 bufFrames = 0;
        CHECK_HR(m_audioClient->GetBufferSize(&bufFrames), "GetBufferSize");
        m_bufferFrames = bufFrames;

        m_readyEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        m_stopEvent.reset (CreateEventW(nullptr, FALSE, FALSE, nullptr));
        CHECK_HR(m_audioClient->SetEventHandle(m_readyEvent.get()), "SetEventHandle");
        CHECK_HR(m_audioClient->GetService(IID_PPV_ARGS(&m_renderClient)),
                 "GetService IAudioRenderClient");

        m_workL.resize(m_bufferFrames);
        m_workR.resize(m_bufferFrames);
        m_sampleRate = m_engine.sampleRate();
    }

    // -------------------------------------------------------
    //  フォーマット判定
    // -------------------------------------------------------
    DevFmt detectFormat(const WAVEFORMATEX* pFmt) {
        if (pFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return DevFmt::Float32;
        if (pFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pFmt);
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) return DevFmt::Float32;
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                switch (pFmt->wBitsPerSample) {
                    case 16: return DevFmt::Int16;
                    case 24: return DevFmt::Int24;
                    case 32: return DevFmt::Int32;
                }
            }
        }
        if (pFmt->wFormatTag == WAVE_FORMAT_PCM) {
            switch (pFmt->wBitsPerSample) {
                case 16: return DevFmt::Int16;
                case 24: return DevFmt::Int24;
                case 32: return DevFmt::Int32;
            }
        }
        return DevFmt::Unknown;
    }

    // -------------------------------------------------------
    //  float32 [-1,1] → デバイスフォーマットへの変換書き込み
    // -------------------------------------------------------
    static float clamp1(float v) {
        return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    }

    void writeToBuffer(BYTE* pData, UINT32 frames) {
        switch (m_devFmt) {
        case DevFmt::Float32: {
            float* dst = reinterpret_cast<float*>(pData);
            for (UINT32 i = 0; i < frames; ++i) {
                dst[i * m_devChannels + 0] = clamp1(m_workL[i]);
                dst[i * m_devChannels + 1] = clamp1(m_workR[i]);
                for (UINT32 ch = 2; ch < m_devChannels; ++ch)
                    dst[i * m_devChannels + ch] = 0.0f;
            }
            break;
        }
        case DevFmt::Int16: {
            int16_t* dst = reinterpret_cast<int16_t*>(pData);
            for (UINT32 i = 0; i < frames; ++i) {
                dst[i * m_devChannels + 0] = static_cast<int16_t>(clamp1(m_workL[i]) * 32767.0f);
                dst[i * m_devChannels + 1] = static_cast<int16_t>(clamp1(m_workR[i]) * 32767.0f);
                for (UINT32 ch = 2; ch < m_devChannels; ++ch)
                    dst[i * m_devChannels + ch] = 0;
            }
            break;
        }
        case DevFmt::Int24: {
            // 24bit は 3バイト/サンプル。ブロックアライメントから計算する
            const UINT32 bytesPerFrame = m_devBlockAlign;
            const UINT32 bytesPerSample = bytesPerFrame / m_devChannels;
            for (UINT32 i = 0; i < frames; ++i) {
                BYTE* frame = pData + i * bytesPerFrame;
                auto write24 = [](BYTE* p, float v) {
                    int32_t s = static_cast<int32_t>(
                        std::max(-1.0f, std::min(1.0f, v)) * 8388607.0f);
                    p[0] = static_cast<BYTE>(s & 0xFF);
                    p[1] = static_cast<BYTE>((s >> 8) & 0xFF);
                    p[2] = static_cast<BYTE>((s >> 16) & 0xFF);
                };
                write24(frame + 0 * bytesPerSample, m_workL[i]);
                write24(frame + 1 * bytesPerSample, m_workR[i]);
                for (UINT32 ch = 2; ch < m_devChannels; ++ch)
                    memset(frame + ch * bytesPerSample, 0, 3);
            }
            break;
        }
        case DevFmt::Int32: {
            int32_t* dst = reinterpret_cast<int32_t*>(pData);
            for (UINT32 i = 0; i < frames; ++i) {
                dst[i * m_devChannels + 0] = static_cast<int32_t>(clamp1(m_workL[i]) * 2147483647.0f);
                dst[i * m_devChannels + 1] = static_cast<int32_t>(clamp1(m_workR[i]) * 2147483647.0f);
                for (UINT32 ch = 2; ch < m_devChannels; ++ch)
                    dst[i * m_devChannels + ch] = 0;
            }
            break;
        }
        default:
            memset(pData, 0, frames * m_devBlockAlign);
            break;
        }
    }

    // -------------------------------------------------------
    //  レンダリングループ
    // -------------------------------------------------------
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

            writeToBuffer(pData, available);
            m_renderClient->ReleaseBuffer(available, 0);
        }
    }

    // -------------------------------------------------------
    //  メンバ変数
    // -------------------------------------------------------
    FmEngine&                  m_engine;
    bool                       m_exclusive;
    uint32_t                   m_sampleRate    = 44100;
    UINT32                     m_bufferFrames  = 0;

    // デバイスフォーマット情報
    DevFmt                     m_devFmt        = DevFmt::Float32;
    UINT32                     m_devChannels   = 2;
    UINT32                     m_devSampleRate = 44100;
    UINT32                     m_devBitsPerSample = 32;
    UINT32                     m_devBlockAlign = 8;

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
