// Host tests of [ramdisk] learn: src/mz_devices/cow_image.* + the store's
// write-back, on the REAL src/flash_fs.c (log-structured remap, quarantine,
// double-buffered map, erase-with-copy) over an emulated NOR chip mapped
// at the RP2040's XIP address (flash_sim.cpp) and the real FatFS.
//
//  1. blank creation: a missing image is created as holes (no data pages),
//     reads as zeros, costs a few sector writes; learn auto/off/on rules
//  2. write-back correctness: under heavy pool pressure every served byte
//     is what the Z80 wrote; after a power cycle the image holds exactly
//     the written-back blocks; NOR rules never violated
//  3. convergence: learning -> ok after clean sessions, a new version
//     relearns, freeze / unfreeze / relearn, learn_freeze_after
//  4. the wear cap: the token bucket stops the write-back, drops counted,
//     refills over time
//  5. remap during write-back: an external rewrite of the image pins the
//     power-up snapshot, the write-back's own rewrites pin nothing; an
//     external delete stops the write-back
//  6. power cuts: flash snapshots at (and torn inside) flash operations of
//     a learning run all mount, keep the other files intact, and every
//     image sector holds a version the Z80 wrote (or the blank)
//  7. the trace replay (argv[1] or COW_TRACE=...rdlog): a real Prince of
//     Persia RAM-disk trace from a blank image over ten sessions, MZ-800
//     resets and power cycles mixed, the title/story phase cut short or
//     repeated with different scratch content each time; write-backs per
//     phase, convergence, the volatile (hot) blocks - at pool 72 and 40

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <set>
#include <map>
#include <string>
#include <random>
#include <atomic>

#include "ff.h"
#include "flash_fs.h"
#include "flash_sim.h"
#include "cow_store.hpp"
#include "cow_image.hpp"

extern int g_fail;
extern bool g_real_flash;
extern bool g_real_no_trim;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static uint8_t* const CHIP = (uint8_t*)(uintptr_t)XIP_BASE;
static const uint32_t REGION = 1024 * 1024;              // flash_fs: the upper 1 MB
static const uint8_t* const ZERO = CHIP + 0x80000;       // "firmware" half: a zero page
static uint64_t g_trace_us = 0;                          // Z80 time (the trace's clock)
static uint32_t now_us(void*) { return (uint32_t)(g_trace_us + sim_flash_us); }
static std::atomic<uint32_t> g_lbusy{0};
static FATFS g_lfs;

static uint8_t pat(uint32_t pos, uint32_t seed) {
    uint32_t x = (pos + 1) * 2654435761u ^ (seed * 0x9E3779B9u);
    return (uint8_t)(x >> 13 ^ x >> 24);
}

static void write_file(const char* path, uint32_t n, uint32_t seed) {
    FIL f;
    CHECK(f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    std::vector<uint8_t> b(n);
    for (uint32_t i = 0; i < n; i++) b[i] = pat(i, seed);
    UINT w = 0;
    CHECK(f_write(&f, b.data(), n, &w) == FR_OK && w == n);
    CHECK(f_close(&f) == FR_OK);
}

static bool file_ok(const char* path, uint32_t n, uint32_t seed) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    std::vector<uint8_t> b(n);
    UINT got = 0;
    bool ok = f_size(&f) == n && f_read(&f, b.data(), n, &got) == FR_OK && got == n;
    f_close(&f);
    for (uint32_t i = 0; ok && i < n; i++) ok = b[i] == pat(i, seed);
    return ok;
}

struct OtherFile { const char* path; uint32_t n, seed; };
static const OtherFile OTHERS[] = {
    {"flash:/mzpico.ini", 1500, 11}, {"flash:/GAME.MZF", 40000, 12}, {"flash:/pico_rd.img", 65536, 13},
};

// A fresh 2 MB chip, the volume formatted like create_fatfs_disk(), the
// usual small files on it
static void real_volume() {
    g_real_flash = true;
    f_mount(nullptr, "flash:", 0);
    memset(CHIP + REGION, 0xFF, REGION);
    memset((void*)ZERO, 0, 512);
    flash_fs_create();
    static BYTE work[FF_MAX_SS * 4];
    CHECK(f_mkfs("flash:", 0, work, sizeof work) == FR_OK);
    CHECK(f_mount(&g_lfs, "flash:", 1) == FR_OK);
    for (const auto& o : OTHERS) write_file(o.path, o.n, o.seed);
}

// Power cycle: the RAM state of flash_fs and FatFS is gone, the chip stays
static bool remount() {
    f_mount(nullptr, "flash:", 0);
    if (flash_fs_mount() != 0) return false;
    return f_mount(&g_lfs, "flash:", 1) == FR_OK;
}

static uint32_t used_pages() {
    uint32_t n = 0;
    for (uint32_t s = 0; s < flash_fs_num_fat_sectors(); s++) n += flash_fs_map_entry(s) != 0;
    return n;
}

