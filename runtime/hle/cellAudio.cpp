// cellAudio: audio ports, read indices and the 48 kHz mixing clock.
// Every 256 samples (5.33 ms) each started port's read index advances one block
// and every registered notify queue receives an event - the timing contract the
// game's DAC thread is written against. The blocks are mixed (port level, 7.1 -> stereo
// downmix) and sent to the host through WASAPI (runtime/audio/audio_out.cpp).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../audio/audio_out.h"
#include "../kernel/kernel.h"
#include "../kernel/sync.h"

void host_sleep_us(uint64_t us);  // ppc_runtime.cpp: accurate short sleeps
bool g_audio_mute = false;         // --mute

namespace {

constexpr uint32_t kMaxPorts = 8, kBlockSamples = 256;
enum : uint32_t {
    CELL_AUDIO_ERROR_ALREADY_INIT = 0x80310701, CELL_AUDIO_ERROR_NOT_INIT = 0x80310703,
    CELL_AUDIO_ERROR_PARAM = 0x80310704, CELL_AUDIO_ERROR_PORT_FULL = 0x80310705,
    CELL_AUDIO_ERROR_PORT_NOT_OPEN = 0x80310708,
};
enum : uint32_t { STATUS_CLOSE = 0x1010, STATUS_READY = 1, STATUS_RUN = 2 };

struct Port {
    bool open = false;
    uint32_t state = STATUS_CLOSE;
    uint64_t channels = 0, blocks = 0, attr = 0;
    uint32_t addr = 0, size = 0;
    float level = 1.0f;
};
constexpr uint64_t PORTATTR_INITLEVEL = 0x1000;

std::mutex g_mutex;
bool g_init = false;
Port g_ports[kMaxPorts];
uint32_t g_read_index_addr = 0;  // u64 read index per port, in guest memory
uint32_t g_port_mem = 0;
std::vector<uint64_t> g_keys;
uint64_t g_ticks = 0;

// One block of a port, big-endian float32 interleaved, mixed into stereo with the PS3 7.1
// layout (L R C LFE Ls Rs Lb Rb) folded down; LFE is dropped.
void mix_block(const uint8_t* src, uint64_t channels, float level, float* stereo) {
    constexpr float k = 0.7071f;
    for (uint32_t i = 0; i < kBlockSamples; ++i) {
        float c[8] = {};
        for (uint64_t ch = 0; ch < channels; ++ch) {
            uint32_t u = ppc_ld32(src + (i * channels + ch) * 4);
            memcpy(&c[ch], &u, 4);
        }
        float l = c[0], r = c[1];
        if (channels == 8) {
            l += k * (c[2] + c[4] + c[6]);
            r += k * (c[2] + c[5] + c[7]);
        }
        stereo[2 * i] += l * level;
        stereo[2 * i + 1] += r * level;
    }
}

void audio_thread() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1000000000ull * kBlockSamples / 48000);
    const bool out = !g_audio_mute && audio_out_open(48000, 2);
    // debugging aid: DANTE_AUDIO_DUMP=<file> also writes the mix as raw 48 kHz stereo float32
    FILE* dump = getenv("DANTE_AUDIO_DUMP") ? fopen(getenv("DANTE_AUDIO_DUMP"), "wb") : nullptr;
    float stereo[2 * kBlockSamples];
    auto next = clock::now();
    for (;;) {
        // Precise 5.33 ms ticks: the game's audio engine advances its stream clock once per
        // wake-up (draining extra events), and movies sync video to that clock. A coarse sleep
        // (std::this_thread::sleep_until rounds up to the ~15.6 ms system tick here) delivered
        // events in bursts of three and ran audio - and movie playback - at a third of real time.
        next += period;
        auto now = clock::now();
        if (next > now)
            host_sleep_us((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(next - now).count());
        else if (now - next > 4 * period)
            next = now;  // fell far behind (debugger, host stall): resync instead of bursting
        std::vector<uint64_t> keys;
        memset(stereo, 0, sizeof(stereo));
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (!g_init) {
                if (out) audio_out_close();
                return;
            }
            for (uint32_t i = 0; i < kMaxPorts; ++i) {
                Port& p = g_ports[i];
                if (!p.open || p.state != STATUS_RUN || !p.blocks) continue;
                uint32_t ri = g_read_index_addr + 8 * i;
                uint64_t idx = vm::read64(ri);
                // play the block, then silence it so stale data is never replayed
                uint32_t block_bytes = (uint32_t)(p.channels * kBlockSamples * 4);
                uint8_t* blk = vm::ptr(p.addr + (uint32_t)(idx % p.blocks) * block_bytes);
                if (out || dump) mix_block(blk, p.channels, p.level, stereo);
                memset(blk, 0, block_bytes);
                vm::write64(ri, (idx + 1) % p.blocks);
            }
            ++g_ticks;
            keys = g_keys;
        }
        for (float& v : stereo) v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
        if (out) {
            audio_out_write(stereo, kBlockSamples);
        }
        if (dump) {
            fwrite(stereo, sizeof(stereo), 1, dump);
            fflush(dump);
        }
        for (uint64_t k : keys)
            if (auto q = event_queue_by_key(k)) q->push(0, 0, 0, 0);
    }
}

}  // namespace

