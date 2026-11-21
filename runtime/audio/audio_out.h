// Host audio output (WASAPI shared mode). The cellAudio mixer clock (runtime/hle/cellAudio.cpp)
// pushes one 256-frame block every 5.33 ms; this keeps a small cushion buffered in the device
// and absorbs drift between the emulated 48 kHz clock and the device clock.
#pragma once

#include <cstdint>

// Call from the thread that will write. Returns false (and stays silent) if no device is usable.
bool audio_out_open(uint32_t sample_rate, uint32_t channels);
// Interleaved float frames in [-1, 1].
void audio_out_write(const float* frames, uint32_t count);
void audio_out_close();

struct AudioOutStats {
    uint32_t buffered_ms = 0;
    uint64_t underruns = 0;  // the device ran dry before a block arrived
    uint64_t dropped = 0;    // blocks skipped because the device clock is slower than ours
    uint64_t padded = 0;     // silence inserted because the device clock is faster
    bool active = false;
};
AudioOutStats audio_out_stats();
