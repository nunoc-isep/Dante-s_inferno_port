// LV2 synchronisation objects, timers, time, memory and TTY syscalls.
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include "kernel.h"
#include "sync.h"

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- object table
static std::mutex g_obj_mutex;
static std::map<uint32_t, std::shared_ptr<void>> g_objects;
static uint32_t g_next_id = 0x40000001;

uint32_t kobj_add(std::shared_ptr<void> obj) {
    std::lock_guard<std::mutex> lk(g_obj_mutex);
    uint32_t id = g_next_id++;
    g_objects[id] = std::move(obj);
    return id;
}
std::shared_ptr<void> kobj_get_raw(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_obj_mutex);
    auto it = g_objects.find(id);
    return it == g_objects.end() ? nullptr : it->second;
}
bool kobj_remove(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_obj_mutex);
    return g_objects.erase(id) != 0;
}

static uint32_t self_id() { return thread_id(current_thread()); }

// timeout in microseconds, 0 = infinite
template <typename Pred>
static bool wait_pred(std::condition_variable& cv, std::unique_lock<std::mutex>& lk, uint64_t timeout_us, Pred pred) {
    if (!timeout_us) {
        cv.wait(lk, pred);
        return true;
    }
    return cv.wait_for(lk, std::chrono::microseconds(timeout_us), pred);
}

// ---------------------------------------------------------------- mutex
uint32_t KMutex::lock(uint32_t tid, uint64_t timeout_us) {
    std::unique_lock<std::mutex> lk(m);
    if (owner == tid) {
        if (!recursive) return CELL_EDEADLK;
        ++count;
        return CELL_OK;
    }
    if (!wait_pred(cv, lk, timeout_us, [&] { return owner == 0; })) return CELL_ETIMEDOUT;
    owner = tid;
    count = 1;
    return CELL_OK;
}
uint32_t KMutex::trylock(uint32_t tid) {
    std::unique_lock<std::mutex> lk(m);
    if (owner == tid) {
        if (!recursive) return CELL_EDEADLK;
        ++count;
        return CELL_OK;
    }
    if (owner) return CELL_EBUSY;
    owner = tid;
    count = 1;
    return CELL_OK;
}
uint32_t KMutex::unlock(uint32_t tid) {
    std::unique_lock<std::mutex> lk(m);
    if (owner != tid) return CELL_EPERM;
    if (--count == 0) {
        owner = 0;
        cv.notify_all();
    }
    return CELL_OK;
}

// sys_mutex_attribute_t: protocol, recursive, pshared, adaptive, u64 ipc_key, s32 flags, pad, char name[8]
SYSCALL(100, sys_mutex_create) {
    auto mtx = std::make_shared<KMutex>();
    uint32_t attr = ARG32(1);
    mtx->recursive = attr && vm::read32(attr + 4) == 0x10;
    uint32_t id = kobj_add(mtx);
    vm::write32(ARG32(0), id);
    RET(CELL_OK);
}
SYSCALL(101, sys_mutex_destroy) { RET(kobj_remove(ARG32(0)) ? CELL_OK : CELL_ESRCH); }
SYSCALL(102, sys_mutex_lock) {
    auto m = kobj_get<KMutex>(ARG32(0));
    RET(m ? m->lock(self_id(), ARG(1)) : CELL_ESRCH);
}
SYSCALL(103, sys_mutex_trylock) {
    auto m = kobj_get<KMutex>(ARG32(0));
    RET(m ? m->trylock(self_id()) : CELL_ESRCH);
}
SYSCALL(104, sys_mutex_unlock) {
    auto m = kobj_get<KMutex>(ARG32(0));
    RET(m ? m->unlock(self_id()) : CELL_ESRCH);
}

// ---------------------------------------------------------------- condition variable (bound to a mutex)
struct KCond {
    std::shared_ptr<KMutex> mtx;
    std::condition_variable cv;
    uint32_t waiters = 0, signals = 0;
};