// --perf: audio blocks produced and notify-queue backlog
void audio_perf_report(double seconds) {
    static uint64_t last = 0;
    uint64_t ticks;
    std::vector<uint64_t> keys;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ticks = g_ticks;
        keys = g_keys;
    }
    size_t backlog = 0;
    for (uint64_t k : keys)
        if (auto q = event_queue_by_key(k)) {
            std::lock_guard<std::mutex> lk(q->m);
            backlog += q->events.size();
        }
    AudioOutStats o = audio_out_stats();
    LOG_I("perf", "  audio: %.1f blocks/s (expected 187.5), notify queue backlog %zu; output %s, %u ms buffered, "
                  "underruns %llu, dropped %llu, padded %llu",
          (ticks - last) / seconds, backlog, o.active ? "on" : "off", o.buffered_ms, (unsigned long long)o.underruns,
          (unsigned long long)o.dropped, (unsigned long long)o.padded);
    last = ticks;
}

HLE_FUNC(cellAudio, cellAudioInit) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_init) return (void)RET(CELL_AUDIO_ERROR_ALREADY_INIT);
    g_init = true;
    if (!g_read_index_addr) {
        g_read_index_addr = vm::alloc(vm::Area::Kernel, 0x1000, 0x80);
        // port buffers: up to 8 channels x 32 blocks x 256 samples x 4 bytes = 256 KB per port
        g_port_mem = vm::alloc(vm::Area::Kernel, kMaxPorts * 0x40000, 0x10000);
    }
    memset(vm::ptr(g_read_index_addr), 0, 0x1000);
    for (auto& p : g_ports) p = Port{};
    std::thread(audio_thread).detach();
    LOG_I("audio", "cellAudioInit: mixer clock running (48 kHz, %u-sample blocks)", kBlockSamples);
    RET(CELL_OK);
}

HLE_FUNC(cellAudio, cellAudioQuit) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_init = false;
    RET(CELL_OK);
}

// CellAudioPortParam: u64 nChannel, u64 nBlock, u64 attr, f32 level
HLE_FUNC(cellAudio, cellAudioPortOpen) {
    uint32_t param = ARG32(0), out = ARG32(1);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_init) return (void)RET(CELL_AUDIO_ERROR_NOT_INIT);
    uint64_t ch = vm::read64(param), blocks = vm::read64(param + 8), attr = vm::read64(param + 16);
    if ((ch != 2 && ch != 8) || (blocks != 2 && blocks != 4 && blocks != 8 && blocks != 16 && blocks != 32))
        return (void)RET(CELL_AUDIO_ERROR_PARAM);
    for (uint32_t i = 0; i < kMaxPorts; ++i) {
        Port& p = g_ports[i];
        if (p.open) continue;
        p.open = true;
        p.state = STATUS_READY;
        p.channels = ch;
        p.blocks = blocks;
        p.attr = attr;
        p.size = (uint32_t)(ch * blocks * kBlockSamples * 4);
        if (attr & PORTATTR_INITLEVEL) {
            uint32_t u = vm::read32(param + 24);
            memcpy(&p.level, &u, 4);
        }
        p.addr = g_port_mem + i * 0x40000;
        memset(vm::ptr(p.addr), 0, p.size);
        vm::write64(g_read_index_addr + 8 * i, 0);
        vm::write32(out, i);
        LOG_I("audio", "port %u opened: %llu channels, %llu blocks, attr 0x%llX, buffer 0x%08X", i,
              (unsigned long long)ch, (unsigned long long)blocks, (unsigned long long)attr, p.addr);
        return (void)RET(CELL_OK);
    }
    RET(CELL_AUDIO_ERROR_PORT_FULL);
}

