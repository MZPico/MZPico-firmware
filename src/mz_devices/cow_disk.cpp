// See cow_disk.hpp
#include "cow_disk.hpp"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <strings.h>
#include <new>

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "ff.h"
#include "bus.hpp"
#include "flash_fs.h"
#include "mz_devices.hpp"

constexpr uint32_t COW_DEFAULT_POOL_KB = 72;   // PoP: ~64 blocks + headroom

CowDisk* CowDisk::g_card = nullptr;
static char g_cow_image[64];
static char g_cow_error[96];
static char g_cow_error_dev[16];
static volatile bool g_cow_clear_requested = false;
static volatile bool g_cow_relearn_req = false;
static volatile bool g_cow_freeze_req = false;
static volatile bool g_cow_unfreeze_req = false;
// Reset diagnostics (/api/ramdisk learn): MZ-800 resets seen, the last
// softReset's duration, the deferred persists and the last one's duration
static uint32_t g_cow_resets = 0, g_cow_reset_us = 0, g_cow_persists = 0, g_cow_persist_us = 0;
static uint32_t g_cow_relearn_fails = 0;   // REST relearn requests that could not run (image gone)

uint32_t cow_parse_size(const char* s) {
    if (!s || !*s) return 0;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 0);
    while (end && *end == ' ') end++;
    if (end && (*end == 'k' || *end == 'K')) v <<= 10;
    else if (end && (*end == 'm' || *end == 'M')) v <<= 20;
    return (uint32_t)v;
}

// flash_fs moved a sector to a new page (a rewrite of the image file):
// follow it, so the table never points at a page that gets recycled
static void cow_remap_hook(uint16_t old_entry, uint16_t new_entry) {
    if (CowDisk::card()) CowDisk::card()->image.onRemap(CowDisk::card()->store, old_entry, new_entry);
}

// flash_fs is about to drop sectors (FatFS trim: a delete or truncate)
static void cow_trim_hook(uint32_t first, uint32_t count) {
    if (CowDisk::card()) CowDisk::card()->image.onTrim(CowDisk::card()->store, first, count);
}

// The holes of a blank (learn) image read from this page: 512 zero bytes
// in flash, page-aligned so a sector-table entry can name it
__attribute__((section(".rodata.cow_zero_page"), aligned(512)))
const uint8_t g_cow_zero_page[512] = {0};

static uint64_t cow_now_us(void*) { return time_us_64(); }

static int ini_int(dictionary* ini, const std::string& key, int dflt, int lo, int hi) {
    int v = iniparser_getint(ini, key.c_str(), dflt);
    return v < lo ? lo : (v > hi ? hi : v);
}

