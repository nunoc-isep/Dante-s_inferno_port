// WASAPI shared-mode output for the cellAudio mix. See audio_out.h.
#include "audio_out.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "../kernel/kernel.h"

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid needing ksuser/ksmedia GUID definitions
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const CLSID kClsidEnumerator = __uuidof(MMDeviceEnumerator);
const IID kIidEnumerator = __uuidof(IMMDeviceEnumerator);
const IID kIidAudioClient = __uuidof(IAudioClient);
const IID kIidRenderClient = __uuidof(IAudioRenderClient);

constexpr uint32_t kTargetMs = 40;  // cushion kept in the device buffer
constexpr uint32_t kHighMs = 90;    // above this the device is slower than us: drop a block
constexpr uint32_t kLowMs = 12;     // below this it is faster: pad with silence

struct State {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    UINT32 buffer_frames = 0;
    uint32_t rate = 0, channels = 0;
    bool started = false;
    uint32_t retry_countdown = 0;
};
State g;
std::mutex g_stats_mutex;
AudioOutStats g_stats;

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void teardown() {
    if (g.client && g.started) g.client->Stop();
    g.started = false;
    release(g.render);
    release(g.client);
    release(g.device);
    release(g.enumerator);
    std::lock_guard<std::mutex> lk(g_stats_mutex);
    g_stats.active = false;
}

bool setup() {
    HRESULT hr = CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator, (void**)&g.enumerator);
    if (FAILED(hr)) return false;
    hr = g.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g.device);
    if (FAILED(hr)) return false;
    hr = g.device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, (void**)&g.client);
    if (FAILED(hr)) return false;

    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = (WORD)g.channels;
    fmt.Format.nSamplesPerSec = g.rate;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = (WORD)(4 * g.channels);
    fmt.Format.nAvgBytesPerSec = g.rate * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = g.channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    fmt.SubFormat = kSubtypeFloat;
    // let Windows convert rate/format to whatever the device mixes at
    const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = g.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1500000 /* 150 ms */, 0, &fmt.Format, nullptr);
    if (FAILED(hr)) {
        LOG_W("audio", "WASAPI Initialize failed (0x%08lX)", (unsigned long)hr);
        return false;
    }
    if (FAILED(g.client->GetBufferSize(&g.buffer_frames))) return false;
    if (FAILED(g.client->GetService(kIidRenderClient, (void**)&g.render))) return false;
    // prefill the cushion with silence, then start
    UINT32 pre = std::min<UINT32>(g.rate * kTargetMs / 1000, g.buffer_frames);
    BYTE* data = nullptr;
    if (SUCCEEDED(g.render->GetBuffer(pre, &data))) g.render->ReleaseBuffer(pre, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(g.client->Start())) return false;
    g.started = true;
    std::lock_guard<std::mutex> lk(g_stats_mutex);
    g_stats.active = true;
    return true;
}

}  // namespace

bool audio_out_open(uint32_t sample_rate, uint32_t channels) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    g.rate = sample_rate;
    g.channels = channels;
    if (!setup()) {
        teardown();
        LOG_W("audio", "no usable audio output device - running silent");
        return false;
    }
    LOG_I("audio", "WASAPI output: %u Hz, %u channels, device buffer %u frames", sample_rate, channels, g.buffer_frames);
    return true;
}

void audio_out_write(const float* frames, uint32_t count) {
    if (!g.started) {
        // device lost or absent: try again every ~2 s (e.g. headphones plugged back in)
        if (g.rate && ++g.retry_countdown >= 375) {
            g.retry_countdown = 0;
            teardown();
            if (setup()) LOG_I("audio", "audio output reopened");
            else teardown();
        }
        return;
    }
    UINT32 padding = 0;
    HRESULT hr = g.client->GetCurrentPadding(&padding);
    if (FAILED(hr)) {  // AUDCLNT_E_DEVICE_INVALIDATED and friends
        LOG_W("audio", "audio device lost (0x%08lX)", (unsigned long)hr);
        teardown();
        return;
    }
    const uint32_t buffered_ms = padding * 1000 / g.rate;
    AudioOutStats delta;
    if (padding == 0) ++delta.underruns;
    if (buffered_ms > kHighMs) {
        ++delta.dropped;  // our clock runs ahead of the device: skip this block
    } else {
        uint32_t pad = buffered_ms < kLowMs ? count : 0;  // device ahead of us: add a block of silence
        if (pad) ++delta.padded;
        UINT32 room = g.buffer_frames - padding;
        UINT32 n = std::min<UINT32>(count + pad, room);
        BYTE* data = nullptr;
        if (n && SUCCEEDED(g.render->GetBuffer(n, &data))) {
            float* out = (float*)data;
            uint32_t sil = std::min<uint32_t>(pad, n);
            memset(out, 0, sizeof(float) * g.channels * sil);
            memcpy(out + g.channels * sil, frames, sizeof(float) * g.channels * (n - sil));
            g.render->ReleaseBuffer(n, 0);
        }
    }
    std::lock_guard<std::mutex> lk(g_stats_mutex);
    g_stats.buffered_ms = buffered_ms;
    g_stats.underruns += delta.underruns;
    g_stats.dropped += delta.dropped;
    g_stats.padded += delta.padded;
}

void audio_out_close() { teardown(); }

AudioOutStats audio_out_stats() {
    std::lock_guard<std::mutex> lk(g_stats_mutex);
    return g_stats;
}
