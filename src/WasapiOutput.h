#pragma once
// WasapiOutput.h
// WASAPI リアルタイム出力
//
// 設計方針:
//   AUTOCONVERTPCM に依存しない。
//   GetMixFormat でデバイスのネイティブフォーマットを取得して Initialize し、
//   renderLoop 内で FmEngine の float32 出力をデバイスフォーマットに自前変換する。
//   これにより複数デバイス環境や AUTOCONVERTPCM 非対応環境でも確実に動作する。
//
// デバイス選択:
//   WasapiOutput(engine, exclusive)            → デフォルトデバイス
//   WasapiOutput(engine, exclusive, device_id) → デバイスIDで明示指定
//   enumerateDevices()                         → 利用可能デバイス一覧を取得

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

static inline std::string toHex(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", v);
    return std::string(buf);
}

// =========================================================
//  デバイス情報構造体
// =========================================================
struct WasapiDeviceInfo {
    std::wstring id;    // デバイスID (Wasapi_CreateWithDevice / WasapiOutput コンストラクタに渡す)
    std::wstring name;  // 表示名
    bool isDefault;     // デフォルトデバイスか
};

// =========================================================
//  デバイス列挙ユーティリティ
//  COM の初期化を内部で行うため、CoInitialize 済みかどうかを問わず呼べる。
// =========================================================
inline std::vector<WasapiDeviceInfo> enumerateWasapiDevices() {
    std::vector<WasapiDeviceInfo> result;

    // COM をこの関数のスコープ内で初期化する。
    // 呼び出し元スレッドが既に初期化済みの場合 (S_FALSE) は Uninitialize が必要。
    // RPC_E_CHANGED_MODE (アパートメント競合) の場合は既存の COM 環境をそのまま使う。
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = (hrCo == S_OK || hrCo == S_FALSE);

    auto cleanup = [&]() { if (needUninit) CoUninitialize(); };

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        cleanup();
        return result;
    }

    // デフォルトデバイスのIDを取得
    ComPtr<IMMDevice> pDefault;
    std::wstring defaultId;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefault))) {
        LPWSTR pwszId = nullptr;
        if (SUCCEEDED(pDefault->GetId(&pwszId)) && pwszId) {
            defaultId = pwszId;
            CoTaskMemFree(pwszId);
        }
    }

    // 全再生デバイスを列挙
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        cleanup();
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> pDevice;
        if (FAILED(collection->Item(i, &pDevice))) continue;

        WasapiDeviceInfo info;

        // デバイスID
        LPWSTR pwszId = nullptr;
        if (SUCCEEDED(pDevice->GetId(&pwszId)) && pwszId) {
            info.id = pwszId;
            CoTaskMemFree(pwszId);
        }

        // 表示名 (IPropertyStore → PKEY_Device_FriendlyName)
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var))
                && var.vt == VT_LPWSTR && var.pwszVal) {
                info.name = var.pwszVal;
            }
            PropVariantClear(&var);
        }

        info.isDefault = (!defaultId.empty() && info.id == defaultId);
        result.push_back(std::move(info));
    }

    cleanup();
    return result;
}

// =========================================================
//  WasapiOutput
// =========================================================
class WasapiOutput {
public:
    struct HandleDeleter {
        void operator()(HANDLE h) const {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    };
    using UniqueHandle = std::unique_ptr<std::remove_pointer<HANDLE>::type, HandleDeleter>;