// The image file in the internal flash volume, read through XIP; see
// cow_store.hpp, and cow_image.hpp for learn. Any failure skips the device
// (boot continues, the reason is in /api/ramdisk) - never halts.
int CowDisk::open(dictionary* ini, const std::string& id, const std::string& image,
                  uint32_t size_given, uint32_t default_size, CowDisk** out) {
    *out = nullptr;
    CowDisk* d = nullptr;
    auto fail = [&](const char* why) {
        snprintf(g_cow_error, sizeof(g_cow_error), "%s", why);
        snprintf(g_cow_error_dev, sizeof(g_cow_error_dev), "%s", id.c_str());
        printf("%s: mode=cow disabled: %s\n", id.c_str(), g_cow_error);
        delete d;
        return (int)E_DEVICE_NO_MEMORY;   // = skip the device, keep booting
    };
    // The card's one cow disk is taken: keep its REST state as it is
    if (g_card) {
        printf("%s: mode=cow disabled: %s already runs the card's cow disk\n", id.c_str(), g_card->dev_);
        return (int)E_DEVICE_NO_MEMORY;
    }
    // Kept JSON-escaped: it is only ever printed into the REST state
    size_t n = 0;
    for (const char* p = image.c_str(); *p && n + 2 < sizeof(g_cow_image); ++p) {
        if (*p == '\\' || *p == '"') g_cow_image[n++] = '\\';
        else if ((unsigned char)*p < 0x20) continue;
        g_cow_image[n++] = *p;
    }
    g_cow_image[n] = '\0';
    g_cow_error[0] = '\0';
    if (image.empty()) return fail("mode=cow needs image=flash:/...");
    if (strncasecmp(image.c_str(), "flash:", 6) != 0)
        return fail("the image must be on the flash volume (flash:/...)");
    d = new (std::nothrow) CowDisk();
    if (!d) return fail("out of RAM");
    snprintf(d->dev_, sizeof(d->dev_), "%s", id.c_str());
    CowStore& cow = d->store;
    CowImage& cowImage = d->image;
    CowLearnPolicy& learnPolicy_ = d->policy_;

    const uint32_t pool_kb = (uint32_t)ini_int(ini, id + ":cow_pool",
                                               (int)COW_DEFAULT_POOL_KB, 1, (int)COW_MAX_SLOTS);
    // learn: omitted = auto (on for an image the card created, off for one
    // prepared elsewhere); "frozen" = a learn image whose write-back is off
    std::string lv = iniparser_getstring(ini, (id + ":learn").c_str(), "");
    for (auto& c : lv) c = (char)tolower((unsigned char)c);
    int learn = -1;
    if (lv == "true" || lv == "1" || lv == "yes" || lv == "on" || lv == "frozen") learn = 1;
    else if (lv == "false" || lv == "0" || lv == "no" || lv == "off") learn = 0;

    learnPolicy_.converge = (uint16_t)ini_int(ini, id + ":learn_converge", learnPolicy_.converge, 1, 1000);
    learnPolicy_.freeze_after = (uint16_t)ini_int(ini, id + ":learn_freeze_after", 0, 0, 1000);
    CowImageCfg cfg{};
    cfg.policy = learnPolicy_;
    cfg.path = image.c_str();
    cfg.size = size_given ? ((size_given + 0xffff) & 0xffff0000) : 0;
    cfg.default_size = default_size;
    cfg.pool_blocks = pool_kb;
    cfg.learn = learn;
    cfg.flash_base = reinterpret_cast<const uint8_t*>(XIP_BASE);
    cfg.zero_page = g_cow_zero_page;
    cfg.busy = &flash_fs_op_active;

    // A missing or short image is created (blank) here: hold the Z80 in
    // EXWAIT meanwhile (cold-boot IPL race, see pico_rd.cpp). Creating is
    // cheap - clusters allocated, pages trimmed, no data written.
    FILINFO fno;
    const bool may_create = learn != 0 &&
        (f_stat(image.c_str(), &fno) != FR_OK || (cfg.size && fno.fsize < cfg.size));
    if (may_create) set_exwait();
    char why[96];
    const int r = cowImage.open(cfg, cow, why, sizeof(why), cow_now_us, nullptr);
    if (may_create) release_exwait();
    if (r != 0) return fail(why);

    CowLearnCfg lc;
    lc.batch = (uint32_t)ini_int(ini, id + ":learn_batch", (int)lc.batch, 1, 32);
    lc.reserve = (uint32_t)ini_int(ini, id + ":learn_reserve", (int)lc.reserve, 0, (int)pool_kb);
    lc.cap = (uint32_t)ini_int(ini, id + ":learn_cap", (int)lc.cap, 0, 1 << 20);
    lc.sync_every = (uint32_t)ini_int(ini, id + ":learn_sync", (int)lc.sync_every, 1, 4096);
    d->sync_quiet_us_ = (uint64_t)ini_int(ini, id + ":learn_sync_ms", 2000, 100, 600000) * 1000u;
    cow.setLearn(cowImage.io(), lc);
    if (lv == "frozen" && cowImage.mode() != COW_LEARN_FROZEN) cowImage.freeze(cow, true);
    cow.setWriteBack(cowImage.learning());
    // The first RAM-disk access marks a learning session open in the
    // trailer (and persists a power-up's closing of the last one)
    d->persist_due_ = cowImage.persistPending(cow);

    g_card = d;
    *out = d;
    flash_fs_remap_hook = cow_remap_hook;
    flash_fs_trim_hook = cow_trim_hook;
    printf("%s: mode=cow %s, %lu KB, pool %lu KB, learn %s%s\n", id.c_str(), image.c_str(),
           (unsigned long)(cow.stats.size >> 10), (unsigned long)pool_kb,
           cow_learn_mode_name(cowImage.mode()), cowImage.created() ? " (created)" : "");
    return 0;
}