SYSCALL(105, sys_cond_create) {
    auto mtx = kobj_get<KMutex>(ARG32(1));
    if (!mtx) return (void)RET(CELL_ESRCH);
    auto c = std::make_shared<KCond>();
    c->mtx = mtx;
    vm::write32(ARG32(0), kobj_add(c));
    RET(CELL_OK);
}
SYSCALL(106, sys_cond_destroy) { RET(kobj_remove(ARG32(0)) ? CELL_OK : CELL_ESRCH); }
SYSCALL(107, sys_cond_wait) {
    auto c = kobj_get<KCond>(ARG32(0));
    if (!c) return (void)RET(CELL_ESRCH);
    uint32_t tid = self_id();
    KMutex& m = *c->mtx;
    std::unique_lock<std::mutex> lk(m.m);
    if (m.owner != tid) return (void)RET(CELL_EPERM);
    uint32_t saved = m.count;
    m.owner = 0;
    m.count = 0;
    m.cv.notify_all();
    ++c->waiters;
    bool ok = wait_pred(c->cv, lk, ARG(1), [&] { return c->signals > 0; });
    if (ok) --c->signals;
    --c->waiters;
    m.cv.wait(lk, [&] { return m.owner == 0; });
    m.owner = tid;
    m.count = saved;
    RET(ok ? CELL_OK : CELL_ETIMEDOUT);
}
static void cond_signal(KCond& c, bool all) {
    std::unique_lock<std::mutex> lk(c.mtx->m);
    if (all) c.signals = c.waiters;
    else if (c.signals < c.waiters) ++c.signals;
    c.cv.notify_all();
}
SYSCALL(108, sys_cond_signal) {
    auto c = kobj_get<KCond>(ARG32(0));
    if (c) cond_signal(*c, false);
    RET(c ? CELL_OK : CELL_ESRCH);
}
SYSCALL(109, sys_cond_signal_all) {
    auto c = kobj_get<KCond>(ARG32(0));
    if (c) cond_signal(*c, true);
    RET(c ? CELL_OK : CELL_ESRCH);
}
SYSCALL(110, sys_cond_signal_to) {
    auto c = kobj_get<KCond>(ARG32(0));
    if (c) cond_signal(*c, false);
    RET(c ? CELL_OK : CELL_ESRCH);
}

// ---------------------------------------------------------------- semaphore
struct KSema {
    std::mutex m;
    std::condition_variable cv;
    int32_t value = 0, max = 0;
};
// sys_semaphore_create(id*, attr*, initial, max)
SYSCALL(90, sys_semaphore_create) {
    auto s = std::make_shared<KSema>();
    s->value = (int32_t)ARG32(2);
    s->max = (int32_t)ARG32(3);
    vm::write32(ARG32(0), kobj_add(s));
    RET(CELL_OK);
}
SYSCALL(91, sys_semaphore_destroy) { RET(kobj_remove(ARG32(0)) ? CELL_OK : CELL_ESRCH); }
SYSCALL(92, sys_semaphore_wait) {
    auto s = kobj_get<KSema>(ARG32(0));
    if (!s) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(s->m);
    if (!wait_pred(s->cv, lk, ARG(1), [&] { return s->value > 0; })) return (void)RET(CELL_ETIMEDOUT);
    --s->value;
    RET(CELL_OK);
}
SYSCALL(93, sys_semaphore_trywait) {
    auto s = kobj_get<KSema>(ARG32(0));
    if (!s) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(s->m);
    if (s->value <= 0) return (void)RET(CELL_EBUSY);
    --s->value;
    RET(CELL_OK);
}
SYSCALL(94, sys_semaphore_post) {
    auto s = kobj_get<KSema>(ARG32(0));
    if (!s) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(s->m);
    int32_t n = (int32_t)ARG32(1);
    if (s->value + n > s->max) return (void)RET(CELL_EBUSY);
    s->value += n;
    s->cv.notify_all();
    RET(CELL_OK);
}
SYSCALL(114, sys_semaphore_get_value) {
    auto s = kobj_get<KSema>(ARG32(0));
    if (!s) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(s->m);
    vm::write32(ARG32(1), (uint32_t)s->value);
    RET(CELL_OK);
}