// The firmware's RamDisk, reduced to the COW parts
struct LDisk {
    CowStore st;
    CowImage im;
    CowLearnPolicy pol;
    char err[96] = "";
    static LDisk* cur;
    static void remapH(uint16_t o, uint16_t n) { if (cur) cur->im.onRemap(cur->st, o, n); }
    static void trimH(uint32_t f, uint32_t c) { if (cur) cur->im.onTrim(cur->st, f, c); }
    int open(const char* path, uint32_t size, uint32_t pool, int learn, const CowLearnCfg& lc = CowLearnCfg()) {
        CowImageCfg c{};
        c.policy = pol;
        c.path = path; c.size = size; c.default_size = 65536; c.pool_blocks = pool; c.learn = learn;
        c.flash_base = CHIP; c.zero_page = ZERO; c.busy = &g_lbusy;
        const int r = im.open(c, st, err, sizeof err, now_us, nullptr);
        if (r) return r;
        st.setLearn(im.io(), lc);
        st.setWriteBack(im.learning());
        cur = this;
        flash_fs_remap_hook = remapH;
        flash_fs_trim_hook = trimH;
        return 0;
    }
    void close() {
        flash_fs_remap_hook = nullptr;
        flash_fs_trim_hook = nullptr;
        cur = nullptr;
        im.close();
        st.release();
    }
    ~LDisk() { if (cur == this) close(); }
    // An MZ-800 reset (RAM only), then - as the firmware does at the next
    // RAM-disk address write - the deferred flash work
    void reset(bool active = true) { resetOnly(active); im.persist(st); }
    void resetOnly(bool active = true) { im.endSession(st, pol, active); }
};
LDisk* LDisk::cur = nullptr;

static uint32_t mismatches(CowStore& st, const std::vector<uint8_t>& model) {
    uint32_t bad = 0;
    for (uint32_t i = 0; i < model.size(); i++) bad += st.read(i) != model[i];
    return bad;
}

static void write_block(CowStore& st, std::vector<uint8_t>& model, uint32_t blk, uint32_t seed) {
    for (uint32_t o = 0; o < 1024; o++) {
        const uint32_t p = blk * 1024 + o;
        const uint32_t d0 = st.stats.drops;
        st.write(p, pat(p, seed));
        if (st.stats.drops == d0) model[p] = pat(p, seed);
    }
}

// ---------------- 1. creation ----------------
static void test_create() {
    printf("learn: blank image creation\n");
    real_volume();
    // A deleted old image: TRIM gives its pages back
    write_file("flash:/OLD.IMG", 512 * 1024, 3);
    const uint32_t with_old = used_pages();
    CHECK(f_unlink("flash:/OLD.IMG") == FR_OK);
    const uint32_t without = used_pages();
    printf("  deleting a 512 KB file: %lu -> %lu flash pages in use (TRIM)\n",
           (unsigned long)with_old, (unsigned long)without);
    CHECK(with_old - without >= 1024);

    // ... and one deleted without TRIM (older firmware, or over USB): its
    // pages stay occupied until the creation below reclaims the free space
    write_file("flash:/OLD2.IMG", 512 * 1024, 4);
    g_real_no_trim = true;
    CHECK(f_unlink("flash:/OLD2.IMG") == FR_OK);
    g_real_no_trim = false;
    const uint32_t leaked = used_pages();
    printf("  deleting it without TRIM: %lu flash pages stay in use\n", (unsigned long)leaked);
    CHECK(leaked >= without + 1024);
    {   // learn=false: a missing image is an error, as before
        LDisk d;
        CHECK(d.open("flash:/POP.IMG", 512 * 1024, 64, 0) != 0);
        printf("  learn=false, missing: %s\n", d.err);
    }
    const uint64_t t0 = sim_flash_us;
    const uint32_t e0 = sim_erases, p0 = sim_programs;
    LDisk d;
    CHECK(d.open("flash:/POP.IMG", 512 * 1024, 64, -1) == 0);
    printf("  created 512 KB: %lu ms simulated flash time, %lu erases, %lu page programs, "
           "%lu flash pages in use\n", (unsigned long)((sim_flash_us - t0) / 1000),
           (unsigned long)(sim_erases - e0), (unsigned long)(sim_programs - p0),
           (unsigned long)used_pages());
    CHECK(d.im.created() && d.im.mode() == COW_LEARN_LEARNING && d.im.hasTrailer());
    CHECK(d.st.writeBackOn());
    printf("  after creating: %lu flash pages in use (the leaked ones reclaimed)\n", (unsigned long)used_pages());
    CHECK(used_pages() <= without + 16);    // the trailer + FAT/dir sectors, no data
    CHECK(sim_erases - e0 <= 16);           // FAT + directory sectors, map syncs
    FILINFO fno;
    CHECK(f_stat("flash:/POP.IMG", &fno) == FR_OK && fno.fsize == 512 * 1024 + 512);
    std::vector<uint8_t> zeros(512 * 1024, 0);
    CHECK(mismatches(d.st, zeros) == 0);
    bool all_zero_page = true;
    for (uint32_t i = 0; i < d.st.sectorCount(); i++) all_zero_page = all_zero_page && d.st.sectorEntry(i) == d.im.zeroEntry();
    CHECK(all_zero_page);
    d.close();

    // A reboot: the learn image is recognized (auto = on), not recreated
    CHECK(remount());
    LDisk d2;
    CHECK(d2.open("flash:/POP.IMG", 512 * 1024, 64, -1) == 0);
    CHECK(!d2.im.created() && d2.im.mode() == COW_LEARN_LEARNING);
    d2.close();
    // size= omitted: the image's own size (the trailer does not count)
    CHECK(d2.open("flash:/POP.IMG", 0, 64, -1) == 0);
    CHECK(d2.st.stats.size == 512 * 1024);
    d2.close();

    // A prepared image (no trailer): auto = plain cow; learn=true adds the trailer
    write_file("flash:/PREP.IMG", 128 * 1024, 7);
    LDisk p;
    CHECK(p.open("flash:/PREP.IMG", 0, 16, -1) == 0);
    CHECK(p.im.mode() == COW_LEARN_OFF && !p.st.writeBackOn());
    std::vector<uint8_t> prep(128 * 1024);
    for (uint32_t i = 0; i < prep.size(); i++) prep[i] = pat(i, 7);
    CHECK(mismatches(p.st, prep) == 0);
    p.close();
    CHECK(p.open("flash:/PREP.IMG", 0, 16, 1) == 0);
    CHECK(p.im.mode() == COW_LEARN_LEARNING && p.im.hasTrailer() && !p.im.created());
    CHECK(mismatches(p.st, prep) == 0);
    p.close();
    CHECK(f_stat("flash:/PREP.IMG", &fno) == FR_OK && fno.fsize == 128 * 1024 + 512);
    // A short prepared image: kept (an error) unless learn=true recreates it
    write_file("flash:/SHORT.IMG", 40 * 1024, 8);
    CHECK(p.open("flash:/SHORT.IMG", 64 * 1024, 16, -1) != 0);
    printf("  short prepared image, learn auto: %s\n", p.err);
    CHECK(file_ok("flash:/SHORT.IMG", 40 * 1024, 8));
    CHECK(p.open("flash:/SHORT.IMG", 64 * 1024, 16, 1) == 0);
    CHECK(p.im.created() && p.st.stats.size == 64 * 1024);
    p.close();
    // A learn image with a larger size= (auto): recreated at the new size
    CHECK(p.open("flash:/SHORT.IMG", 128 * 1024, 16, -1) == 0);
    CHECK(p.im.created() && p.st.stats.size == 128 * 1024);
    p.close();
    for (const auto& o : OTHERS) CHECK(file_ok(o.path, o.n, o.seed));
}