void CowDisk::softReset() {
    // Z80 in reset, bus quiet: the one safe moment for REST-armed changes
    // and the session's end - all of it RAM only, no flash operation and no
    // EXWAIT here: a reset path that does flash work collides with the
    // reset escape hatch (a second press makes core 0 flush and reboot) and
    // freezes the machine. The flash part waits for the next RAM-disk
    // access (touch(), from the device's addressing-port handlers).
    if (get_core_num() != 1) return;
    const uint32_t t0 = time_us_32();
    g_cow_resets++;
    if (g_cow_freeze_req) { g_cow_freeze_req = false; image.freeze(store, true); }
    if (g_cow_unfreeze_req) { g_cow_unfreeze_req = false; image.freeze(store, false); }
    image.endSession(store, policy_, active_);
    active_ = false;
    if (g_cow_relearn_req) {
        g_cow_relearn_req = false;
        // A relearn empties the pool too; if it cannot run, a pending
        // clear still does
        if (image.relearn(store)) g_cow_clear_requested = false;
        else g_cow_relearn_fails++;
    }
    if (g_cow_clear_requested) {
        store.clear();
        g_cow_clear_requested = false;
    }
    persist_due_ = image.persistPending(store);
    g_cow_reset_us = time_us_32() - t0;
}

// An addressing write with flash work possibly due: after a reset (the
// session record), or write-backs that the map does not hold yet and a
// burst that has gone quiet (learn_sync_ms) - at most one persist per burst
RAM_FUNC void CowDisk::touchSlow() {
    if (persist_due_ || image.syncDue(store, time_us_64(), sync_quiet_us_)) persist();
}

void CowDisk::persist() {
    persist_due_ = false;
    const uint32_t t0 = time_us_32();
    image.persist(store);
    g_cow_persist_us = time_us_32() - t0;
    g_cow_persists++;
}

extern "C" void ramdisk_cow_request_clear(void) {
    g_cow_clear_requested = true;
}

extern "C" int ramdisk_cow_request(const char* what) {
    if (!CowDisk::card() || CowDisk::card()->image.mode() == COW_LEARN_OFF) return -1;
    if (!strcmp(what, "relearn")) g_cow_relearn_req = true;
    else if (!strcmp(what, "freeze")) g_cow_freeze_req = true;
    else if (!strcmp(what, "unfreeze")) g_cow_unfreeze_req = true;
    else return -1;
    return 0;
}

static const char* cow_state(const CowStats& st) {
    // image_changed (blocks lost, until power-up) outranks pool_full
    // (writes dropped, until a clear)
    return st.lost ? "image_changed" : (st.pool_full ? "pool_full" : "ok");
}

// learn's state as shown: a converged image that is writing back again is
// learning again (persisted at the next reset)
static const char* cow_learn_state(const CowImage& im, const CowStore& cs) {
    if (im.mode() == COW_LEARN_OFF) return "off";
    if (im.gone()) return "image_gone";
    if (cs.learn.capped && cs.tokens() == 0) return "capped";
    if (cs.learn.fails) return "flash_full";
    if (im.mode() == COW_LEARN_OK && cs.learn.session_blocks) return "learning";
    return cow_learn_mode_name(im.mode());
}

int cow_disk_json(char* buf, size_t len, bool full);

extern "C" int ramdisk_cow_json(char* buf, size_t len) { return cow_disk_json(buf, len, false); }
extern "C" int ramdisk_cow_json_full(char* buf, size_t len) { return cow_disk_json(buf, len, true); }

