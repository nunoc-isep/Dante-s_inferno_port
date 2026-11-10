// cellRtc: real-time clock. CellRtcDateTime = u16 year, month, day, hour, minute, second; u32 microsecond.
// CellRtcTick = u64 microseconds since 0001-01-01 00:00:00.
#include <chrono>
#include <ctime>

#include "../kernel/kernel.h"

namespace {
constexpr uint64_t kEpochTick = 62135596800ull * 1000000ull;  // 1970-01-01 in RTC ticks

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {  // H. Hinnant's algorithm
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

uint64_t datetime_to_tick(uint32_t p) {
    int64_t days = days_from_civil(vm::read16(p), vm::read16(p + 2), vm::read16(p + 4));
    int64_t secs = days * 86400 + vm::read16(p + 6) * 3600 + vm::read16(p + 8) * 60 + vm::read16(p + 10);
    return (uint64_t)(secs * 1000000 + vm::read32(p + 12)) + kEpochTick;
}
void tick_to_datetime(uint64_t tick, uint32_t p) {
    int64_t us = (int64_t)(tick - kEpochTick);
    int64_t secs = us >= 0 ? us / 1000000 : (us - 999999) / 1000000;
    int64_t days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
    int64_t rem = secs - days * 86400;
    int64_t y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    vm::write16(p + 0, (uint16_t)y);
    vm::write16(p + 2, (uint16_t)m);
    vm::write16(p + 4, (uint16_t)d);
    vm::write16(p + 6, (uint16_t)(rem / 3600));
    vm::write16(p + 8, (uint16_t)(rem / 60 % 60));
    vm::write16(p + 10, (uint16_t)(rem % 60));
    vm::write32(p + 12, (uint32_t)(us - secs * 1000000));
}
uint64_t now_utc_tick() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() + kEpochTick;
}
int64_t local_offset_us() {
    time_t t = time(nullptr);
    tm loc = *localtime(&t), utc = *gmtime(&t);
    return (int64_t)difftime(mktime(&loc), mktime(&utc)) * 1000000;
}
}  // namespace

HLE_FUNC(cellRtc, cellRtcGetCurrentClockLocalTime) {
    tick_to_datetime(now_utc_tick() + local_offset_us(), ARG32(0));
    RET(CELL_OK);
}
// (datetime*, timezone minutes)
HLE_FUNC(cellRtc, cellRtcGetCurrentClock) {
    tick_to_datetime(now_utc_tick() + (int64_t)(int32_t)ARG32(1) * 60000000, ARG32(0));
    RET(CELL_OK);
}
HLE_FUNC(cellRtc, cellRtcGetTick) {
    vm::write64(ARG32(1), datetime_to_tick(ARG32(0)));
    RET(CELL_OK);
}
HLE_FUNC(cellRtc, cellRtcSetTick) {
    tick_to_datetime(vm::read64(ARG32(1)), ARG32(0));
    RET(CELL_OK);
}
// (datetime*, time_t)
HLE_FUNC(cellRtc, cellRtcSetTime_t) {
    tick_to_datetime(ARG(1) * 1000000ull + kEpochTick, ARG32(0));
    RET(CELL_OK);
}
HLE_FUNC(cellRtc, cellRtcGetTime_t) {
    vm::write64(ARG32(1), (datetime_to_tick(ARG32(0)) - kEpochTick) / 1000000);
    RET(CELL_OK);
}
// (year, month, day) -> 0 = Sunday
HLE_FUNC(cellRtc, cellRtcGetDayOfWeek) {
    int64_t days = days_from_civil((int32_t)ARG32(0), ARG32(1), ARG32(2));
    ctx.r[3] = (uint64_t)(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
}
HLE_FUNC(cellRtc, cellRtcConvertUtcToLocalTime) {
    vm::write64(ARG32(1), vm::read64(ARG32(0)) + local_offset_us());
    RET(CELL_OK);
}