// ---------------- 2. write-back correctness ----------------
static void test_writeback() {
    printf("learn: write-back correctness\n");
    real_volume();
    LDisk d;
    const uint32_t SIZE = 512 * 1024;
    CowLearnCfg lc;
    lc.cap = 1u << 30;         // a stress far past the default cap (see test_cap)
    CHECK(d.open("flash:/POP.IMG", SIZE, 16, -1, lc) == 0);
    std::vector<uint8_t> model(SIZE, 0);
    std::mt19937 rng(4242);
    std::vector<uint32_t> blks(150);
    for (auto& b : blks) b = rng() % (SIZE / 1024);
    const uint32_t over0 = sim_overprograms;
    uint32_t mism = 0;
    for (uint32_t op = 0; op < 300000; op++) {
        const uint32_t pos = blks[rng() % blks.size()] * 1024 + rng() % 1024;
        const uint32_t k = rng() % 4;
        if (k == 0) { mism += d.st.read(pos) != model[pos]; continue; }
        const uint8_t v = k == 1 ? 0 : (uint8_t)rng();        // 1 in 3 back to the blank
        const uint32_t d0 = d.st.stats.drops;
        d.st.write(pos, v);
        if (d.st.stats.drops == d0) model[pos] = v;
    }
    const CowLearnStats& L = d.st.learn;
    printf("  %lu write-backs, %lu blocks, %lu sectors, %lu erases (%lu copy), stall %lu ms (max %lu)\n",
           (unsigned long)L.batches, (unsigned long)L.blocks, (unsigned long)L.sectors,
           (unsigned long)d.im.erases(), (unsigned long)flash_fs_copy_erases,
           (unsigned long)(L.stall_us / 1000), (unsigned long)(L.stall_max_us / 1000));
    CHECK(mism == 0);
    CHECK(mismatches(d.st, model) == 0);
    CHECK(d.st.stats.drops == 0 && d.st.stats.lost == 0 && d.st.pinnedCount() == 0);
    CHECK(L.blocks > 100 && L.fails == 0);
    CHECK(sim_overprograms == over0);
    // The image file through FatFS: every block not in the pool is the model
    std::set<uint32_t> pooled;
    for (uint32_t b = 0; b < SIZE / 1024; b++) if (d.st.slotOf(b * 1024) != COW_NONE) pooled.insert(b);
    d.reset();
    // Power cycle: the pool is gone, the image holds the written-back blocks
    d.close();
    CHECK(remount());
    LDisk d2;
    CHECK(d2.open("flash:/POP.IMG", SIZE, 16, -1) == 0);
    uint32_t bad = 0, kept = 0;
    for (uint32_t b = 0; b < SIZE / 1024; b++) {
        if (pooled.count(b)) continue;
        kept++;
        for (uint32_t o = 0; o < 1024; o++) bad += d2.st.read(b * 1024 + o) != model[b * 1024 + o];
    }
    printf("  after a power cycle: %lu blocks checked, %lu bytes wrong (pool held %lu blocks)\n",
           (unsigned long)kept, (unsigned long)bad, (unsigned long)pooled.size());
    CHECK(bad == 0);
    for (const auto& o : OTHERS) CHECK(file_ok(o.path, o.n, o.seed));
    d2.close();
}

// ---------------- 3. convergence ----------------
static void boot_like(LDisk& d, std::vector<uint8_t>& model, uint32_t nblk, uint32_t seed, uint32_t first = 0) {
    for (uint32_t b = first; b < first + nblk; b++) write_block(d.st, model, b, seed);
}

