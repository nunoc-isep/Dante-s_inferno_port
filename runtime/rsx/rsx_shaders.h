// RSX shader translators (vertex/fragment microcode -> HLSL).
#pragma once

#include <cstdint>
#include <string>

// Vertex program starting at instruction `start` of the 512-slot program memory.
std::string vp_to_hlsl(const uint32_t* ucode, uint32_t start, uint32_t& inputs_used, uint32_t& insn_count);

struct FpConfig {
    uint8_t tex_dims[16];       // 0 = unused, 1/2 = 2D, 3 = 3D, 6 = cube
    uint32_t shader_control;    // NV4097_SET_SHADER_CONTROL (0x40 = 32-bit registers, 0xE = depth replace)
    bool alpha_test;
    uint32_t alpha_func;        // GL-style 0x200..0x207
    float alpha_ref;            // 0..1
};

// Fragment program at guest address `addr`. Returns HLSL, the program size in bytes
// (including inline constants) and the mask of texture units it samples.
std::string fp_to_hlsl(uint32_t addr, const FpConfig& cfg, uint32_t& size_bytes, uint32_t& tex_used);