// CellAudioPortConfig: u32 readIndexAddr, u32 status, u64 nChannel, u64 nBlock, u32 portSize, u32 portAddr
HLE_FUNC(cellAudio, cellAudioGetPortConfig) {
    uint32_t n = ARG32(0), cfg = ARG32(1);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (n >= kMaxPorts) return (void)RET(CELL_AUDIO_ERROR_PARAM);
    Port& p = g_ports[n];
    vm::write32(cfg + 0, g_read_index_addr + 8 * n);
    vm::write32(cfg + 4, p.open ? p.state : STATUS_CLOSE);
    vm::write64(cfg + 8, p.channels);
    vm::write64(cfg + 16, p.blocks);
    vm::write32(cfg + 24, p.size);
    vm::write32(cfg + 28, p.addr);
    RET(CELL_OK);
}

HLE_FUNC(cellAudio, cellAudioPortStart) {
    std::lock_guard<std::mutex> lk(g_mutex);
    uint32_t n = ARG32(0);
    if (n >= kMaxPorts || !g_ports[n].open) return (void)RET(CELL_AUDIO_ERROR_PORT_NOT_OPEN);
    g_ports[n].state = STATUS_RUN;
    RET(CELL_OK);
}
HLE_FUNC(cellAudio, cellAudioPortStop) {
    std::lock_guard<std::mutex> lk(g_mutex);
    uint32_t n = ARG32(0);
    if (n >= kMaxPorts || !g_ports[n].open) return (void)RET(CELL_AUDIO_ERROR_PORT_NOT_OPEN);
    g_ports[n].state = STATUS_READY;
    RET(CELL_OK);
}
HLE_FUNC(cellAudio, cellAudioPortClose) {
    std::lock_guard<std::mutex> lk(g_mutex);
    uint32_t n = ARG32(0);
    if (n >= kMaxPorts || !g_ports[n].open) return (void)RET(CELL_AUDIO_ERROR_PORT_NOT_OPEN);
    g_ports[n] = Port{};
    RET(CELL_OK);
}

// cellAudioSetPortLevel(portNum, float level)
HLE_FUNC(cellAudio, cellAudioSetPortLevel) {
    std::lock_guard<std::mutex> lk(g_mutex);
    uint32_t n = ARG32(0);
    if (n >= kMaxPorts || !g_ports[n].open) return (void)RET(CELL_AUDIO_ERROR_PORT_NOT_OPEN);
    float level = (float)ctx.f[1].f64;
    if (level < 0.0f) return (void)RET(CELL_AUDIO_ERROR_PARAM);
    g_ports[n].level = level;
    RET(CELL_OK);
}

HLE_FUNC(cellAudio, cellAudioSetNotifyEventQueue) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_keys.push_back(ARG(0));
    LOG_I("audio", "notify queue key 0x%llX registered", (unsigned long long)ARG(0));
    RET(CELL_OK);
}
HLE_FUNC(cellAudio, cellAudioRemoveNotifyEventQueue) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (size_t i = 0; i < g_keys.size(); ++i)
        if (g_keys[i] == ARG(0)) {
            g_keys.erase(g_keys.begin() + i);
            break;
        }
    RET(CELL_OK);
}