static void test_converge() {
    printf("learn: convergence, new version, freeze, relearn\n");
    real_volume();
    const uint32_t SIZE = 256 * 1024;
    std::vector<uint8_t> model(SIZE, 0);
    LDisk d;
    CHECK(d.open("flash:/C.IMG", SIZE, 16, -1) == 0);
    // Session 1: a "boot" writes 100 KB the image lacks
    boot_like(d, model, 100, 21);
    CHECK(d.st.learn.session_blocks >= 84);
    d.reset();
    CHECK(d.im.mode() == COW_LEARN_LEARNING && d.im.trailer().dirty == 1);
    // Sessions 2, 3: the same boot matches the image (and the pool)
    for (int s = 0; s < 2; s++) {
        const uint32_t a0 = d.st.stats.allocs;
        boot_like(d, model, 100, 21);
        CHECK(d.st.learn.session_blocks == 0 && d.st.stats.allocs == a0);
        d.reset();
    }
    CHECK(d.im.mode() == COW_LEARN_OK && d.im.trailer().clean == 2);
    const uint32_t tw = d.im.trailerWrites();
    boot_like(d, model, 100, 21);
    d.reset();
    CHECK(d.im.trailerWrites() == tw);       // a converged clean session writes nothing
    // Power cycle: still ok, nothing to write back
    d.close();
    CHECK(remount());
    CHECK(d.open("flash:/C.IMG", SIZE, 16, -1) == 0);
    CHECK(d.im.mode() == COW_LEARN_OK);
    // The pool's last dirty blocks were never written back: lost with the
    // power, like a RAM disk's content; the boot writes them again
    for (uint32_t i = 0; i < SIZE; i++) model[i] = d.st.read(i);
    boot_like(d, model, 100, 21);
    CHECK(d.st.learn.session_blocks == 0);
    d.reset();
    // A new version: half the data differs - learning restarts by itself
    boot_like(d, model, 50, 22);
    CHECK(d.st.learn.session_blocks > 0);
    d.reset();
    CHECK(d.im.mode() == COW_LEARN_LEARNING);
    for (int s = 0; s < 2; s++) { boot_like(d, model, 50, 22); d.reset(); }
    CHECK(d.im.mode() == COW_LEARN_OK);
    CHECK(mismatches(d.st, model) == 0);

    // Freeze: write-back off, persisted; the pool alone takes the writes
    d.im.freeze(d.st, true);
    d.reset();
    d.close();
    CHECK(remount());
    CHECK(d.open("flash:/C.IMG", SIZE, 16, -1) == 0);
    CHECK(d.im.mode() == COW_LEARN_FROZEN && !d.st.writeBackOn());
    const uint32_t wb0 = d.st.learn.blocks;
    boot_like(d, model, 40, 23, 100);
    CHECK(d.st.learn.blocks == wb0 && d.st.stats.drops > 0);
    // Unfreeze, relearn: a blank image again, learning
    d.im.freeze(d.st, false);
    CHECK(d.im.mode() == COW_LEARN_LEARNING && d.st.writeBackOn());
    d.reset();
    CHECK(d.im.relearn(d.st));
    d.im.persist(d.st);                      // the next RAM-disk access
    std::vector<uint8_t> zeros(SIZE, 0);
    CHECK(mismatches(d.st, zeros) == 0);
    CHECK(d.st.stats.used == 0 && d.im.trailer().relearns == 1 && d.im.mode() == COW_LEARN_LEARNING);
    d.close();
    CHECK(remount());
    CHECK(d.open("flash:/C.IMG", SIZE, 16, -1) == 0);
    CHECK(mismatches(d.st, zeros) == 0 && d.im.trailer().relearns == 1);
    // learn_freeze_after: never converging -> frozen by itself
    d.pol.freeze_after = 2;
    for (uint32_t s = 0; s < 2; s++) { boot_like(d, model, 60, 30 + s); d.reset(); }
    CHECK(d.im.mode() == COW_LEARN_FROZEN && d.im.trailer().frozen_auto == 1);
    d.close();
    for (const auto& o : OTHERS) CHECK(file_ok(o.path, o.n, o.seed));
}

// ---------------- 3b. the reset path does no flash work ----------------
static void test_reset_path() {
    printf("learn: an MZ-800 reset does no flash work\n");
    real_volume();
    const uint32_t SIZE = 512 * 1024;
    std::vector<uint8_t> model(SIZE, 0);
    LDisk d;
    CHECK(d.open("flash:/RS.IMG", SIZE, 64, -1) == 0);
    // A learning session that leaves the pool full of dirty blocks, the
    // map unsynced (fewer than learn_sync blocks since the last sync)
    boot_like(d, model, 300, 70);
    CHECK(d.st.dirtyCount() == 64 && d.st.learn.blocks > 0 && d.st.syncPending());
    const uint32_t ops0 = sim_ops, wb0 = d.st.learn.blocks;
    d.resetOnly();
    CHECK(sim_ops == ops0);                       // no erase, no program at the reset
    CHECK(d.st.learn.blocks == wb0);              // the dirty pool is NOT written back
    CHECK(d.st.dirtyCount() == 64);
    CHECK(d.im.persistPending(d.st));
    // A second reset before any RAM-disk access: still nothing on flash,
    // the work stays pending once
    d.resetOnly(false);
    CHECK(sim_ops == ops0 && d.im.persistPending(d.st));
    // REST relearn/freeze at a reset: RAM only too
    d.im.freeze(d.st, true);
    d.resetOnly();
    CHECK(sim_ops == ops0);
    d.im.freeze(d.st, false);
    // The deferred work, at the next access: bounded
    const uint64_t t0 = sim_flash_us;
    const uint32_t e0 = sim_erases;
    d.im.persist(d.st);
    printf("  deferred persist: %lu flash ops, %lu erases, %.0f ms\n", (unsigned long)(sim_ops - ops0),
           (unsigned long)(sim_erases - e0), (sim_flash_us - t0) / 1e3);
    CHECK(sim_erases - e0 <= 3 && sim_flash_us - t0 < 200000);
    CHECK(!d.im.persistPending(d.st));
    d.close();
    // ... and it reached flash: a power cycle sees the trailer's state
    CHECK(remount());
    CHECK(d.open("flash:/RS.IMG", SIZE, 64, -1) == 0);
    CHECK(d.im.mode() == COW_LEARN_LEARNING && d.im.trailer().wb_blocks == wb0);
    const uint32_t ops1 = sim_ops;
    d.im.relearn(d.st);
    CHECK(sim_ops == ops1);                       // the trims are map edits
    d.im.persist(d.st);
    d.close();
    CHECK(remount());
    CHECK(d.open("flash:/RS.IMG", SIZE, 64, -1) == 0);
    std::vector<uint8_t> zeros(SIZE, 0);
    CHECK(mismatches(d.st, zeros) == 0 && d.im.trailer().relearns == 1);
    d.close();
}

