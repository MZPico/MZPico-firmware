#pragma once

// The flash-image RAM-disk backend shared by [ramdisk] and [pico_rd]
// (mode=cow, with or without learn): a CowStore (cow_store.hpp) over a
// CowImage (cow_image.hpp), the ini keys, the reset/persist flow, the
// flash_fs hooks and the REST state.
//
// One per card: the pool alone is 72 KB by default and no Pico W heap
// holds two. The first device configured with mode=cow gets it; a second
// one is skipped at boot with the reason in /api/ramdisk.
//
// The owning device keeps its own addressing (MR1R18 pages for [ramdisk],
// a 24-bit counter for [pico_rd]) and calls, from its RAM_FUNC port
// handlers: read()/write() with a position below size(), and touch() on
// every addressing port write (session activity, and the deferred flash
// work after a reset - the Z80 held by EXWAIT there). softReset() from the
// device's softReset (core 1, Z80 in reset: RAM only).

#include <stdint.h>
#include <string>
#include "common.hpp"
#include "cow_store.hpp"
#include "cow_image.hpp"
#include "iniparser.h"

class CowDisk {
public:
    // Parses the [id] cow/learn keys, opens (or creates) the image. On
    // success *out is the card's CowDisk; on failure nullptr, the reason
    // kept for /api/ramdisk. Returns 0 or E_DEVICE_NO_MEMORY (= skip the
    // device, keep booting). size_given: bytes, 0 = the image's own size;
    // default_size: when creating with size_given 0.
    static int open(dictionary* ini, const std::string& id, const std::string& image,
                    uint32_t size_given, uint32_t default_size, CowDisk** out);
    static CowDisk* card() { return g_card; }

    ALWAYS_INLINE uint8_t read(uint32_t pos) { return store.read(pos); }
    ALWAYS_INLINE void write(uint32_t pos, uint8_t v) { store.write(pos, v); }
    ALWAYS_INLINE void touch() {
        active_ = true;
        if (__builtin_expect(persist_due_ || store.syncPending(), 0)) touchSlow();
    }
    uint32_t size() const { return store.stats.size; }
    void softReset();

    const char* device() const { return dev_; }
    CowStore store;
    CowImage image;

private:
    CowDisk() = default;
    void persist();
    RAM_FUNC void touchSlow();
    uint64_t sync_quiet_us_ = 2000000;  // learn_sync_ms
    static CowDisk* g_card;
    CowLearnPolicy policy_;
    bool active_ = false;       // addressed this session (learn's convergence)
    bool persist_due_ = false;  // learn: the trailer / map sync deferred from a reset
    char dev_[16] = "";
    friend int cow_disk_json(char*, size_t, bool);
};

// "524288", "0x80000", "512K", "512k", "1M": bytes
uint32_t cow_parse_size(const char* s);

// REST / diagnostics (core 0). The JSON is the cow disk's state, whichever
// device runs it ("device": "ramdisk" | "pico_rd"). Plain C linkage: the C
// heap diagnostic embeds the brief form.
extern "C" int ramdisk_cow_json(char* buf, size_t len);        // brief (inside /api/heap)
extern "C" int ramdisk_cow_json_full(char* buf, size_t len);   // GET /api/ramdisk
extern "C" void ramdisk_cow_request_clear(void);
// learn: "relearn" | "freeze" | "unfreeze", applied at the next Z80 reset;
// -1 when there is no learn disk
extern "C" int ramdisk_cow_request(const char* what);