    // デフォルトデバイスで初期化
    explicit WasapiOutput(FmEngine& engine, bool exclusive = false)
        : m_engine(engine), m_exclusive(exclusive)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr != RPC_E_CHANGED_MODE) CHECK_HR(hr, "CoInitialize");
        openDefaultDevice();
        initAudioClient();
    }

    // デバイスIDを明示指定して初期化
    // device_id: enumerateWasapiDevices() で取得した WasapiDeviceInfo::id
    WasapiOutput(FmEngine& engine, bool exclusive, const std::wstring& device_id)
        : m_engine(engine), m_exclusive(exclusive)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr != RPC_E_CHANGED_MODE) CHECK_HR(hr, "CoInitialize");
        openDeviceById(device_id);
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
    enum class DevFmt { Float32, Int16, Int24, Int32, Unknown };

    // -------------------------------------------------------
    //  デバイスオープン
    // -------------------------------------------------------
    void openDefaultDevice() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        CHECK_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator)),
                 "MMDeviceEnumerator");
        CHECK_HR(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device),
                 "GetDefaultAudioEndpoint");
    }

    void openDeviceById(const std::wstring& device_id) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        CHECK_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator)),
                 "MMDeviceEnumerator");
        CHECK_HR(enumerator->GetDevice(device_id.c_str(), &m_device),
                 "IMMDeviceEnumerator::GetDevice");
    }

    // -------------------------------------------------------
    //  IAudioClient 初期化
    // -------------------------------------------------------
    void initAudioClient() {
        CHECK_HR(m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(m_audioClient.GetAddressOf())),
                 "Activate IAudioClient");

        WAVEFORMATEX* pMixFormat = nullptr;
        CHECK_HR(m_audioClient->GetMixFormat(&pMixFormat), "GetMixFormat");

        m_devChannels      = pMixFormat->nChannels;
        m_devSampleRate    = pMixFormat->nSamplesPerSec;
        m_devBitsPerSample = pMixFormat->wBitsPerSample;
        m_devBlockAlign    = pMixFormat->nBlockAlign;
        m_devFmt           = detectFormat(pMixFormat);

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        if (m_exclusive) {
            HRESULT hrSup = m_audioClient->IsFormatSupported(
                AUDCLNT_SHAREMODE_EXCLUSIVE, pMixFormat, nullptr);
            if (FAILED(hrSup))
                m_exclusive = false; // 非対応なら Shared に降格
        }

        HRESULT hr;
        if (m_exclusive) {
            REFERENCE_TIME minPeriod = 0;
            m_audioClient->GetDevicePeriod(nullptr, &minPeriod);
            hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                                           minPeriod, minPeriod, pMixFormat, nullptr);
        } else {
            hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
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
        // エンジンのサンプルレートはそのまま維持し、
        // renderLoop 内でデバイスフレーム数に対応したエンジンフレーム数を計算する。
        // デバイスレートとエンジンレートが一致しない場合の変換比率を保持する。
        m_engRate   = m_engine.sampleRate();
        m_sampleRate = m_devSampleRate;
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
    //  float32 → デバイスフォーマット変換書き込み
    // -------------------------------------------------------
    static float clamp1(float v) {
        return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    }

    void writeToBuffer(BYTE* pData, UINT32 frames, const float* srcL, const float* srcR) {
        switch (m_devFmt) {
        case DevFmt::Float32: {
            float* dst = reinterpret_cast<float*>(pData);
            for (UINT32 i = 0; i < frames; ++i) {
                dst[i * m_devChannels + 0] = clamp1(srcL[i]);
                dst[i * m_devChannels + 1] = clamp1(srcR[i]);
                for (UINT32 ch = 2; ch < m_devChannels; ++ch)
                    dst[i * m_devChannels + ch] = 0.0f;
            }
            break;
        }
        case DevFmt::Int16: {
            int16_t* dst = reinterpret_cast<int16_t*>(pData);
            for (UINT32 i = 0; i < frames; ++i) {
                dst[i * m_devChannels + 0] = static_cast<int16_t>(clamp1(srcL[i]) * 32767.0f);
                dst[i * m_devChannels + 1] = static_cast<int16_t>(clamp1(srcR[i]) * 32767.0f);
                for (UINT32 ch = 2; ch < m_devChannels; ++ch)
                    dst[i * m_devChannels + ch] = 0;
            }
            break;
        }
        case DevFmt::Int24: {
            const UINT32 bytesPerFrame  = m_devBlockAlign;
            const UINT32 bytesPerSample = bytesPerFrame / m_devChannels;
            for (UINT32 i = 0; i < frames; ++i) {
                BYTE* frame = pData + i * bytesPerFrame;
                auto write24 = [](BYTE* p, float v) {
                    int32_t s = static_cast<int32_t>(
                        (v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v) * 8388607.0f);
                    p[0] = static_cast<BYTE>( s        & 0xFF);
                    p[1] = static_cast<BYTE>((s >>  8) & 0xFF);
                    p[2] = static_cast<BYTE>((s >> 16) & 0xFF);
                };
                write24(frame + 0 * bytesPerSample, srcL[i]);
                write24(frame + 1 * bytesPerSample, srcR[i]);
                for (UINT32 ch = 2; ch < m_devChannels; ++ch)
                    memset(frame + ch * bytesPerSample, 0, 3);
            }
            break;
        }
        case DevFmt::Int32: {
            int32_t* dst = reinterpret_cast<int32_t*>(pData);
            for (UINT32 i = 0; i < frames; ++i) {
                dst[i * m_devChannels + 0] = static_cast<int32_t>(clamp1(srcL[i]) * 2147483647.0f);
                dst[i * m_devChannels + 1] = static_cast<int32_t>(clamp1(srcR[i]) * 2147483647.0f);
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
            if (padding >= m_bufferFrames) continue;  // アンダーフロー防止
            const UINT32 devFrames = m_bufferFrames - padding;
            if (devFrames == 0) continue;

            // デバイスフレーム数に対応するエンジンフレーム数を計算する。
            // デバイスレートとエンジンレートが一致する場合はそのまま使う。
            UINT32 engFrames = devFrames;
            if (m_engRate != m_devSampleRate) {
                engFrames = static_cast<UINT32>(
                    static_cast<uint64_t>(devFrames) * m_engRate / m_devSampleRate) + 1;
            }

            // エンジンバッファをエンジンフレーム数で確保・生成
            if (m_workL.size() < engFrames) m_workL.resize(engFrames);
            if (m_workR.size() < engFrames) m_workR.resize(engFrames);
            m_engine.generate(m_workL.data(), m_workR.data(), engFrames);

            // レート変換が必要な場合は線形補間でリサンプリング
            if (m_engRate != m_devSampleRate) {
                if (m_resampledL.size() < devFrames) m_resampledL.resize(devFrames);
                if (m_resampledR.size() < devFrames) m_resampledR.resize(devFrames);
                const double ratio = static_cast<double>(m_engRate) / m_devSampleRate;
                for (UINT32 i = 0; i < devFrames; ++i) {
                    const double pos   = i * ratio;
                    const UINT32 idx   = static_cast<UINT32>(pos);
                    const float  frac  = static_cast<float>(pos - idx);
                    const UINT32 idx1  = (idx + 1 < engFrames) ? idx + 1 : idx;
                    m_resampledL[i] = m_workL[idx] + (m_workL[idx1] - m_workL[idx]) * frac;
                    m_resampledR[i] = m_workR[idx] + (m_workR[idx1] - m_workR[idx]) * frac;
                }
            }

            const float* srcL = (m_engRate != m_devSampleRate) ? m_resampledL.data() : m_workL.data();
            const float* srcR = (m_engRate != m_devSampleRate) ? m_resampledR.data() : m_workR.data();

            BYTE* pData = nullptr;
            if (FAILED(m_renderClient->GetBuffer(devFrames, &pData))) break;

            writeToBuffer(pData, devFrames, srcL, srcR);
            m_renderClient->ReleaseBuffer(devFrames, 0);
        }
    }

    // -------------------------------------------------------
    //  メンバ変数
    // -------------------------------------------------------
    FmEngine&                  m_engine;
    bool                       m_exclusive;
    uint32_t                   m_sampleRate       = 44100;  // デバイスのサンプルレート
    uint32_t                   m_engRate          = 44100;  // エンジンのサンプルレート
    UINT32                     m_bufferFrames     = 0;

    DevFmt                     m_devFmt           = DevFmt::Float32;
    UINT32                     m_devChannels      = 2;
    UINT32                     m_devSampleRate    = 44100;
    UINT32                     m_devBitsPerSample = 32;
    UINT32                     m_devBlockAlign    = 8;

    ComPtr<IMMDevice>          m_device;
    ComPtr<IAudioClient>       m_audioClient;
    ComPtr<IAudioRenderClient> m_renderClient;

    UniqueHandle               m_readyEvent;
    UniqueHandle               m_stopEvent;

    std::thread                m_thread;
    std::atomic<bool>          m_running{false};

    std::vector<float>         m_workL;
    std::vector<float>         m_workR;
    std::vector<float>         m_resampledL;  // レート変換後バッファ (L)
    std::vector<float>         m_resampledR;  // レート変換後バッファ (R)
};

#undef CHECK_HR