// ---------------- 3c. power-up sessions, the v1 trailer ----------------
static void test_power_sessions() {
    printf("learn: sessions ended by power-off, a v1 (15d9e36) trailer\n");
    real_volume();
    const uint32_t SIZE = 256 * 1024;
    std::vector<uint8_t> model(SIZE, 0);
    LDisk d;
    CHECK(d.open("flash:/PS.IMG", SIZE, 16, -1) == 0);
    d.im.persist(d.st);                       // first access: the session marked open
    boot_like(d, model, 100, 80);             // learns (write-backs, map syncs on the way)
    CHECK(d.st.learn.blocks > 0);
    // Power-off without an MZ-800 reset, three times, the same boot
    int ok_at = 0;
    for (int pw = 1; pw <= 4; pw++) {
        d.close();
        CHECK(remount());
        CHECK(d.open("flash:/PS.IMG", SIZE, 16, -1) == 0);
        if (pw == 1) CHECK(d.im.trailer().dirty == 1 && d.im.trailer().sessions == 1);
        if (!ok_at && d.im.mode() == COW_LEARN_OK) ok_at = pw;
        for (uint32_t i = 0; i < SIZE; i++) model[i] = d.st.read(i);
        d.im.persist(d.st);
        boot_like(d, model, 100, 80);
    }
    printf("  ok after %d power cycles (clean %u, sessions %lu)\n", ok_at, (unsigned)d.im.trailer().clean,
           (unsigned long)d.im.trailer().sessions);
    CHECK(ok_at == 3);   // power-up 1 closes the dirty session, 2 and 3 the clean ones
    d.close();

    // A v1 trailer (as 15d9e36 wrote it) is read, its state kept, upgraded
    CHECK(remount());
    write_file("flash:/V1.IMG", 64 * 1024 + 512, 0);
    FIL f;
    CHECK(f_open(&f, "flash:/V1.IMG", FA_READ | FA_WRITE) == FR_OK);
    uint8_t sec[512] = {0};
    CowTrailer t;
    memset(&t, 0, sizeof t);
    memcpy(t.magic, "MZPCOWL1", 8);
    t.disk_size = 64 * 1024; t.mode = COW_LEARN_OK; t.clean = 2; t.sessions = 7; t.wb_blocks = 123;
    memcpy(sec, &t, sizeof t);
    uint32_t h = 0x5A17C0DEu;
    for (size_t i = 0; i < offsetof(CowTrailer, l1_sum); i++) h = (h << 5) + h + sec[i];
    memcpy(sec + offsetof(CowTrailer, l1_sum), &h, 4);
    UINT w = 0;
    CHECK(f_lseek(&f, 64 * 1024) == FR_OK && f_write(&f, sec, 512, &w) == FR_OK && w == 512);
    CHECK(f_close(&f) == FR_OK);
    CHECK(d.open("flash:/V1.IMG", 0, 16, -1) == 0);
    CHECK(d.im.hasTrailer() && d.im.mode() == COW_LEARN_OK && d.im.trailer().sessions == 7 &&
          d.im.trailer().wb_blocks == 123 && d.st.volatileCount() == 0);
    d.im.freeze(d.st, true);
    d.reset();                                // writes it back as v2
    d.close();
    CHECK(remount());
    CHECK(d.open("flash:/V1.IMG", 0, 16, -1) == 0);
    CHECK(d.im.mode() == COW_LEARN_FROZEN && d.im.trailer().sessions >= 7);
    d.close();
}

// ---------------- 4. wear cap ----------------
static void test_cap() {
    printf("learn: wear cap\n");
    real_volume();
    const uint32_t SIZE = 128 * 1024;
    std::vector<uint8_t> model(SIZE, 0);
    LDisk d;
    CowLearnCfg lc;
    lc.cap = 20;              // 20 blocks an hour
    CHECK(d.open("flash:/W.IMG", SIZE, 8, -1, lc) == 0);
    boot_like(d, model, 60, 40);
    printf("  blocks %lu, drops %lu, cap_drops %lu, capped %d, tokens %lu\n",
           (unsigned long)d.st.learn.blocks, (unsigned long)d.st.stats.drops,
           (unsigned long)d.st.learn.cap_drops, (int)d.st.learn.capped, (unsigned long)d.st.tokens());
    CHECK(d.st.learn.blocks == 20 && d.st.learn.capped && d.st.stats.drops > 0);
    CHECK(d.st.learn.cap_drops == d.st.stats.drops);
    CHECK(mismatches(d.st, model) == 0);     // the model skips dropped writes
    // Half an hour later: 10 more blocks may go
    g_trace_us += 1800ull * 1000000u;
    const uint32_t b0 = d.st.learn.blocks;
    boot_like(d, model, 30, 41, 60);
    CHECK(d.st.learn.blocks - b0 == 10);
    CHECK(mismatches(d.st, model) == 0);
    d.reset();
    CHECK(d.im.mode() == COW_LEARN_LEARNING);
    d.close();
}

