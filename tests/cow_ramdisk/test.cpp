// Host tests of the COW RAM disk ([ramdisk] mode=cow).
//
// The real FatFS (external/fatfs-sdk ff15, the firmware's src/ffconf.h)
// runs on an emulation of flash_fs: 512-byte FAT sectors remapped onto
// "flash" pages by a log-structured allocator that never writes in place,
// hands out pages in a scrambled order (so the physical layout is nothing
// like the logical one) and POISONS a page (0xEE) the moment it is
// replaced - harsher than the real one, which recycles it only after the
// next map sync - so a table entry left pointing at a dead page shows up at
// once. The remap hook fires like flash_fs_write_FAT_sector's.
//
// Checked:
//  1. cow_resolve_file: fragmented files on 1- and 4-sector clusters
//     resolve to pages holding exactly the file's bytes; holes (f_expand,
//     never written) and short files are refused.
//  2. CowStore against a reference model: read-through, write-compare (the
//     boot's identical copy allocates nothing), the size probe's
//     write-then-restore blocks reclaimed when the pool fills, exhaustion
//     drops counted (and the ONLY way reads ever diverge from a perfect
//     RAM), random stress with and without exhaustion.
//  3. Changing the image file while the disk runs (rewritten in place;
//     deleted and replaced by another file reusing its clusters): the disk
//     keeps its power-up content, the touched blocks preserved (pinned) in
//     the pool, the table following the moved pages (never a poisoned
//     page); with too small a pool the loss is counted, never silent;
//     clear() keeps the pinned blocks and ends the pool-full state.
//  4. The flash-busy flag: an access waits for the flag to clear.
//  5. Positions: the MR1R18 counter wraps inside its 64 KB page (a 64 KB
//     disk at 0xFFFF, two reads), and a stray position past the end is
//     wrapped, never indexes past the tables (run.sh builds with ASan).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>

#include "ff.h"
#include "diskio.h"
#include "cow_store.hpp"
#include "cow_resolve.hpp"
#include "flash_fs.h"
#include "ramdisk_pos.hpp"

int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---------------- emulated flash_fs ----------------
constexpr uint32_t RESERVED_PAGES = 16;     // like the map sectors: entry 0 is never valid
constexpr uint32_t NUM_PAGES = 4096;        // 2 MB of "flash"
constexpr uint32_t NUM_LBA = 3000;          // FAT volume sectors (< pages: room to relocate)
static std::vector<uint8_t> g_flash(NUM_PAGES * 512, 0xFF);
static std::vector<uint16_t> g_map(NUM_LBA, 0);
static std::vector<uint16_t> g_free;
static std::mt19937 g_rng(12345);
static void (*g_remap_hook)(uint16_t, uint16_t) = nullptr;
static uint32_t g_writes = 0;

static void flash_reset() {
    std::fill(g_flash.begin(), g_flash.end(), 0xFF);
    std::fill(g_map.begin(), g_map.end(), 0);
    g_free.clear();
    for (uint32_t p = RESERVED_PAGES; p < NUM_PAGES; p++) g_free.push_back((uint16_t)p);
    std::shuffle(g_free.begin(), g_free.end(), g_rng);
    g_remap_hook = nullptr;
}

static uint16_t map_entry(uint32_t lba) { return lba < NUM_LBA ? g_map[lba] : 0; }

// The learn tests (learn.cpp) run FatFS on the REAL src/flash_fs.c over an
// emulated NOR chip (flash_sim.cpp) instead: g_real_flash switches the disk
bool g_real_flash = false;
bool g_real_no_trim = false;   // real mode: a delete as firmware before TRIM did it
// Emulated TRIM (FatFS CTRL_TRIM, as the firmware does since learn): off
// = the pages stay, as with the firmware before; on = the hook is told,
// then the pages are poisoned and recycled
static bool g_emu_trim = false;
static void (*g_trim_hook)(uint32_t lba, uint16_t old_page) = nullptr;