// ---------------------------------------------------------------- rwlock
struct KRwlock {
    std::mutex m;
    std::condition_variable cv;
    uint32_t readers = 0, writer = 0;
};
SYSCALL(120, sys_rwlock_create) {
    vm::write32(ARG32(0), kobj_add(std::make_shared<KRwlock>()));
    RET(CELL_OK);
}
SYSCALL(121, sys_rwlock_destroy) { RET(kobj_remove(ARG32(0)) ? CELL_OK : CELL_ESRCH); }
SYSCALL(122, sys_rwlock_rlock) {
    auto l = kobj_get<KRwlock>(ARG32(0));
    if (!l) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(l->m);
    if (!wait_pred(l->cv, lk, ARG(1), [&] { return l->writer == 0; })) return (void)RET(CELL_ETIMEDOUT);
    ++l->readers;
    RET(CELL_OK);
}
SYSCALL(123, sys_rwlock_tryrlock) {
    auto l = kobj_get<KRwlock>(ARG32(0));
    if (!l) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(l->m);
    if (l->writer) return (void)RET(CELL_EBUSY);
    ++l->readers;
    RET(CELL_OK);
}
SYSCALL(124, sys_rwlock_runlock) {
    auto l = kobj_get<KRwlock>(ARG32(0));
    if (!l) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(l->m);
    if (!l->readers) return (void)RET(CELL_EPERM);
    if (--l->readers == 0) l->cv.notify_all();
    RET(CELL_OK);
}
SYSCALL(125, sys_rwlock_wlock) {
    auto l = kobj_get<KRwlock>(ARG32(0));
    if (!l) return (void)RET(CELL_ESRCH);
    uint32_t tid = self_id();
    std::unique_lock<std::mutex> lk(l->m);
    if (l->writer == tid) return (void)RET(CELL_EDEADLK);
    if (!wait_pred(l->cv, lk, ARG(1), [&] { return l->writer == 0 && l->readers == 0; }))
        return (void)RET(CELL_ETIMEDOUT);
    l->writer = tid;
    RET(CELL_OK);
}
SYSCALL(126, sys_rwlock_trywlock) {
    auto l = kobj_get<KRwlock>(ARG32(0));
    if (!l) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(l->m);
    if (l->writer || l->readers) return (void)RET(CELL_EBUSY);
    l->writer = self_id();
    RET(CELL_OK);
}
SYSCALL(127, sys_rwlock_wunlock) {
    auto l = kobj_get<KRwlock>(ARG32(0));
    if (!l) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(l->m);
    if (l->writer != self_id()) return (void)RET(CELL_EPERM);
    l->writer = 0;
    l->cv.notify_all();
    RET(CELL_OK);
}

// ---------------------------------------------------------------- event queues / ports
std::shared_ptr<KEventQueue> event_queue_get(uint32_t id) { return kobj_get<KEventQueue>(id); }

static std::mutex g_eq_key_mutex;
static std::map<uint64_t, std::weak_ptr<KEventQueue>> g_eq_by_key;

std::shared_ptr<KEventQueue> event_queue_by_key(uint64_t key) {
    std::lock_guard<std::mutex> lk(g_eq_key_mutex);
    auto it = g_eq_by_key.find(key);
    return it == g_eq_by_key.end() ? nullptr : it->second.lock();
}

void KEventQueue::push(uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3) {
    std::unique_lock<std::mutex> lk(m);
    events.push_back({source, d1, d2, d3});
    cv.notify_one();
}

