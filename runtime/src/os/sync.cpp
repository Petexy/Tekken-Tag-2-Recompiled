// OSMutex, OSCond, OSSemaphore, OSEvent, OSMessageQueue, OSThreadQueue and
// the interrupt lock. Observable state lives in the game's structures
// (layouts as in Cemu's coreinit headers); host maps hold wake-up
// generations for objects whose state is only "who is sleeping".

#include "kernel.h"

#include "cafe/export.h"

#include <cstring>
#include <unordered_map>

namespace cafe::os {
namespace {

// ------------------------------------------------------------ interrupts
// Disabling interrupts on a Cafe core stops preemption there; games use it
// as a lock against other threads on that core and against alarm callbacks.
// With every guest thread truly parallel it has to exclude all of them.
Thread* g_interrupt_owner = nullptr;

constexpr uint32_t kMutexMagic = 0x6D557458;     // 'mUtX'
constexpr uint32_t kCondMagic = 0x634E6456;      // 'cNdV'
constexpr uint32_t kSemaphoreMagic = 0x73506852; // 'sPhR'
constexpr uint32_t kEventMagic = 0x65566E54;     // 'eVnT'
constexpr uint32_t kQueueMagic = 0x6D536751;     // 'mSgQ'

// Wake-up generation per object address (conditions, thread queues).
std::unordered_map<uint32_t, uint64_t> g_generations;

// OSMutex: +0x1C owner, +0x20 lock count. OSThreadQueue at +0x0C.
uint32_t mutex_owner(uint32_t m) { return field<uint32_t>(m, 0x1C); }

void lock_mutex(KernelLock& lock, uint32_t mutex, uint32_t self) {
    wait_until(lock, [&] {
        const uint32_t owner = mutex_owner(mutex);
        return owner == 0 || owner == self;
    });
    field<uint32_t>(mutex, 0x1C) = self;
    field<int32_t>(mutex, 0x20) += 1;
}

void unlock_mutex(uint32_t mutex, uint32_t self) {
    if (mutex_owner(mutex) != self) {
        fatal("OSUnlockMutex on 0x%08X by a thread that does not own it", mutex);
    }
    be<int32_t>& count = field<int32_t>(mutex, 0x20);
    count -= 1;
    if (count == 0) {
        field<uint32_t>(mutex, 0x1C) = 0;
        wake_all();
    }
}

} // namespace

void acquire_interrupt_lock(KernelLock& lock, Thread* self) {
    kernel_cv().wait(lock, [&] { return g_interrupt_owner == nullptr || g_interrupt_owner == self; });
    g_interrupt_owner = self;
    self->interrupts_disabled = true;
}

void release_interrupt_lock_for_wait(Thread* self, bool& was_held) {
    was_held = self != nullptr && g_interrupt_owner == self;
    if (was_held) {
        g_interrupt_owner = nullptr;
        self->interrupts_disabled = false;
        wake_all();
    }
}

void reacquire_interrupt_lock_after_wait(KernelLock& lock, Thread* self, bool was_held) {
    if (was_held) {
        acquire_interrupt_lock(lock, self);
    }
}

namespace {

uint32_t OSDisableInterrupts() {
    KernelLock lock(kernel_mutex());
    Thread* self = current_thread();
    if (self->interrupts_disabled) return 0;
    acquire_interrupt_lock(lock, self);
    return 1;
}

uint32_t OSRestoreInterrupts(uint32_t enable) {
    KernelLock lock(kernel_mutex());
    Thread* self = current_thread();
    const uint32_t previous = self->interrupts_disabled ? 0 : 1;
    if (enable && self->interrupts_disabled) {
        bool held = false;
        release_interrupt_lock_for_wait(self, held);
    } else if (!enable && !self->interrupts_disabled) {
        acquire_interrupt_lock(lock, self);
    }
    return previous;
}

// ------------------------------------------------------------------ mutex
void OSInitMutex(GuestAddress mutex) {
    std::memset(mutex.as<uint8_t>(), 0, 0x2C);
    field<uint32_t>(mutex.value, 0) = kMutexMagic;
}

void OSLockMutex(GuestAddress mutex) {
    KernelLock lock(kernel_mutex());
    lock_mutex(lock, mutex.value, current_thread()->guest);
}

bool OSTryLockMutex(GuestAddress mutex) {
    KernelLock lock(kernel_mutex());
    const uint32_t self = current_thread()->guest;
    const uint32_t owner = mutex_owner(mutex.value);
    if (owner != 0 && owner != self) return false;
    field<uint32_t>(mutex.value, 0x1C) = self;
    field<int32_t>(mutex.value, 0x20) += 1;
    return true;
}

void OSUnlockMutex(GuestAddress mutex) {
    KernelLock lock(kernel_mutex());
    unlock_mutex(mutex.value, current_thread()->guest);
}

// -------------------------------------------------------------- condition
void OSInitCond(GuestAddress cond) {
    std::memset(cond.as<uint8_t>(), 0, 0x1C);
    field<uint32_t>(cond.value, 0) = kCondMagic;
}

// Releases the mutex completely, waits for a signal, then restores the
// mutex with its previous recursion count.
void OSWaitCond(GuestAddress cond, GuestAddress mutex) {
    KernelLock lock(kernel_mutex());
    const uint32_t self = current_thread()->guest;
    if (mutex_owner(mutex.value) != self) {
        fatal("OSWaitCond with mutex 0x%08X not held by the caller", mutex.value);
    }
    const int32_t depth = field<int32_t>(mutex.value, 0x20);
    field<int32_t>(mutex.value, 0x20) = 0;
    field<uint32_t>(mutex.value, 0x1C) = 0;
    wake_all();
    const uint64_t generation = g_generations[cond.value];
    wait_until(lock, [&] { return g_generations[cond.value] != generation; });
    wait_until(lock, [&] { return mutex_owner(mutex.value) == 0; });
    field<uint32_t>(mutex.value, 0x1C) = self;
    field<int32_t>(mutex.value, 0x20) = depth;
}

// Wakes every thread waiting on the condition.
void OSSignalCond(GuestAddress cond) {
    KernelLock lock(kernel_mutex());
    ++g_generations[cond.value];
    wake_all();
}

// -------------------------------------------------------------- semaphore
// OSSemaphore: +0x0C count.
void OSInitSemaphore(GuestAddress semaphore, int32_t count) {
    std::memset(semaphore.as<uint8_t>(), 0, 0x20);
    field<uint32_t>(semaphore.value, 0) = kSemaphoreMagic;
    field<int32_t>(semaphore.value, 0x0C) = count;
}

int32_t OSWaitSemaphore(GuestAddress semaphore) {
    KernelLock lock(kernel_mutex());
    be<int32_t>& count = field<int32_t>(semaphore.value, 0x0C);
    wait_until(lock, [&] { return count > 0; });
    const int32_t previous = count;
    count = previous - 1;
    return previous;
}

int32_t OSTryWaitSemaphore(GuestAddress semaphore) {
    KernelLock lock(kernel_mutex());
    be<int32_t>& count = field<int32_t>(semaphore.value, 0x0C);
    const int32_t previous = count;
    if (previous > 0) count = previous - 1;
    return previous;
}

int32_t OSSignalSemaphore(GuestAddress semaphore) {
    KernelLock lock(kernel_mutex());
    be<int32_t>& count = field<int32_t>(semaphore.value, 0x0C);
    const int32_t previous = count;
    count = previous + 1;
    wake_all();
    return previous;
}

// ------------------------------------------------------------------ event
// OSEvent: +0x0C state (0 unsignalled, 1 signalled), +0x20 mode (0 manual
// reset, 1 auto reset). As on hardware, an auto-reset event signalled while
// threads wait hands the signal to a waiter (or, for SignalAll, to every
// waiter) and stays unsignalled; it only latches when nobody waits.
struct EventWaiters {
    uint32_t waiting = 0;
    uint32_t handoffs = 0;   // single wake-ups not yet claimed
    uint64_t generation = 0; // bumped to release every current waiter
};
std::unordered_map<uint32_t, EventWaiters> g_events;

bool event_auto(uint32_t e) { return field<uint32_t>(e, 0x20) == 1u; }

void OSInitEvent(GuestAddress event, uint32_t signalled, uint32_t mode) {
    std::memset(event.as<uint8_t>(), 0, 0x24);
    field<uint32_t>(event.value, 0) = kEventMagic;
    field<uint32_t>(event.value, 0x0C) = signalled ? 1u : 0u;
    field<uint32_t>(event.value, 0x20) = mode;
    g_events.erase(event.value);
}

// Returns whether the event was obtained (false only on timeout).
bool wait_event(KernelLock& lock, uint32_t e, const uint64_t* timeout_ns) {
    be<uint32_t>& state = field<uint32_t>(e, 0x0C);
    if (state != 0u) {
        if (event_auto(e)) state = 0u;
        return true;
    }
    if (timeout_ns != nullptr && *timeout_ns == 0) return false;
    EventWaiters& w = g_events[e];
    const uint64_t generation = w.generation;
    ++w.waiting;
    const auto ready = [&] { return state != 0u || w.handoffs > 0 || w.generation != generation; };
    bool obtained = true;
    if (timeout_ns != nullptr) {
        obtained = wait_until_for(lock, *timeout_ns, ready);
    } else {
        wait_until(lock, ready);
    }
    --w.waiting;
    if (!obtained) return false;
    if (w.generation != generation) return true;
    if (w.handoffs > 0) {
        --w.handoffs;
        return true;
    }
    if (event_auto(e)) state = 0u;
    return true;
}

void OSWaitEvent(GuestAddress event) {
    KernelLock lock(kernel_mutex());
    wait_event(lock, event.value, nullptr);
}

bool OSWaitEventWithTimeout(GuestAddress event, uint64_t timeout_ns) {
    KernelLock lock(kernel_mutex());
    return wait_event(lock, event.value, &timeout_ns);
}

void signal_event(uint32_t e, bool everyone) {
    be<uint32_t>& state = field<uint32_t>(e, 0x0C);
    if (state != 0u) return;
    EventWaiters& w = g_events[e];
    if (!event_auto(e)) {
        state = 1u;
        ++w.generation;
    } else if (w.waiting > w.handoffs) {
        if (everyone) {
            ++w.generation;
            w.handoffs = 0;
        } else {
            ++w.handoffs;
        }
    } else {
        state = 1u;
    }
    wake_all();
}

void OSSignalEvent(GuestAddress event) {
    KernelLock lock(kernel_mutex());
    signal_event(event.value, false);
}

void OSSignalEventAll(GuestAddress event) {
    KernelLock lock(kernel_mutex());
    signal_event(event.value, true);
}

void OSResetEvent(GuestAddress event) {
    KernelLock lock(kernel_mutex());
    field<uint32_t>(event.value, 0x0C) = 0u;
}

// ----------------------------------------------------------- message queue
// OSMessageQueue: +0x2C message array, +0x30 capacity, +0x34 first index,
// +0x38 used count. A message is four words.
constexpr uint32_t kMessageBlocking = 1;
constexpr uint32_t kMessageHighPriority = 2;

void OSInitMessageQueue(GuestAddress queue, uint32_t messages, int32_t count) {
    std::memset(queue.as<uint8_t>(), 0, 0x3C);
    field<uint32_t>(queue.value, 0) = kQueueMagic;
    field<uint32_t>(queue.value, 0x2C) = messages;
    field<uint32_t>(queue.value, 0x30) = static_cast<uint32_t>(count);
}

bool OSSendMessage(GuestAddress queue, GuestAddress message, uint32_t flags) {
    KernelLock lock(kernel_mutex());
    const uint32_t q = queue.value;
    const uint32_t capacity = field<uint32_t>(q, 0x30);
    be<uint32_t>& used = field<uint32_t>(q, 0x38);
    be<uint32_t>& first = field<uint32_t>(q, 0x34);
    if (used == capacity) {
        if (!(flags & kMessageBlocking)) return false;
        wait_until(lock, [&] { return used < capacity; });
    }
    uint32_t slot;
    if (flags & kMessageHighPriority) {
        // Jammed messages go in front of everything queued.
        first = (first + capacity - 1) % capacity;
        slot = first;
    } else {
        slot = (first + used) % capacity;
    }
    std::memcpy(guest_pointer(field<uint32_t>(q, 0x2C) + slot * 16), message.as<uint8_t>(), 16);
    used += 1;
    wake_all();
    return true;
}

bool OSReceiveMessage(GuestAddress queue, GuestAddress message, uint32_t flags) {
    KernelLock lock(kernel_mutex());
    const uint32_t q = queue.value;
    const uint32_t capacity = field<uint32_t>(q, 0x30);
    be<uint32_t>& used = field<uint32_t>(q, 0x38);
    be<uint32_t>& first = field<uint32_t>(q, 0x34);
    if (used == 0u) {
        if (!(flags & kMessageBlocking)) return false;
        wait_until(lock, [&] { return used != 0u; });
    }
    std::memcpy(message.as<uint8_t>(), guest_pointer(field<uint32_t>(q, 0x2C) + first * 16), 16);
    first = (first + 1) % capacity;
    used -= 1;
    wake_all();
    return true;
}

// ----------------------------------------------------------- thread queue
void OSInitThreadQueue(GuestAddress queue) { std::memset(queue.as<uint8_t>(), 0, 0x10); }

void OSSleepThread(GuestAddress queue) {
    KernelLock lock(kernel_mutex());
    const uint64_t generation = g_generations[queue.value];
    wait_until(lock, [&] { return g_generations[queue.value] != generation; });
}

void OSWakeupThread(GuestAddress queue) {
    KernelLock lock(kernel_mutex());
    ++g_generations[queue.value];
    wake_all();
}

} // namespace

CAFE_EXPORT(coreinit, OSDisableInterrupts, OSDisableInterrupts);
CAFE_EXPORT(coreinit, OSRestoreInterrupts, OSRestoreInterrupts);
CAFE_EXPORT(coreinit, OSInitMutex, OSInitMutex);
CAFE_EXPORT(coreinit, OSLockMutex, OSLockMutex);
CAFE_EXPORT(coreinit, OSTryLockMutex, OSTryLockMutex);
CAFE_EXPORT(coreinit, OSUnlockMutex, OSUnlockMutex);
CAFE_EXPORT(coreinit, OSInitCond, OSInitCond);
CAFE_EXPORT(coreinit, OSWaitCond, OSWaitCond);
CAFE_EXPORT(coreinit, OSSignalCond, OSSignalCond);
CAFE_EXPORT(coreinit, OSInitSemaphore, OSInitSemaphore);
CAFE_EXPORT(coreinit, OSWaitSemaphore, OSWaitSemaphore);
CAFE_EXPORT(coreinit, OSTryWaitSemaphore, OSTryWaitSemaphore);
CAFE_EXPORT(coreinit, OSSignalSemaphore, OSSignalSemaphore);
CAFE_EXPORT(coreinit, OSInitEvent, OSInitEvent);
CAFE_EXPORT(coreinit, OSWaitEvent, OSWaitEvent);
CAFE_EXPORT(coreinit, OSWaitEventWithTimeout, OSWaitEventWithTimeout);
CAFE_EXPORT(coreinit, OSSignalEvent, OSSignalEvent);
CAFE_EXPORT(coreinit, OSSignalEventAll, OSSignalEventAll);
CAFE_EXPORT(coreinit, OSResetEvent, OSResetEvent);
CAFE_EXPORT(coreinit, OSInitMessageQueue, OSInitMessageQueue);
CAFE_EXPORT(coreinit, OSSendMessage, OSSendMessage);
CAFE_EXPORT(coreinit, OSReceiveMessage, OSReceiveMessage);
CAFE_EXPORT(coreinit, OSInitThreadQueue, OSInitThreadQueue);
CAFE_EXPORT(coreinit, OSSleepThread, OSSleepThread);
CAFE_EXPORT(coreinit, OSWakeupThread, OSWakeupThread);

} // namespace cafe::os
