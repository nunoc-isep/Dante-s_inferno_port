// Runtime side of the SPU static recompiler (tools/spu_recomp.py -> build/gen/spu_recomp.cpp).
//
// Each recompiled image is a set of code ranges with a byte snapshot. A range is only used
// after its snapshot matched this thread's LS; DMA writes overlapping a range reset it to
// "unknown" so it is re-validated on next use. Generated code checks range validity at every
// dispatch, so overwritten or different code always falls back to the interpreter.
#include <cstring>
#include <mutex>

#include "spu.h"
#include "spu_native.h"

namespace {
enum : uint8_t { kUnknown = 0, kValid = 1, kInvalid = 2 };
bool g_native_disabled = false;

uint8_t* states_for(SpuThread& t, uint32_t img) {
    if (!t.native_states) {
        t.native_states = new uint8_t*[g_spu_image_count]();
    }
    uint8_t*& s = t.native_states[img];
    if (!s) s = new uint8_t[g_spu_images[img].range_count]();
    return s;
}
}  // namespace

void spu_native_set_enabled(bool on) { g_native_disabled = !on; }

bool spu_validate_range(SpuThread& t, uint32_t img, uint32_t range) {
    const SpuImageDesc& d = g_spu_images[img];
    uint8_t* st = states_for(t, img);
    if (st[range] == kUnknown) {
        uint32_t lo = d.ranges[2 * range], hi = d.ranges[2 * range + 1];
        ++t.native_validations;
        st[range] = memcmp(t.ls + lo, d.bytes + (lo - d.base), hi - lo) == 0 ? kValid : kInvalid;
    }
    return st[range] == kValid;
}

// union of every image's entry bitmap: rejects branches into untranslated code with one lookup
static const uint64_t* any_entry_bitmap() {
    static const uint64_t* bm = [] {
        auto* m = new uint64_t[1024]();
        for (uint32_t i = 0; i < g_spu_image_count; ++i)
            for (uint32_t k = 0; k < 1024; ++k) m[k] |= g_spu_images[i].entry_bitmap[k];
        return m;
    }();
    return bm;
}

bool spu_native_available(SpuThread& t, uint32_t pc) {
    if (g_native_disabled) return false;
    uint32_t word = (pc >> 2) & 0xFFFF;
    if (!((any_entry_bitmap()[word >> 6] >> (word & 63)) & 1)) return false;
    for (uint32_t i = 0; i < g_spu_image_count; ++i) {
        const SpuImageDesc& d = g_spu_images[i];
        if (pc < d.base || pc >= d.end) continue;
        if (!((d.entry_bitmap[word >> 6] >> (word & 63)) & 1)) continue;
        int16_t r = d.range_of_word[word - (d.base >> 2)];
        if (r < 0 || !spu_validate_range(t, i, (uint32_t)r)) continue;
        t.native_image = i;
        return true;
    }
    return false;
}

uint64_t spu_run_native(SpuThread& t, uint32_t pc) {
    const SpuImageDesc& d = g_spu_images[t.native_image];
    uint64_t r = d.fn(t, pc, states_for(t, t.native_image));
    ++t.native_calls;
    return r;
}

// 4 KB LS pages holding each image's translated code: a DMA into data pages skips the image
static const uint64_t* code_page_masks() {
    static const uint64_t* masks = [] {
        auto* m = new uint64_t[g_spu_image_count]();
        for (uint32_t i = 0; i < g_spu_image_count; ++i) {
            const SpuImageDesc& d = g_spu_images[i];
            for (uint32_t r = 0; r < d.range_count; ++r)
                for (uint32_t p = d.ranges[2 * r] >> 12; p <= ((d.ranges[2 * r + 1] - 1) >> 12) && p < 64; ++p) m[i] |= 1ull << p;
        }
        return m;
    }();
    return masks;
}

void spu_native_ls_written(SpuThread& t, uint32_t lsa, uint32_t size) {
    if (!t.native_states || !size) return;
    uint32_t end = lsa + size;
    const uint64_t* masks = code_page_masks();
    uint32_t p0 = (lsa >> 12) & 63, p1 = ((end - 1) >> 12) & 63;
    uint64_t wmask = (p1 >= p0) ? ((p1 - p0 == 63 ? ~0ull : ((1ull << (p1 - p0 + 1)) - 1)) << p0) : ~0ull;
    for (uint32_t i = 0; i < g_spu_image_count; ++i) {
        uint8_t* st = t.native_states[i];
        if (!st || !(masks[i] & wmask)) continue;
        const SpuImageDesc& d = g_spu_images[i];
        if (end <= d.base || lsa >= d.end) continue;
        // ranges are sorted and disjoint: binary search the first one ending after lsa
        uint32_t lo = 0, hi = d.range_count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (d.ranges[2 * mid + 1] <= lsa) lo = mid + 1; else hi = mid;
        }
        for (uint32_t r = lo; r < d.range_count && d.ranges[2 * r] < end; ++r) st[r] = kUnknown;
    }
}