// sys_event_queue_create(eq_id*, attr*, key u64, size)
SYSCALL(128, sys_event_queue_create) {
    auto q = std::make_shared<KEventQueue>();
    q->key = ARG(2);
    q->size = ARG32(3);
    uint32_t id = kobj_add(q);
    vm::write32(ARG32(0), id);
    if (q->key) {
        std::lock_guard<std::mutex> lk(g_eq_key_mutex);
        g_eq_by_key[q->key] = q;
    }
    LOG_I("lv2", "event queue 0x%X created (key 0x%llX, size %u)", id, (unsigned long long)q->key, q->size);
    RET(CELL_OK);
}
SYSCALL(129, sys_event_queue_destroy) {
    auto q = kobj_get<KEventQueue>(ARG32(0));
    if (q) {
        std::unique_lock<std::mutex> lk(q->m);
        q->destroyed = true;
        q->cv.notify_all();
    }
    RET(kobj_remove(ARG32(0)) ? CELL_OK : CELL_ESRCH);
}
// Returns the event in r4..r7 (the pointer argument is unused by lv2).
SYSCALL(130, sys_event_queue_receive) {
    auto q = kobj_get<KEventQueue>(ARG32(0));
    if (!q) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(q->m);
    if (!wait_pred(q->cv, lk, ARG(2), [&] { return !q->events.empty() || q->destroyed; }))
        return (void)RET(CELL_ETIMEDOUT);
    if (q->destroyed) return (void)RET(CELL_ECANCELED);
    KEvent e = q->events.front();
    q->events.pop_front();
    ctx.r[4] = e.source;
    ctx.r[5] = e.d1;
    ctx.r[6] = e.d2;
    ctx.r[7] = e.d3;
    RET(CELL_OK);
}
// sys_event_queue_tryreceive(eq_id, event_array*, size, number*)
SYSCALL(131, sys_event_queue_tryreceive) {
    auto q = kobj_get<KEventQueue>(ARG32(0));
    if (!q) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(q->m);
    uint32_t arr = ARG32(1), n = 0, max = ARG32(2);
    while (n < max && !q->events.empty()) {
        KEvent e = q->events.front();
        q->events.pop_front();
        vm::write64(arr + n * 32 + 0, e.source);
        vm::write64(arr + n * 32 + 8, e.d1);
        vm::write64(arr + n * 32 + 16, e.d2);
        vm::write64(arr + n * 32 + 24, e.d3);
        ++n;
    }
    vm::write32(ARG32(3), n);
    RET(CELL_OK);
}
SYSCALL(133, sys_event_queue_drain) {
    auto q = kobj_get<KEventQueue>(ARG32(0));
    if (!q) return (void)RET(CELL_ESRCH);
    std::unique_lock<std::mutex> lk(q->m);
    q->events.clear();
    RET(CELL_OK);
}

struct KEventPort {
    uint64_t name = 0;
    uint32_t id = 0;
    std::weak_ptr<KEventQueue> queue;
};
// sys_event_port_create(port_id*, type, name u64)
SYSCALL(134, sys_event_port_create) {
    auto p = std::make_shared<KEventPort>();
    p->name = ARG(2);
    p->id = kobj_add(p);
    vm::write32(ARG32(0), p->id);
    RET(CELL_OK);
}
SYSCALL(135, sys_event_port_destroy) { RET(kobj_remove(ARG32(0)) ? CELL_OK : CELL_ESRCH); }
SYSCALL(136, sys_event_port_connect_local) {
    auto p = kobj_get<KEventPort>(ARG32(0));
    auto q = kobj_get<KEventQueue>(ARG32(1));
    if (!p || !q) return (void)RET(CELL_ESRCH);
    p->queue = q;
    RET(CELL_OK);
}
SYSCALL(137, sys_event_port_disconnect) {
    auto p = kobj_get<KEventPort>(ARG32(0));
    if (!p) return (void)RET(CELL_ESRCH);
    p->queue.reset();
    RET(CELL_OK);
}
SYSCALL(138, sys_event_port_send) {
    auto p = kobj_get<KEventPort>(ARG32(0));
    if (!p) return (void)RET(CELL_ESRCH);
    auto q = p->queue.lock();
    if (!q) return (void)RET(0x80010013 /* ENOTCONN */);
    uint64_t source = p->name ? p->name : ((1ull << 32) | p->id);
    q->push(source, ARG(1), ARG(2), ARG(3));
    RET(CELL_OK);
}

// ---------------------------------------------------------------- timers / time
SYSCALL(141, sys_timer_usleep) {
    std::this_thread::sleep_for(std::chrono::microseconds(ARG(0)));
    RET(CELL_OK);
}
SYSCALL(142, sys_timer_sleep) {
    std::this_thread::sleep_for(std::chrono::seconds(ARG32(0)));
    RET(CELL_OK);
}
SYSCALL(144, sys_time_get_timezone) {
    TIME_ZONE_INFORMATION tz;
    DWORD r = GetTimeZoneInformation(&tz);
    if (ARG32(0)) vm::write32(ARG32(0), (uint32_t)(-tz.Bias));
    if (ARG32(1)) vm::write32(ARG32(1), r == TIME_ZONE_ID_DAYLIGHT ? 60 : 0);
    RET(CELL_OK);
}
SYSCALL(145, sys_time_get_current_time) {
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    vm::write64(ARG32(0), (uint64_t)duration_cast<seconds>(now).count());
    vm::write64(ARG32(1), (uint64_t)(duration_cast<nanoseconds>(now).count() % 1000000000));
    RET(CELL_OK);
}
SYSCALL(147, sys_time_get_timebase_frequency) { ctx.r[3] = 79800000; }