extern "C" {
DSTATUS disk_status(BYTE) { return 0; }
DSTATUS disk_initialize(BYTE) { return 0; }
DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (g_real_flash) {
        for (UINT i = 0; i < count; i++) {
            if (sector + i >= flash_fs_num_fat_sectors()) return RES_PARERR;
            flash_fs_read_FAT_sector((uint16_t)(sector + i), buff + i * 512);
        }
        return RES_OK;
    }
    for (UINT i = 0; i < count; i++) {
        LBA_t s = sector + i;
        if (s >= NUM_LBA) return RES_PARERR;
        if (g_map[s]) memcpy(buff + i * 512, &g_flash[g_map[s] * 512u], 512);
        else memset(buff + i * 512, 0, 512);
    }
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (g_real_flash) {   // as src/fatfs_disk.c
        for (UINT i = 0; i < count; i++) {
            if (sector + i >= flash_fs_num_fat_sectors()) return RES_PARERR;
            if (!flash_fs_write_FAT_sector((uint16_t)(sector + i), buff + i * 512)) return RES_ERROR;
            if (!flash_fs_verify_FAT_sector((uint16_t)(sector + i), buff + i * 512)) return RES_ERROR;
        }
        return RES_OK;
    }
    for (UINT i = 0; i < count; i++) {
        LBA_t s = sector + i;
        if (s >= NUM_LBA || g_free.empty()) return RES_ERROR;
        uint16_t old = g_map[s];
        // pseudo-random free page: scrambled physical layout
        size_t k = g_rng() % g_free.size();
        uint16_t neu = g_free[k];
        g_free[k] = g_free.back(); g_free.pop_back();
        memcpy(&g_flash[neu * 512u], buff + i * 512, 512);
        g_map[s] = neu;
        g_writes++;
        if (old) {
            if (g_remap_hook) g_remap_hook(old, neu);
            memset(&g_flash[old * 512u], 0xEE, 512);   // recycled at once: poison
            g_free.push_back(old);
        }
    }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (g_real_flash) {   // as src/fatfs_glue.c
        switch (cmd) {
        case CTRL_SYNC: flash_fs_sync(); return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t*)buff = flash_fs_num_fat_sectors(); return RES_OK;
        case GET_SECTOR_SIZE: *(WORD*)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
        case CTRL_TRIM: {
            const LBA_t* r = (const LBA_t*)buff;
            if (r[1] >= r[0] && !g_real_no_trim) flash_fs_trim_FAT_sectors((uint32_t)r[0], (uint32_t)(r[1] - r[0] + 1));
            return RES_OK;
        }
        default: return RES_PARERR;
        }
    }
    if (cmd == CTRL_TRIM && g_emu_trim) {
        const LBA_t* r = (const LBA_t*)buff;
        for (LBA_t l = r[0]; l <= r[1] && l < NUM_LBA; l++) {
            const uint16_t old = g_map[l];
            if (!old) continue;
            if (g_trim_hook) g_trim_hook((uint32_t)l, old);
            g_map[l] = 0;
            memset(&g_flash[old * 512u], 0xEE, 512);
            g_free.push_back(old);
        }
        return RES_OK;
    }
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t*)buff = NUM_LBA; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD*)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (11 << 21) | (16 << 16); }
}

static FATFS g_fs;

static void make_volume(UINT au_sectors) {
    flash_reset();
    static BYTE work[FF_MAX_SS * 4];
    MKFS_PARM opt = {FM_FAT, 1, 0, 0, au_sectors * 512};
    FRESULT fr = f_mkfs("flash:", &opt, work, sizeof work);
    CHECK(fr == FR_OK);
    fr = f_mount(&g_fs, "flash:", 1);
    CHECK(fr == FR_OK);
}

static uint8_t pattern(uint32_t pos, uint32_t seed) {
    uint32_t x = pos * 2654435761u ^ seed;
    return (uint8_t)(x >> 13 ^ x >> 24);
}

static void append(FIL* f, uint32_t& pos, uint32_t n, uint32_t seed) {
    std::vector<uint8_t> b(n);
    for (uint32_t i = 0; i < n; i++) b[i] = pattern(pos + i, seed);
    UINT w = 0;
    CHECK(f_write(f, b.data(), n, &w) == FR_OK && w == n);
    pos += n;
}

static uint32_t fragments(FIL* f) {
    DWORD tbl[512];
    tbl[0] = 512;
    f->cltbl = tbl;
    FRESULT fr = f_lseek(f, CREATE_LINKMAP);
    f->cltbl = nullptr;
    CHECK(fr == FR_OK);
    return (tbl[0] - 2) / 2;   // used items: 2 + 2 per fragment... minus the terminator pair
}