int cow_disk_json(char* buf, size_t len, bool full) {
    CowDisk* d = CowDisk::g_card;
    if (!d) {
        return snprintf(buf, len, "{\"mode\":\"%s\",\"device\":\"%s\",\"error\":\"%s\"}",
                        g_cow_error[0] ? "cow" : "off", g_cow_error_dev, g_cow_error);
    }
    const CowStore& cs = d->store;
    const CowImage& im = d->image;
    const CowStats& st = cs.stats;
    if (!full) return snprintf(buf, len,
        "{\"mode\":\"cow\",\"device\":\"%s\",\"state\":\"%s\",\"learn\":\"%s\",\"used\":%lu,\"peak\":%lu,"
        "\"drops\":%lu,\"lost\":%lu,\"wb_blocks\":%lu,\"ram\":%lu}",
        d->dev_, cow_state(st), cow_learn_state(im, cs),
        (unsigned long)st.used, (unsigned long)st.peak, (unsigned long)st.drops,
        (unsigned long)st.lost, (unsigned long)cs.learn.blocks, (unsigned long)cs.ramBytes());

    const CowLearnStats& L = cs.learn;
    const CowTrailer& t = im.trailer();
    // Wear: lifetime erases of the learn path over the volume's data
    // sectors, in parts per million of a 100,000-cycle endurance
    // (MZP1: the FAT sector count is exactly the data pages - map chunks
    // excluded, 4 reserved sectors at 2M, 30 at 16M)
    const uint32_t data_sectors = flash_fs_num_fat_sectors() / 8;
    const uint64_t erases_total = (uint64_t)t.erases + im.sessionErases();
    const unsigned long wear_ppm = (unsigned long)(erases_total * 10u / (data_sectors ? data_sectors : 1));
    int n = snprintf(buf, len,
        "{\"mode\":\"cow\",\"device\":\"%s\",\"state\":\"%s\",\"image\":\"%s\",\"size\":%lu,"
        "\"pool_blocks\":%lu,\"used\":%lu,\"peak\":%lu,\"allocs\":%lu,\"reclaims\":%lu,"
        "\"drops\":%lu,\"last_drop\":%lu,\"image_moves\":%lu,\"preserved\":%lu,"
        "\"lost\":%lu,\"pinned\":%lu,\"out_of_range\":%lu,\"flash_waits\":%lu,"
        "\"clears\":%lu,\"clear_pending\":%s,\"ram\":%lu,",
        d->dev_, cow_state(st), g_cow_image, (unsigned long)st.size,
        (unsigned long)st.pool_blocks, (unsigned long)st.used, (unsigned long)st.peak,
        (unsigned long)st.allocs, (unsigned long)st.reclaims, (unsigned long)st.drops,
        (unsigned long)st.last_drop, (unsigned long)st.image_moves,
        (unsigned long)st.preserved, (unsigned long)st.lost, (unsigned long)cs.pinnedCount(),
        (unsigned long)st.out_of_range,
        (unsigned long)st.flash_waits, (unsigned long)st.clears,
        g_cow_clear_requested ? "true" : "false", (unsigned long)cs.ramBytes());
    if (n < 0 || (size_t)n >= len) return n;
    n += snprintf(buf + n, len - (size_t)n,
        "\"learn\":{\"state\":\"%s\",\"created\":%s,\"create_ms\":%lu,\"dirty\":%lu,"
        "\"writebacks\":%lu,\"blocks\":%lu,\"ahead\":%lu,\"sectors\":%lu,\"flash_bytes\":%lu,"
        "\"fails\":%lu,\"stall_ms\":%lu,\"stall_max_ms\":%lu,\"syncs\":%lu,\"erases\":%lu,"
        "\"tokens\":%lu,\"cap_drops\":%lu,\"session_blocks\":%lu,"
        "\"sessions\":%lu,\"clean_sessions\":%u,\"dirty_sessions\":%u,\"relearns\":%u,"
        "\"blocks_total\":%lu,\"erases_total\":%lu,\"wear_ppm\":%lu,"
        "\"volatile\":%lu,\"vol_skips\":%lu,\"vol_forced\":%lu,\"session_vol\":%lu,"
        "\"fs_erases\":%lu,\"fs_copy_erases\":%lu,\"fs_map_syncs\":%lu,\"pending\":\"%s\","
        "\"resets\":%lu,\"reset_us\":%lu,\"persists\":%lu,\"persist_ms\":%lu,\"persist_due\":%s,"
        "\"relearn_fails\":%lu,\"trailer_fails\":%lu}}",
        cow_learn_state(im, cs), im.created() ? "true" : "false",
        (unsigned long)(im.createUs() / 1000), (unsigned long)cs.dirtyCount(),
        (unsigned long)L.batches, (unsigned long)L.blocks, (unsigned long)L.ahead,
        (unsigned long)L.sectors, (unsigned long)L.sectors * 512u, (unsigned long)L.fails,
        (unsigned long)(L.stall_us / 1000), (unsigned long)(L.stall_max_us / 1000),
        (unsigned long)L.syncs, (unsigned long)im.erases(),
        (unsigned long)cs.tokens(), (unsigned long)L.cap_drops, (unsigned long)L.session_blocks,
        (unsigned long)t.sessions, (unsigned)t.clean, (unsigned)t.dirty, (unsigned)t.relearns,
        (unsigned long)(t.wb_blocks + L.session_blocks), (unsigned long)erases_total, wear_ppm,
        (unsigned long)cs.volatileCount(), (unsigned long)L.vol_skips, (unsigned long)L.vol_forced,
        (unsigned long)L.session_vol,
        (unsigned long)flash_fs_erases, (unsigned long)flash_fs_copy_erases,
        (unsigned long)flash_fs_map_syncs,
        g_cow_relearn_req ? "relearn" : (g_cow_freeze_req ? "freeze" : (g_cow_unfreeze_req ? "unfreeze" : "")),
        (unsigned long)g_cow_resets, (unsigned long)g_cow_reset_us, (unsigned long)g_cow_persists,
        (unsigned long)(g_cow_persist_us / 1000), d->persist_due_ ? "true" : "false",
        (unsigned long)g_cow_relearn_fails, (unsigned long)im.trailerFails());
    return n;
}