uint64_t system_time_us() {
    static const auto start = Clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

// ---------------------------------------------------------------- memory
// sys_memory_allocate(size, flags, alloc_addr*)  flags: 0x400 = 1 MB pages, 0x200 = 64 KB pages
SYSCALL(348, sys_memory_allocate) {
    uint32_t size = ARG32(0), flags = ARG32(1);
    uint32_t align = (flags & 0x400) ? 0x100000 : 0x10000;
    uint32_t a = vm::alloc(vm::Area::User, size, align);
    if (!a) return (void)RET(CELL_ENOMEM);
    vm::write32(ARG32(2), a);
    LOG_I("lv2", "sys_memory_allocate(0x%X, 0x%X) -> 0x%08X", size, flags, a);
    RET(CELL_OK);
}
SYSCALL(349, sys_memory_free) { RET(vm::free(vm::Area::User, ARG32(0)) ? CELL_OK : CELL_EINVAL); }
SYSCALL(350, sys_memory_allocate_from_container) {
    uint32_t size = ARG32(0), flags = ARG32(2);
    uint32_t a = vm::alloc(vm::Area::User, size, (flags & 0x400) ? 0x100000 : 0x10000);
    if (!a) return (void)RET(CELL_ENOMEM);
    vm::write32(ARG32(3), a);
    RET(CELL_OK);
}
SYSCALL(352, sys_memory_get_user_memory_size) {
    // sys_memory_info_t { u32 total_user_memory; u32 available_user_memory; }
    vm::write32(ARG32(0), 0x0D500000);  // 213 MB, as on retail
    vm::write32(ARG32(0) + 4, 0x0D000000);
    RET(CELL_OK);
}
SYSCALL(324, sys_memory_container_create) {
    vm::write32(ARG32(0), kobj_add(std::make_shared<int>(0)));
    RET(CELL_OK);
}
SYSCALL(325, sys_memory_container_destroy) { RET(CELL_OK); }

// ---------------------------------------------------------------- TTY
SYSCALL(403, sys_tty_write) {
    uint32_t buf = ARG32(1), len = ARG32(2);
    std::string s((const char*)vm::ptr(buf), len);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (!s.empty()) LOG_I("tty", "%s", s.c_str());
    if (ARG32(3)) vm::write32(ARG32(3), len);
    RET(CELL_OK);
}
SYSCALL(402, sys_tty_read) {
    if (ARG32(3)) vm::write32(ARG32(3), 0);
    RET(CELL_OK);
}

// ---------------------------------------------------------------- PPU thread syscalls
SYSCALL(41, sys_ppu_thread_exit) { thread_exit(ARG(0)); }
SYSCALL(43, sys_ppu_thread_yield) {
    std::this_thread::yield();
    RET(CELL_OK);
}
SYSCALL(44, sys_ppu_thread_join) {
    uint64_t status = 0;
    uint32_t r = thread_join(ARG32(0), &status);
    if (r == CELL_OK && ARG32(1)) vm::write64(ARG32(1), status);
    RET(r);
}
SYSCALL(45, sys_ppu_thread_detach) { RET(thread_detach(ARG32(0))); }
SYSCALL(47, sys_ppu_thread_set_priority) { RET(CELL_OK); }
SYSCALL(48, sys_ppu_thread_get_priority) {
    vm::write32(ARG32(1), 1000);
    RET(CELL_OK);
}
SYSCALL(49, sys_ppu_thread_get_stack_information) {
    // sys_ppu_thread_stack_t { u32 pst_addr; u32 pst_size; } - report the current stack region
    uint32_t sp = (uint32_t)ctx.r[1];
    vm::write32(ARG32(0), sp & ~0xFFFFFu);
    vm::write32(ARG32(0) + 4, 0x100000);
    RET(CELL_OK);
}