// ---------------- 5. remap during write-back ----------------
static void test_remap() {
    printf("learn: external rewrite and delete during write-back\n");
    real_volume();
    const uint32_t SIZE = 256 * 1024;
    std::vector<uint8_t> model(SIZE, 0);
    LDisk d;
    CHECK(d.open("flash:/R.IMG", SIZE, 32, -1) == 0);
    boot_like(d, model, 60, 50);                    // write-back running
    CHECK(d.st.learn.blocks > 0 && d.st.pinnedCount() == 0 && d.st.stats.preserved == 0);
    // Someone rewrites 10 KB of the image file in place (FatFS, as an
    // upload would), over blocks already learned (written back or still
    // pooled): the disk keeps the power-up snapshot of those blocks. (A
    // write into a hole moves no page: the table keeps the zero page.)
    // Preserving needs free slots (a flash_fs hook cannot write back): with
    // a pool full of dirty blocks the change would only be counted as lost
    d.st.writeBack(32, false);
    FIL f;
    CHECK(f_open(&f, "flash:/R.IMG", FA_READ | FA_WRITE) == FR_OK);
    CHECK(f_lseek(&f, 10 * 1024) == FR_OK);
    std::vector<uint8_t> nb(10 * 1024);
    for (uint32_t i = 0; i < nb.size(); i++) nb[i] = pat(i, 51);
    UINT w = 0;
    CHECK(f_write(&f, nb.data(), (UINT)nb.size(), &w) == FR_OK && w == nb.size());
    CHECK(f_close(&f) == FR_OK);
    const uint32_t pinned = d.st.pinnedCount();
    printf("  external rewrite: pinned %lu, preserved %lu, lost %lu\n", (unsigned long)pinned,
           (unsigned long)d.st.stats.preserved, (unsigned long)d.st.stats.lost);
    CHECK(pinned == 10 && d.st.stats.lost == 0);
    CHECK(mismatches(d.st, model) == 0);
    // More learning around the pinned blocks: they are never written back
    boot_like(d, model, 60, 52, 150);
    CHECK(d.st.pinnedCount() == pinned && d.st.stats.lost == 0);
    CHECK(mismatches(d.st, model) == 0);
    // The file deleted under the running disk: the write-back stops for good
    CHECK(f_unlink("flash:/R.IMG") == FR_OK);
    CHECK(d.im.gone() && !d.st.writeBackOn());
    printf("  deleted: gone, preserved %lu, lost %lu\n", (unsigned long)d.st.stats.preserved,
           (unsigned long)d.st.stats.lost);
    d.close();
    for (const auto& o : OTHERS) CHECK(file_ok(o.path, o.n, o.seed));
}

// ---------------- 6. power cuts ----------------
struct Snap { std::vector<uint8_t> region; uint32_t op; int kind; };
static std::vector<Snap> g_snaps;
static uint32_t g_snap_every = 0, g_snap_phase = 0;

static void snap_hook(int kind, uint32_t off, size_t n) {
    if (!g_snap_every || (sim_ops % g_snap_every) != g_snap_phase) return;
    Snap s;
    s.region.assign(CHIP + REGION, CHIP + 2 * REGION);
    s.op = sim_ops;
    s.kind = kind;
    // Every other cut lands INSIDE the operation: half of it done
    if ((sim_ops / g_snap_every) & 1) {
        uint8_t* p = s.region.data() + (off - REGION);
        if (kind == 0) memset(p, 0xFF, n / 2);
        else for (size_t i = 0; i < n / 2; i++) p[i] &= 0x00;   // torn: worst case bits
        s.kind += 2;
    }
    g_snaps.push_back(std::move(s));
}

static uint64_t hash512(const uint8_t* p) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 512; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void test_powercut() {
    printf("learn: power cuts during write-back\n");
    real_volume();
    const uint32_t SIZE = 512 * 1024, NS = SIZE / 512;
    std::vector<uint8_t> model(SIZE, 0);
    // Every version each image sector ever had on the Z80 side (at block
    // boundaries: the write-back only takes whole, finished blocks)
    std::vector<std::set<uint64_t>> hist(NS);
    std::vector<uint8_t> zero(512, 0);
    for (auto& h : hist) h.insert(hash512(zero.data()));
    {
        LDisk d;
        CHECK(d.open("flash:/P.IMG", SIZE, 16, -1) == 0);
        g_snaps.clear();
        g_snap_every = 2;
        g_snap_phase = 1;
        sim_op_hook = snap_hook;
        std::mt19937 rng(99);
        for (uint32_t k = 0; k < 160; k++) {
            const uint32_t b = rng() % (SIZE / 1024);
            write_block(d.st, model, b, 60 + rng() % 3);
            hist[2 * b].insert(hash512(&model[b * 1024]));
            hist[2 * b + 1].insert(hash512(&model[b * 1024 + 512]));
            if (k == 80) d.reset();      // a session end: trailer + map sync on the way
        }
        d.reset();
        sim_op_hook = nullptr;
        g_snap_every = 0;
        printf("  %lu flash operations, %lu cuts, %lu blocks written back\n", (unsigned long)sim_ops,
               (unsigned long)g_snaps.size(), (unsigned long)d.st.learn.blocks);
        d.close();
    }
    uint32_t mounted = 0, others_bad = 0, img_bad = 0, sect_bad = 0, trailer_ok = 0;
    for (const Snap& s : g_snaps) {
        memcpy(CHIP + REGION, s.region.data(), REGION);
        if (!remount()) continue;
        mounted++;
        for (const auto& o : OTHERS) if (!file_ok(o.path, o.n, o.seed)) others_bad++;
        LDisk d;
        if (d.open("flash:/P.IMG", SIZE, 16, -1) != 0) { img_bad++; printf("  cut %lu: %s\n", (unsigned long)s.op, d.err); continue; }
        trailer_ok += d.im.hasTrailer();
        for (uint32_t i = 0; i < NS; i++) {
            const uint8_t* p = CHIP + ((uint32_t)d.st.sectorEntry(i) << 9);
            if (!hist[i].count(hash512(p))) sect_bad++;
        }
        d.close();
    }
    printf("  %lu of %lu cuts mount; other files damaged %lu, image unusable %lu, image sectors "
           "never written %lu, trailer valid %lu\n", (unsigned long)mounted, (unsigned long)g_snaps.size(),
           (unsigned long)others_bad, (unsigned long)img_bad, (unsigned long)sect_bad, (unsigned long)trailer_ok);
    CHECK(mounted == g_snaps.size());
    CHECK(others_bad == 0 && img_bad == 0 && sect_bad == 0);
    g_snaps.clear();
    g_snaps.shrink_to_fit();
}

