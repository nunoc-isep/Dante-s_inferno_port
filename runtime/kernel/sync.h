// Kernel object table and the sync primitives other modules share.
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "kernel.h"

uint32_t kobj_add(std::shared_ptr<void> obj);
std::shared_ptr<void> kobj_get_raw(uint32_t id);
bool kobj_remove(uint32_t id);
// Objects are type-erased; callers must request the type they created.
template <typename T>
std::shared_ptr<T> kobj_get(uint32_t id) {
    return std::static_pointer_cast<T>(kobj_get_raw(id));
}

struct KMutex {
    std::mutex m;
    std::condition_variable cv;
    uint32_t owner = 0, count = 0;
    bool recursive = false;
    uint32_t lock(uint32_t tid, uint64_t timeout_us);
    uint32_t trylock(uint32_t tid);
    uint32_t unlock(uint32_t tid);
};

struct KEvent {
    uint64_t source, d1, d2, d3;
};
struct KEventQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<KEvent> events;
    uint64_t key = 0;
    uint32_t size = 0;
    bool destroyed = false;
    void push(uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3);
};
std::shared_ptr<KEventQueue> event_queue_get(uint32_t id);

uint64_t system_time_us();