// Builds flash:/IMG.BIN (img_size bytes, pattern seed 1) fragmented by an
// interleaved second file and a deleted first one
static void build_fragmented_image(uint32_t img_size) {
    FIL a, b, img;
    uint32_t pa = 0, pb = 0, pi = 0;
    CHECK(f_open(&a, "flash:/A.BIN", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    append(&a, pa, 7000, 2);
    f_close(&a);
    CHECK(f_open(&b, "flash:/B.BIN", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    CHECK(f_open(&img, "flash:/IMG.BIN", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    while (pi < img_size / 2) {
        append(&img, pi, 3072, 1); f_sync(&img);
        append(&b, pb, 1024, 3); f_sync(&b);
    }
    f_close(&b);
    CHECK(f_unlink("flash:/A.BIN") == FR_OK);   // a hole the image grows into
    while (pi < img_size) { append(&img, pi, 4096, 1); f_sync(&img); }
    f_close(&img);
}

static void test_resolve(UINT au) {
    printf("resolve, %u-sector clusters\n", au);
    make_volume(au);
    const uint32_t SIZE = 256 * 1024, NS = SIZE / 512;
    build_fragmented_image(SIZE);

    FIL f;
    CHECK(f_open(&f, "flash:/IMG.BIN", FA_READ) == FR_OK);
    uint32_t frags = fragments(&f);
    printf("  image: %lu fragments\n", (unsigned long)frags);
    CHECK(frags >= 3);
    std::vector<uint16_t> tbl(NS);
    char err[96] = "";
    int r = cow_resolve_file(&f, NS, map_entry, NUM_PAGES, 0, 0, tbl.data(), nullptr, err, sizeof err);
    if (r) printf("  error: %s\n", err);
    CHECK(r == 0);
    uint32_t bad = 0, jumps = 0;
    for (uint32_t i = 0; i < NS; i++) {
        const uint8_t* p = &g_flash[tbl[i] * 512u];
        for (uint32_t o = 0; o < 512; o++)
            if (p[o] != pattern(i * 512 + o, 1)) { bad++; break; }
        if (i && tbl[i] != tbl[i - 1] + 1) jumps++;
    }
    printf("  %lu sectors, %lu non-consecutive pages, %lu wrong\n",
           (unsigned long)NS, (unsigned long)jumps, (unsigned long)bad);
    CHECK(bad == 0);
    CHECK(jumps > NS / 2);   // the scrambled allocator: the test is not trivially linear

    // A prefix is fine; longer than the file is refused
    CHECK(cow_resolve_file(&f, NS / 2, map_entry, NUM_PAGES, 0, 0, tbl.data(), nullptr, err, sizeof err) == 0);
    const uint32_t past = (uint32_t)((f_size(&f) + 511) / 512) + 1;
    tbl.resize(past);
    CHECK(cow_resolve_file(&f, past, map_entry, NUM_PAGES, 0, 0, tbl.data(), nullptr, err, sizeof err) != 0);
    printf("  longer than the file: %s\n", err);
    // Entries outside the region are refused
    CHECK(cow_resolve_file(&f, NS, map_entry, RESERVED_PAGES + 1, 0, 0, tbl.data(), nullptr, err, sizeof err) != 0);
    printf("  outside the region: %s\n", err);
    f_close(&f);

    // A hole: clusters allocated (f_expand) but never written
    FIL h;
    CHECK(f_open(&h, "flash:/HOLE.BIN", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    CHECK(f_expand(&h, 64 * 1024, 1) == FR_OK);
    r = cow_resolve_file(&h, 128, map_entry, NUM_PAGES, 0, 0, tbl.data(), nullptr, err, sizeof err);
    CHECK(r != 0);
    printf("  hole: %s\n", err);
    // ... unless a hole entry is given (learn's blank image): every
    // sector then names it, and the run list covers the whole file
    CowRuns runs;
    r = cow_resolve_file(&h, 128, map_entry, NUM_PAGES, 100, 7, tbl.data(), &runs, err, sizeof err);
    CHECK(r == 0);
    bool all7 = true;
    for (uint32_t i = 0; i < 128; i++) all7 = all7 && tbl[i] == 7;
    CHECK(all7);
    CHECK(runs.sectors == 128 && runs.n == 1);
    CHECK(runs.index(runs.lba(77)) == 77);
    runs.release();
    f_close(&h);
    f_mount(nullptr, "flash:", 0);
}

// ---------------- CowStore vs a model ----------------
static std::atomic<uint32_t> g_busy{0};   // CowBusy is std::atomic on the host
static CowStore* g_cow = nullptr;
static void hook(uint16_t o, uint16_t n) { if (g_cow) g_cow->remap(o, n); }

// A CowStore over the resolved IMG.BIN (fresh volume), and the image bytes
static bool open_disk(CowStore& cow, uint32_t size, uint32_t pool, std::vector<uint8_t>& image) {
    make_volume(1);
    build_fragmented_image(size);
    FIL f;
    CHECK(f_open(&f, "flash:/IMG.BIN", FA_READ) == FR_OK);
    if (!cow.setup(size, pool, g_flash.data(), &g_busy)) return false;
    char err[96];
    int r = cow_resolve_file(&f, cow.sectorCount(), map_entry, NUM_PAGES, 0, 0, cow.sectorTable(), nullptr, err, sizeof err);
    f_close(&f);
    CHECK(r == 0);
    image.resize(size);
    for (uint32_t i = 0; i < size; i++) image[i] = pattern(i, 1);
    return r == 0;
}

static uint32_t compare_all(CowStore& cow, const std::vector<uint8_t>& model) {
    uint32_t bad = 0;
    for (uint32_t i = 0; i < model.size(); i++)
        if (cow.read(i) != model[i]) bad++;
    return bad;
}

static void test_semantics() {
    printf("COW semantics\n");
    const uint32_t SIZE = 256 * 1024;
    CowStore cow;
    std::vector<uint8_t> model;
    CHECK(open_disk(cow, SIZE, 8, model));
    CHECK(compare_all(cow, model) == 0);

    // The boot's copy of the whole image from floppy: identical bytes
    for (uint32_t i = 0; i < SIZE; i++) cow.write(i, model[i]);
    CHECK(cow.stats.allocs == 0 && cow.stats.used == 0);

    // The size probe: a marker per page, read back, the old byte restored
    for (uint32_t pg = 0; pg < SIZE / 65536; pg++) {
        uint32_t a = pg * 65536 + 0x1234;
        uint8_t old = cow.read(a);
        cow.write(a, (uint8_t)~old);
        CHECK(cow.read(a) == (uint8_t)~old);
        cow.write(a, old);
    }
    CHECK(cow.stats.allocs == 4 && cow.stats.used == 4);
    for (uint32_t s = 0; s < 4; s++) CHECK(cow.diffOf((uint8_t)s) == 0);
    CHECK(compare_all(cow, model) == 0);

    // 8 real changes in 8 other blocks: 4 free slots, then the 4 probe
    // blocks (equal to flash again) are reclaimed
    for (uint32_t k = 0; k < 8; k++) {
        uint32_t a = 0x2000 + k * 4096 + 7;
        model[a] ^= 0x5A;
        cow.write(a, model[a]);
    }
    CHECK(cow.stats.reclaims == 4);
    CHECK(cow.stats.drops == 0);
    CHECK(compare_all(cow, model) == 0);

    // A 9th changed block: nothing reclaimable - dropped, counted, remembered
    uint32_t a = 0x30000 + 99;
    cow.write(a, (uint8_t)(model[a] ^ 1));
    CHECK(cow.stats.drops == 1 && cow.stats.last_drop == a);
    CHECK(compare_all(cow, model) == 0);   // the lost write is the only difference
    // Writes inside pooled blocks still work while full
    model[0x2000 + 8] ^= 0xFF;
    cow.write(0x2000 + 8, model[0x2000 + 8]);
    CHECK(compare_all(cow, model) == 0);
    // Restoring a pooled block's byte makes it reclaimable again
    uint32_t r0 = 0x2000 + 7;
    model[r0] = pattern(r0, 1); cow.write(r0, model[r0]);
    model[0x2000 + 8] = pattern(0x2000 + 8, 1); cow.write(0x2000 + 8, model[0x2000 + 8]);
    cow.write(a, (uint8_t)(model[a] ^ 1)); model[a] ^= 1;
    CHECK(cow.stats.drops == 1 && cow.stats.reclaims == 5);
    CHECK(compare_all(cow, model) == 0);

    // clear(): back to the image
    cow.clear();
    std::vector<uint8_t> img(SIZE);
    for (uint32_t i = 0; i < SIZE; i++) img[i] = pattern(i, 1);
    CHECK(compare_all(cow, img) == 0);
    CHECK(cow.stats.used == 0);
}

static void test_stress(uint32_t pool, uint32_t blocks_touched, bool expect_drops) {
    printf("stress: pool %lu, %lu blocks touched\n", (unsigned long)pool, (unsigned long)blocks_touched);
    const uint32_t SIZE = 512 * 1024;
    CowStore cow;
    std::vector<uint8_t> model;
    CHECK(open_disk(cow, SIZE, pool, model));
    const std::vector<uint8_t> image = model;
    std::mt19937 rng(777 + pool);
    std::vector<uint32_t> blks(blocks_touched);
    for (auto& b : blks) b = rng() % (SIZE / 1024);
    uint32_t mism = 0;
    for (uint32_t op = 0; op < 400000; op++) {
        uint32_t blk = blks[rng() % blocks_touched];
        uint32_t pos = blk * 1024 + (rng() % 1024);
        uint32_t kind = rng() % 4;
        if (kind == 0) {
            if (cow.read(pos) != model[pos]) mism++;
        } else {
            // 1 in 3 writes restores the image byte (scratch areas come and go)
            uint8_t v = (kind == 1) ? image[pos] : (uint8_t)rng();
            uint32_t d0 = cow.stats.drops;
            cow.write(pos, v);
            if (cow.stats.drops == d0) model[pos] = v;   // a drop is the one counted loss
        }
    }
    CHECK(mism == 0);
    CHECK(compare_all(cow, model) == 0);
    // The difference counts are exact
    uint32_t badcount = 0;
    for (uint32_t blk = 0; blk < SIZE / 1024; blk++) {
        uint8_t s = cow.slotOf(blk * 1024);
        if (s == COW_NONE) continue;
        uint32_t d = 0;
        for (uint32_t o = 0; o < 1024; o++) d += model[blk * 1024 + o] != image[blk * 1024 + o];
        if (d != cow.diffOf(s)) badcount++;
    }
    CHECK(badcount == 0);
    printf("  allocs %lu reclaims %lu drops %lu peak %lu\n", (unsigned long)cow.stats.allocs,
           (unsigned long)cow.stats.reclaims, (unsigned long)cow.stats.drops, (unsigned long)cow.stats.peak);
    if (expect_drops) CHECK(cow.stats.drops > 0); else CHECK(cow.stats.drops == 0);
}

static std::vector<uint8_t> file_bytes(uint32_t off, uint32_t n, uint32_t seed) {
    std::vector<uint8_t> b(n);
    for (uint32_t i = 0; i < n; i++) b[i] = pattern(off + i, seed);
    return b;
}

static void test_rewrite() {
    printf("image rewritten in place while running\n");
    const uint32_t SIZE = 256 * 1024;
    CowStore cow;
    std::vector<uint8_t> model;
    CHECK(open_disk(cow, SIZE, 16, model));
    g_cow = &cow;
    g_remap_hook = hook;
    // Two blocks pooled by the Z80 inside the region about to be rewritten
    const uint32_t P1 = 20 * 1024 + 5, P2 = 23 * 1024 + 9;
    model[P1] ^= 0x11; cow.write(P1, model[P1]);
    model[P2] ^= 0x22; cow.write(P2, model[P2]);
    // and one elsewhere
    const uint32_t P3 = 100 * 1024 + 1;
    model[P3] ^= 0x33; cow.write(P3, model[P3]);

    // In-place rewrite of bytes 20000..29999 (blocks 19..29) of the file
    FIL f;
    CHECK(f_open(&f, "flash:/IMG.BIN", FA_READ | FA_WRITE) == FR_OK);
    CHECK(f_lseek(&f, 20000) == FR_OK);
    std::vector<uint8_t> nb = file_bytes(20000, 10000, 9);
    UINT w;
    CHECK(f_write(&f, nb.data(), (UINT)nb.size(), &w) == FR_OK && w == nb.size());
    f_close(&f);
    printf("  image_moves %lu preserved %lu lost %lu\n", (unsigned long)cow.stats.image_moves,
           (unsigned long)cow.stats.preserved, (unsigned long)cow.stats.lost);
    CHECK(cow.stats.image_moves >= 10000 / 512);
    CHECK(cow.stats.preserved == 11 - 2);   // blocks 19..29, two already pooled
    CHECK(cow.stats.lost == 0);
    CHECK(compare_all(cow, model) == 0);    // the power-up content, not the new file
    for (uint32_t b = 19; b <= 29; b++) CHECK(cow.slotOf(b * 1024) != COW_NONE && cow.pinnedSlot(cow.slotOf(b * 1024)));
    CHECK(!cow.pinnedSlot(cow.slotOf(P3)));

    // Pinned blocks are not reclaimable: fill the pool, the 13th change drops
    // (16 slots: 11 pinned + P3 + 4 more)
    for (uint32_t k = 0; k < 4; k++) { uint32_t a = (150 + k) * 1024; model[a] ^= 1; cow.write(a, model[a]); }
    CHECK(cow.stats.drops == 0);
    uint32_t a = 200 * 1024;
    cow.write(a, (uint8_t)(model[a] ^ 1));
    CHECK(cow.stats.drops == 1 && cow.stats.pool_full == 1);
    CHECK(compare_all(cow, model) == 0);

    // clear(): back to the power-up image - the pinned blocks hold it
    // (P1/P2 lose their Z80 change? no: their pool content IS the disk, and
    // the power-up bytes are gone from flash, so they stay as they are)
    cow.clear();
    CHECK(cow.stats.pool_full == 0 && cow.stats.used == 11);
    for (uint32_t i = 0; i < SIZE; i++) {
        const uint32_t blk = i / 1024;
        if (blk < 19 || blk > 29) model[i] = pattern(i, 1);
    }
    CHECK(compare_all(cow, model) == 0);
    g_remap_hook = nullptr;
    g_cow = nullptr;
}

// Delete the image and write a different file until the volume is full,
// so every cluster the image had is reused (FatFS allocates forward from
// its last position, wrapping round to the freed clusters at the end)
static void replace_image() {
    CHECK(f_unlink("flash:/IMG.BIN") == FR_OK);
    FIL f;
    CHECK(f_open(&f, "flash:/NEW.BIN", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    std::vector<uint8_t> b(4096);
    uint32_t pos = 0;
    for (;;) {
        for (uint32_t i = 0; i < b.size(); i++) b[i] = pattern(pos + i, 5);
        UINT w = 0;
        if (f_write(&f, b.data(), (UINT)b.size(), &w) != FR_OK || w < b.size()) break;
        pos += w;
    }
    f_close(&f);
    FATFS* fs; DWORD nfree = 0;
    CHECK(f_getfree("flash:", &nfree, &fs) == FR_OK && nfree == 0);
}

static uint32_t poisoned_sectors(CowStore& cow, uint32_t size) {
    uint32_t n = 0;
    for (uint32_t s = 0; s < size / 512; s++) {
        uint32_t ee = 0;
        for (uint32_t o = 0; o < 512; o++) ee += cow.read(s * 512 + o) == 0xEE;
        if (ee == 512) n++;
    }
    return n;
}

static void test_replace() {
    printf("image deleted and replaced while running\n");
    {   // a pool big enough to keep every block: the disk does not change
        const uint32_t SIZE = 128 * 1024;
        CowStore cow;
        std::vector<uint8_t> model;
        CHECK(open_disk(cow, SIZE, 200, model));
        g_cow = &cow; g_remap_hook = hook;
        model[777] ^= 0x40; cow.write(777, model[777]);
        replace_image();   // more than was freed: every cluster reused
        printf("  big pool: image_moves %lu preserved %lu lost %lu\n",
               (unsigned long)cow.stats.image_moves, (unsigned long)cow.stats.preserved,
               (unsigned long)cow.stats.lost);
        CHECK(cow.stats.image_moves >= SIZE / 512);
        CHECK(cow.stats.lost == 0);
        CHECK(compare_all(cow, model) == 0);
        g_remap_hook = nullptr; g_cow = nullptr;
    }
    {   // too small a pool: the loss is counted, blocks read the new data
        const uint32_t SIZE = 256 * 1024;
        CowStore cow;
        std::vector<uint8_t> model;
        CHECK(open_disk(cow, SIZE, 16, model));
        g_cow = &cow; g_remap_hook = hook;
        replace_image();
        printf("  small pool: preserved %lu lost %lu\n",
               (unsigned long)cow.stats.preserved, (unsigned long)cow.stats.lost);
        CHECK(cow.stats.preserved == 16);
        CHECK(cow.stats.lost > 0);
        uint32_t changed = 0;
        for (uint32_t b = 0; b < SIZE / 1024; b++) {
            bool same = true;
            for (uint32_t o = 0; o < 1024 && same; o++) same = cow.read(b * 1024 + o) == model[b * 1024 + o];
            if (!same) changed++;
        }
        CHECK(changed <= cow.stats.lost);        // every changed block is accounted for
        CHECK(poisoned_sectors(cow, SIZE) == 0); // never a recycled page
        g_remap_hook = nullptr; g_cow = nullptr;
    }
}

static void test_positions() {
    printf("positions\n");
    // The counter: 64 KB page wrap, the page bits kept
    CHECK(mr1r18_next_pos(0x0000FFFF) == 0x00000000);
    CHECK(mr1r18_next_pos(0x0002FFFF) == 0x00020000);
    CHECK(mr1r18_next_pos(0x00021234) == 0x00021235);
    // The review's case: a 64 KB disk, address 0xFFFF, two data reads
    CowStore cow;
    std::vector<uint8_t> model;
    CHECK(open_disk(cow, 64 * 1024, 4, model));
    uint32_t pos = 0xFFFF;
    CHECK(cow.read(pos) == model[0xFFFF]); pos = mr1r18_next_pos(pos);
    CHECK(pos == 0);
    CHECK(cow.read(pos) == model[0]);
    cow.write(pos, (uint8_t)(model[0] ^ 1)); model[0] ^= 1;
    CHECK(cow.stats.out_of_range == 0);
    // A stray position past the end wraps into page 0 (counted)
    CHECK(cow.read(0x10000) == model[0]);
    cow.write(0x10005, (uint8_t)(model[5] ^ 2)); model[5] ^= 2;
    CHECK(cow.read(5) == model[5]);
    CHECK(cow.read(0xFFFFFFFF) == model[0xFFFF]);
    CHECK(cow.stats.out_of_range == 3);
    CHECK(compare_all(cow, model) == 0);
    // Sizes that are not whole 64 KB pages are refused (the stray-position
    // wrap into page 0 relies on at least one full page)
    CowStore small;
    CHECK(!small.setup(1024, 4, g_flash.data(), &g_busy));
    CHECK(!small.setup(64 * 1024 + 1024, 4, g_flash.data(), &g_busy));
    CHECK(small.setup(64 * 1024, 4, g_flash.data(), &g_busy));
}

static void test_busy() {
    printf("flash busy\n");
    CowStore cow;
    std::vector<uint8_t> model;
    CHECK(open_disk(cow, 64 * 1024, 4, model));
    g_busy = 1;
    std::thread t([] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); g_busy = 0; });
    auto t0 = std::chrono::steady_clock::now();
    uint8_t v = cow.read(1234);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    t.join();
    CHECK(v == model[1234]);
    CHECK(cow.stats.flash_waits == 1);
    CHECK(ms >= 20);
    printf("  waited %ld ms\n", (long)ms);
}

void learn_tests(int argc, char** argv);

// The image deleted with TRIM (the firmware since learn): the trim hook
// preserves the blocks by a remap onto a zero page before the pages go
static constexpr uint16_t ZERO_PAGE = 1;   // inside RESERVED_PAGES
static void trim_hook(uint32_t, uint16_t old) { if (g_cow) g_cow->remap(old, ZERO_PAGE); }

static void test_replace_trim() {
    printf("image deleted (TRIM) and replaced while running\n");
    const uint32_t SIZE = 128 * 1024;
    CowStore cow;
    std::vector<uint8_t> model;
    CHECK(open_disk(cow, SIZE, 200, model));
    memset(&g_flash[ZERO_PAGE * 512u], 0, 512);
    g_cow = &cow; g_remap_hook = hook; g_emu_trim = true; g_trim_hook = trim_hook;
    model[777] ^= 0x40; cow.write(777, model[777]);
    replace_image();
    printf("  preserved %lu lost %lu\n", (unsigned long)cow.stats.preserved, (unsigned long)cow.stats.lost);
    CHECK(cow.stats.lost == 0);
    CHECK(cow.stats.preserved == SIZE / 1024 - 1);   // all but the pooled one
    CHECK(compare_all(cow, model) == 0);
    CHECK(poisoned_sectors(cow, SIZE) == 0);
    g_remap_hook = nullptr; g_cow = nullptr; g_emu_trim = false; g_trim_hook = nullptr;
}

int main(int argc, char** argv) {
    test_resolve(1);
    test_resolve(4);
    test_semantics();
    test_stress(64, 40, false);    // the game's shape: fewer changed blocks than slots
    test_stress(16, 60, true);     // exhaustion
    test_rewrite();
    test_replace();
    test_replace_trim();
    test_positions();
    test_busy();
    learn_tests(argc, argv);
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
