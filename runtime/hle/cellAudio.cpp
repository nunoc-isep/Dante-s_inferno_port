// cellAudio: audio ports, read indices and the 48 kHz mixing clock.
// Every 256 samples (5.33 ms) each started port's read index advances one block
// and every registered notify queue receives an event - the timing contract the
// game's DAC thread is written against. Output is silent for now (the mixed
// port data is consumed but not sent to a host device yet).
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "../kernel/kernel.h"
#include "../kernel/sync.h"

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
};

std::mutex g_mutex;
bool g_init = false;
Port g_ports[kMaxPorts];
uint32_t g_read_index_addr = 0;  // u64 read index per port, in guest memory
uint32_t g_port_mem = 0;
std::vector<uint64_t> g_keys;
uint64_t g_ticks = 0;

void audio_thread() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1000000000ull * kBlockSamples / 48000);
    auto next = clock::now();
    for (;;) {
        next += period;
        std::this_thread::sleep_until(next);
        std::vector<uint64_t> keys;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (!g_init) return;
            for (uint32_t i = 0; i < kMaxPorts; ++i) {
                Port& p = g_ports[i];
                if (!p.open || p.state != STATUS_RUN || !p.blocks) continue;
                uint32_t ri = g_read_index_addr + 8 * i;
                uint64_t idx = vm::read64(ri);
                // consume the block (silence it so stale data is never replayed)
                uint32_t block_bytes = (uint32_t)(p.channels * kBlockSamples * 4);
                memset(vm::ptr(p.addr + (uint32_t)(idx % p.blocks) * block_bytes), 0, block_bytes);
                vm::write64(ri, (idx + 1) % p.blocks);
            }
            ++g_ticks;
            keys = g_keys;
        }
        for (uint64_t k : keys)
            if (auto q = event_queue_by_key(k)) q->push(0, 0, 0, 0);
    }
}

}  // namespace

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
    LOG_I("audio", "cellAudioInit: mixer clock running (48 kHz, %u-sample blocks), output silent", kBlockSamples);
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