// ---------------- 7. trace replay ----------------
struct Rec { char k; uint8_t page; uint16_t start; uint32_t len; uint64_t cycle; uint32_t data; };

static bool load_trace(const char* path, std::vector<Rec>& recs, std::vector<uint8_t>& data) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    size_t i = 0;
    while (i + 16 <= b.size()) {
        Rec r;
        r.k = (char)b[i]; r.page = b[i + 1];
        memcpy(&r.start, &b[i + 2], 2); memcpy(&r.len, &b[i + 4], 4); memcpy(&r.cycle, &b[i + 8], 8);
        i += 16;
        r.data = 0;
        if (r.k == 'W') {
            if (i + r.len > b.size()) return false;
            r.data = (uint32_t)data.size();
            data.insert(data.end(), b.begin() + i, b.begin() + i + r.len);
            i += r.len;
        }
        recs.push_back(r);
    }
    return i == b.size();
}

struct PhaseStat {
    uint32_t batches = 0, blocks = 0, ahead = 0, sectors = 0, drops = 0, peak = 0, erases = 0;
    uint64_t stall_us = 0;
    uint32_t stall_max = 0;
};

// One session of the trace: the boot, the title/story phase played for
// `title` of its length (> 1: repeated - PRES runs longer, its scratch
// grows and differs), then the play phase
struct ReplayCtx {
    const std::vector<Rec>* recs;
    const std::vector<uint8_t>* data;
    uint64_t boot_end, play_start;
    uint64_t base_us = 0;
    uint32_t read_bad = 0;
};

// salt != 0: every written byte XOR salt - the same writes with other
// content (scratch that differs from session to session)
static void replay_range(ReplayCtx& c, LDisk& d, std::vector<uint8_t>& model, size_t from, size_t to,
                         uint64_t c0, uint32_t& peak, uint8_t salt = 0) {
    const uint32_t SIZE = (uint32_t)model.size();
    for (size_t k = from; k < to; k++) {
        const Rec& r = (*c.recs)[k];
        g_trace_us = c.base_us + (r.cycle - c0) * 1000000ull / 3546875ull;
        if (r.k == 'F') continue;
        const uint32_t pagebase = ((uint32_t)r.page << 16) % SIZE;
        for (uint32_t i = 0; i < r.len; i++) {
            const uint32_t p = pagebase | ((r.start + i) & 0xFFFF);
            if (r.k == 'R') {
                c.read_bad += d.st.read(p) != model[p];
            } else {
                const uint8_t v = (uint8_t)((*c.data)[r.data + i] ^ salt);
                const uint32_t dr = d.st.stats.drops;
                d.st.write(p, v);
                if (d.st.stats.drops == dr) model[p] = v;
            }
        }
        if (d.st.stats.used > peak) peak = d.st.stats.used;
    }
    if (!c.recs->empty() && to > from) c.base_us = g_trace_us;
}

struct SessStat { uint32_t wb_boot, wb_title, wb_play, vol, drops, peak, volatiles; uint64_t stall_us; };

static SessStat replay_session(ReplayCtx& c, LDisk& d, std::vector<uint8_t>& model, double title, int sess) {
    const auto& R = *c.recs;
    size_t b_end = 0, t_end = 0;
    while (b_end < R.size() && R[b_end].cycle < c.boot_end) b_end++;
    t_end = b_end;
    while (t_end < R.size() && R[t_end].cycle < c.play_start) t_end++;
    SessStat st{};
    d.im.persist(d.st);                 // the session's first RAM-disk access
    const uint32_t b0 = d.st.learn.blocks, v0 = d.st.learn.vol_forced, dr0 = d.st.stats.drops;
    const uint64_t s0 = d.st.learn.stall_us;
    replay_range(c, d, model, 0, b_end, R.front().cycle, st.peak);
    st.wb_boot = d.st.learn.blocks - b0;
    // title/story: whole passes, then a part
    double left = title;
    const uint64_t tlen = c.play_start - c.boot_end;
    int pass = 0;
    while (left > 1e-9) {
        const double f = left >= 1.0 ? 1.0 : left;
        size_t e = b_end;
        const uint64_t lim = c.boot_end + (uint64_t)(f * (double)tlen);
        while (e < t_end && R[e].cycle < lim) e++;
        // Session 1's first pass is the trace as recorded; every other pass
        // writes different scratch content
        const uint8_t salt = (sess == 1 && pass == 0) ? 0 : (uint8_t)(0x11 * (sess + 3 * pass) | 1);
        replay_range(c, d, model, b_end, e, c.boot_end, st.peak, salt);
        left -= f;
        pass++;
    }
    st.wb_title = d.st.learn.blocks - b0 - st.wb_boot;
    replay_range(c, d, model, t_end, R.size(), c.play_start, st.peak);
    st.wb_play = d.st.learn.blocks - b0 - st.wb_boot - st.wb_title;
    st.vol = d.st.learn.vol_forced - v0;
    st.drops = d.st.stats.drops - dr0;
    st.stall_us = d.st.learn.stall_us - s0;
    c.base_us += 10000000ull;             // 10 s to the next session
    return st;
}

