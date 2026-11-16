// Heap diagnostic, see heap_diag.h.
//
// Locking: every malloc/free already runs under the SDK's pico_malloc
// mutex, but mallinfo() is not wrapped and the probe must hold the heap
// still across many allocations. newlib calls __malloc_lock/__malloc_unlock
// around every heap operation (malloc, free, realloc, mallinfo); the
// library versions are empty. They are overridden here with a recursive
// mutex, so the diagnostic can hold the heap while core 1 (or any caller)
// simply waits inside its malloc for at most the probe's duration.
// Lock order: callers of the wrapped malloc take the SDK mutex, then this
// one; the diagnostic takes ONLY this one and calls __real_malloc/free
// directly, so there is no inversion. Core 0 interrupts are masked while
// the lock is held so an ISR on the same core cannot re-enter the heap
// through the (recursive) lock.

#include "heap_diag.h"

int ramdisk_cow_json(char *buf, size_t len);   // src/mz_devices/ramdisk.cpp

#ifdef USE_PICO_W   // exposed via the REST API only; non-W builds are unchanged

#include <malloc.h>
#include <stdio.h>
#include <unistd.h>

#include "pico/mutex.h"
#include "pico/time.h"
#include "hardware/sync.h"

extern char end;            // heap start (linker script)
extern char __StackLimit;   // _sbrk's limit (== __HeapLimit)

void *__real_malloc(size_t size);
void __real_free(void *ptr);

static recursive_mutex_t g_heap_lock;
static volatile bool g_lock_armed = false;

struct _reent;
void __malloc_lock(struct _reent *r) {
    (void)r;
    if (g_lock_armed) recursive_mutex_enter_blocking(&g_heap_lock);
}
void __malloc_unlock(struct _reent *r) {
    (void)r;
    if (g_lock_armed) recursive_mutex_exit(&g_heap_lock);
}

void heap_diag_init(void) {
    if (g_lock_armed) return;
    recursive_mutex_init(&g_heap_lock);
    __asm volatile("" ::: "memory");
    g_lock_armed = true;
}

static uint32_t g_min_free = UINT32_MAX;
static uint32_t g_free_devices_up = 0;
static uint32_t g_free_wifi_up = 0;
static uint32_t g_samples = 0;
static uint32_t g_next_sample_ms = 0;

typedef struct {
    uint32_t arena, fordblks, uordblks, tail, total_free;
} heap_now_t;

// Caller holds the lock.
static void heap_read_locked(heap_now_t *h) {
    struct mallinfo mi = mallinfo();
    char *brk = (char *)sbrk(0);
    h->arena = (uint32_t)mi.arena;
    h->fordblks = (uint32_t)mi.fordblks;
    h->uordblks = (uint32_t)mi.uordblks;
    h->tail = (brk && brk < &__StackLimit) ? (uint32_t)(&__StackLimit - brk) : 0;
    h->total_free = h->fordblks + h->tail;
}

static uint32_t heap_lock(void) {
    uint32_t irq = save_and_disable_interrupts();
    if (g_lock_armed) recursive_mutex_enter_blocking(&g_heap_lock);
    return irq;
}

static void heap_unlock(uint32_t irq) {
    if (g_lock_armed) recursive_mutex_exit(&g_heap_lock);
    restore_interrupts(irq);
}

static uint32_t sample(void) {
    heap_now_t h;
    uint32_t irq = heap_lock();
    heap_read_locked(&h);
    heap_unlock(irq);
    if (h.total_free < g_min_free) g_min_free = h.total_free;
    g_samples++;
    return h.total_free;
}

void heap_diag_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (g_samples && (int32_t)(now - g_next_sample_ms) < 0) return;
    g_next_sample_ms = now + 100;
    uint32_t f = sample();
    if (g_samples == 1) g_free_devices_up = f;
}

void heap_diag_mark_wifi_up(void) {
    g_free_wifi_up = sample();
}

#define PROBE_MAX_BLOCKS 64
static void *g_probe_ptrs[PROBE_MAX_BLOCKS];

// Caller holds the lock. Allocates blocks of `size` until failure (or the
// table is full), frees them all, returns the count.
static uint32_t count_blocks_locked(size_t size) {
    uint32_t n = 0;
    while (n < PROBE_MAX_BLOCKS) {
        void *p = __real_malloc(size);
        if (!p) break;
        g_probe_ptrs[n++] = p;
    }
    for (uint32_t i = n; i > 0; --i) __real_free(g_probe_ptrs[i - 1]);
    return n;
}

int heap_diag_json(char *buf, size_t len) {
    heap_now_t h;
    uint32_t largest = 0, n16 = 0, n4 = 0;
    uint64_t t0 = time_us_64();

    uint32_t irq = heap_lock();
    heap_read_locked(&h);   // before the probe: the probe moves sbrk to the limit
    // Largest single block: binary search on a successful malloc, each
    // success freed at once. Upper bound: all free bytes plus slack.
    uint32_t lo = 0, hi = h.total_free + 64;
    while (hi - lo > 8) {
        uint32_t mid = (lo + (hi - lo) / 2) & ~7u;
        if (mid <= lo) break;
        void *p = __real_malloc(mid);
        if (p) { __real_free(p); lo = mid; } else { hi = mid; }
    }
    largest = lo;
    n16 = count_blocks_locked(16384);
    n4 = count_blocks_locked(4096);
    heap_unlock(irq);

    uint32_t probe_us = (uint32_t)(time_us_64() - t0);
    if (h.total_free < g_min_free) g_min_free = h.total_free;

    int n = snprintf(buf, len,
        "{\"free\":%lu,\"largest\":%lu,\"blocks_16k\":%lu,\"blocks_4k\":%lu,"
        "\"min_free\":%lu,\"free_devices_up\":%lu,\"free_wifi_up\":%lu,"
        "\"arena\":%lu,\"arena_free\":%lu,\"arena_used\":%lu,\"tail\":%lu,"
        "\"span\":%lu,\"samples\":%lu,\"probe_us\":%lu,\"ramdisk\":",
        (unsigned long)h.total_free, (unsigned long)largest,
        (unsigned long)n16, (unsigned long)n4,
        (unsigned long)(g_min_free == UINT32_MAX ? 0 : g_min_free),
        (unsigned long)g_free_devices_up, (unsigned long)g_free_wifi_up,
        (unsigned long)h.arena, (unsigned long)h.fordblks,
        (unsigned long)h.uordblks, (unsigned long)h.tail,
        (unsigned long)(&__StackLimit - &end),
        (unsigned long)g_samples, (unsigned long)probe_us);
    // The COW RAM disk's pool state rides along (src/mz_devices/ramdisk.cpp)
    if (n > 0 && (size_t)n < len) n += ramdisk_cow_json(buf + n, len - (size_t)n);
    if (n > 0 && (size_t)n + 1 < len) { buf[n++] = '}'; buf[n] = '\0'; }
    return n;
}

#endif // USE_PICO_W