// The volatile blocks as page:offset ranges
static void print_volatile(const CowStore& st, uint32_t nblk) {
    printf("    volatile blocks (%lu):", (unsigned long)st.volatileCount());
    for (uint32_t b = 0; b < nblk; ) {
        if (!st.isVolatile(b)) { b++; continue; }
        uint32_t e = b;
        while (e + 1 < nblk && st.isVolatile(e + 1)) e++;
        printf(" %u:%04X-%04X", b >> 6, (b & 63) << 10, ((e & 63) << 10) | 0x3FF);
        b = e + 1;
    }
    printf("\n");
}

static void replay_sessions(const std::vector<Rec>& recs, const std::vector<uint8_t>& data,
                            uint64_t boot_end, uint64_t play_start, uint32_t pool, bool verbose) {
    real_volume();
    const uint32_t SIZE = 512 * 1024;
    std::vector<uint8_t> model(SIZE, 0);
    ReplayCtx c{&recs, &data, boot_end, play_start};
    LDisk d;
    CHECK(d.open("flash:/POP800RD.IMG", SIZE, pool, -1) == 0);
    // power: the session starts with a card power-up (the pool is gone;
    // the previous session is closed from the trailer); else an MZ-800 reset
    struct Plan { bool power; double title; } plan[] = {
        {true, 1.0}, {false, 0.5}, {true, 1.5}, {false, 0.25}, {true, 2.0}, {true, 1.0},
        {false, 0.75}, {true, 3.0}, {true, 0.1}, {false, 1.0},
    };
    printf("  pool %lu KB:\n", (unsigned long)pool);
    uint32_t late_wb = 0, all_drops = 0;
    int n = 0;
    for (const Plan& pl : plan) {
        n++;
        if (n > 1) {
            if (pl.power) {
                d.close();
                CHECK(remount());
                CHECK(d.open("flash:/POP800RD.IMG", SIZE, pool, -1) == 0);
                for (uint32_t i = 0; i < SIZE; i++) model[i] = d.st.read(i);   // the pool is gone
            } else {
                d.resetOnly();
            }
        }
        const SessStat st = replay_session(c, d, model, pl.title, n);
        if (verbose || st.wb_boot + st.wb_title + st.wb_play || st.drops)
            printf("    session %2d (%s, title x%.2f): write-backs boot %3lu title %3lu play %3lu (volatile %lu), "
                   "stall %5.2f s, drops %lu, pool peak %lu, learn %s, volatile %lu\n", n,
                   pl.power ? "power-up" : "reset   ", pl.title, (unsigned long)st.wb_boot,
                   (unsigned long)st.wb_title, (unsigned long)st.wb_play, (unsigned long)st.vol,
                   st.stall_us / 1e6, (unsigned long)st.drops, (unsigned long)st.peak,
                   cow_learn_mode_name(d.im.mode()), (unsigned long)d.st.volatileCount());
        if (n >= 4) late_wb += st.wb_boot + st.wb_title + st.wb_play - st.vol;
        all_drops += st.drops;
        CHECK(d.st.pinnedCount() == 0 && d.st.stats.lost == 0);
    }
    d.resetOnly();
    d.im.persist(d.st);
    print_volatile(d.st, SIZE / 1024);
    printf("    end: learn %s (clean %u), sessions %lu, non-volatile write-backs from session 4 on: %lu, "
           "drops %lu, reads wrong %lu\n", cow_learn_mode_name(d.im.mode()), (unsigned)d.im.trailer().clean,
           (unsigned long)d.im.trailer().sessions, (unsigned long)late_wb, (unsigned long)all_drops,
           (unsigned long)c.read_bad);
    // Even a pool far too small for the scratch converges: the scratch
    // turns volatile, its write-backs stop counting, nothing is dropped
    CHECK(late_wb == 0);
    CHECK(all_drops == 0);
    CHECK(d.im.mode() == COW_LEARN_OK);
    CHECK(c.read_bad == 0);
    CHECK(mismatches(d.st, model) == 0);
    d.close();
}

static void test_trace(const char* path) {
    std::vector<Rec> recs;
    std::vector<uint8_t> data;
    if (!path || !load_trace(path, recs, data) || recs.empty()) {
        printf("learn: trace replay skipped (pass a .rdlog: argv[1] or COW_TRACE)\n");
        return;
    }
    const char* be = getenv("COW_TRACE_BOOT_END");
    const char* ps = getenv("COW_TRACE_PLAY");
    const uint64_t boot_end = be ? strtoull(be, nullptr, 0) : 62669615ull;    // full_512K: PRES start (marker - 2^32: the rdlog clock is 32-bit)
    const uint64_t play = ps ? strtoull(ps, nullptr, 0) : 405643179ull;        // level 1 play
    uint64_t w = 0;
    for (const Rec& r : recs) if (r.k == 'W') w += r.len;
    printf("learn: trace replay %s: %lu records, %lu bytes written\n", path,
           (unsigned long)recs.size(), (unsigned long)w);
    const uint32_t pool = getenv("COW_POOL") ? (uint32_t)atoi(getenv("COW_POOL")) : 72;
    replay_sessions(recs, data, boot_end, play, pool, true);
    replay_sessions(recs, data, boot_end, play, 40, true);     // scratch > pool: the hot-block rule at work
    if (getenv("COW_TRACE_VARIANTS"))
        for (uint32_t pv : {48u, 64u, 96u}) replay_sessions(recs, data, boot_end, play, pv, false);
}

void learn_tests(int argc, char** argv) {
    const uint64_t t0 = sim_flash_us;
    test_create();
    test_writeback();
    test_converge();
    test_reset_path();
    test_power_sessions();
    test_cap();
    test_remap();
    test_powercut();
    test_trace(argc > 1 ? argv[1] : getenv("COW_TRACE"));
    (void)t0;
    g_real_flash = false;
}
