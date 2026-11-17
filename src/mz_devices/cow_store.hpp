#pragma once

// Copy-on-write RAM disk store (the [ramdisk] mode=cow backend).
//
// The disk's content is a read-only image in flash, read straight through
// XIP, plus a small SRAM pool of 1 KB blocks that holds every block the Z80
// has changed. Per 512-byte image sector a 16-bit flash_fs page index
// (sect_) locates the byte in flash; per 1 KB disk block one byte (slot_of_)
// says which pool slot, if any, overrides it.
//
// Reads:  pooled block -> pool byte; else the image byte via XIP.
// Writes: pooled block -> pool byte; else equal to the image byte -> no-op
//         (write-compare: the MZ-800 boot copies the whole image from floppy
//         onto the disk and only a handful of bytes really change); else a
//         slot is taken (the block's 1 KB copied in from flash) and written.
//
// Every pooled block keeps a count of its bytes that differ from the image
// (diff_). A block whose count is back at 0 (e.g. the size probe's marker
// bytes, written and then restored) is identical to flash and is reclaimed
// when the pool runs out. When no slot is free and none is reclaimable:
//  - write-back off (plain cow, or learn frozen): the write is DROPPED:
//    counted, the address remembered, the "pool full" state latched until
//    the pool is cleared - see CowStats (exposed via /api/ramdisk and
//    /api/heap);
//  - write-back on ([ramdisk] learn, cow_image.hpp): the least recently
//    written dirty blocks (a clock over per-slot "written" bits) are written
//    into the image file - their content becomes the image's - and freed, a
//    batch at a time, inside the very port write that needed the slot (the
//    Z80 held by EXWAIT). See writeBack() for the policy and the wear cap.
//
// The disk is a snapshot of the image at power-up. A write to the image
// file while the disk runs moves the written sectors to new flash pages
// (flash_fs never updates in place) and remap() is told before the old page
// can be recycled: the affected 1 KB block is first copied into the pool
// from the old page and pinned there (never reclaimed, kept by clear()), so
// the running disk never shows the new file, another file reusing the
// freed clusters, or a recycled page. Deleting the file writes no data
// sectors, and its trim (FatFS CTRL_TRIM, flash_fs_trim_FAT_sectors) is told
// to the owner before the pages are dropped: the same remap() to the zero
// page preserves the blocks. When no slot is left to preserve a block, its
// sectors are counted as lost and the state reads image_changed until
// power-up - never silently. The learn write-back's own rewrites are the
// exception: the block just written IS the image now, so the table follows
// the new page and the slot is freed (nothing pinned).
//
// Positions are always < size (the MR1R18 counter wraps inside its 64 KB
// page, see ramdisk_pos.hpp); read()/write() still wrap a stray position
// into page 0 instead of indexing past the tables (counted).
//
// Everything on the bus path is header-inline (inlined into the RAM_FUNC
// port handlers) or RAM_FUNC, and touches only SRAM tables and XIP: no
// FatFS, no flash-resident code - except the learn write-back, which calls
// flash_fs through CowLearnIo from the port handler (core 1, the only flash
// user in MZ-800 mode; no flash operation is in progress then). While a flash program/erase is in progress
// (*busy_ != 0) no XIP read is issued: the access spins, the Z80 held in
// /WAIT by the bus loop's EXWAIT.
//
// No pico-sdk dependencies: tests/cow_ramdisk builds this on the host.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "common.hpp"

// The flash-busy flag: an MMIO-free volatile word in the firmware; the
// host tests poll it across threads, so there it is a std::atomic
#ifdef COW_HOST_ATOMIC_BUSY
#include <atomic>
using CowBusy = const std::atomic<uint32_t>;
#else
using CowBusy = const volatile uint32_t;
#endif

constexpr uint32_t COW_BLOCK_SHIFT = 10;                 // 1 KB pool blocks
constexpr uint32_t COW_BLOCK = 1u << COW_BLOCK_SHIFT;
constexpr uint32_t COW_SECT_SHIFT = 9;                   // 512 B flash pages
constexpr uint32_t COW_SECT = 1u << COW_SECT_SHIFT;
constexpr uint8_t  COW_NONE = 0xFF;
constexpr uint32_t COW_MAX_SLOTS = 255;                  // slot index is a byte
constexpr uint16_t COW_FREE = 0xFFFF;                    // owner_ of an empty slot
constexpr uint32_t COW_NOBLK = 0xFFFFFFFFu;
// Hot-block tracking covers the first COW_HOT_MAX blocks (three bitmaps of
// COW_HOT_BYTES each fit the learn trailer's 512 B sector): 1,184 KB
constexpr uint32_t COW_HOT_BYTES = 148;
constexpr uint32_t COW_HOT_MAX = COW_HOT_BYTES * 8;

struct CowStats {
    uint32_t size;          // disk bytes
    uint32_t sectors;       // image sectors resolved (size / 512)
    uint32_t pool_blocks;   // slots
    uint32_t used;          // slots holding a block now
    uint32_t peak;          // most slots ever in use
    uint32_t allocs;        // blocks copied in from flash
    uint32_t reclaims;      // slots taken back from blocks equal to the image again
    uint32_t drops;         // writes LOST: pool full, nothing reclaimable
    uint32_t last_drop;     // disk address of the last lost write
    uint32_t pool_full;     // 1 after a drop, until the pool is cleared
    uint32_t image_moves;   // image sectors that moved in flash (the file written)
    uint32_t preserved;     // blocks pinned in the pool to keep the power-up image
    uint32_t lost;          // image SECTORS moved with no slot to keep their block (image_changed)
    uint32_t out_of_range;  // stray positions >= size, wrapped into page 0
    uint32_t flash_waits;   // accesses that waited for a flash program/erase
    uint32_t clears;        // pool clears (REST /api/ramdisk/clear at a Z80 reset)
};

// The flash side of the learn write-back (firmware: cow_image.cpp)
struct CowLearnIo {
    // Write the disk's 512 B sector `sector` into the image; returns its new
    // page (a sect_ entry), 0 = failed (the image keeps the old content)
    uint16_t (*write_sector)(void* ctx, uint32_t sector, const uint8_t* data);
    void (*sync)(void* ctx);            // persist the flash map
    uint64_t (*now_us)(void* ctx);      // monotonic microseconds (64-bit: no wrap)
    void* ctx;
};

struct CowLearnCfg {
    uint32_t batch = 4;           // blocks per write-back (4 x 1 KB = one 4 KB flash sector)
    uint32_t reserve = 0;         // learning burst: keep this many slots free ahead of need (0 = off)
    uint32_t burst_ms = 3000;     // ...while the last need-driven write-back is this recent
    uint32_t cap = 1024;          // wear cap: blocks per cap_period (token bucket); 0 = no write-back
    uint32_t cap_period_s = 3600;
    uint32_t sync_every = 64;     // blocks written back between map syncs
};

struct CowLearnStats {
    uint32_t batches;       // write-back episodes (each one Z80 stall)
    uint32_t blocks;        // blocks written back (freed)
    uint32_t ahead;         // ... of them ahead of need (learning burst reserve)
    uint32_t sectors;       // 512 B sectors written into the image
    uint32_t fails;         // sector writes that failed (flash volume full)
    uint32_t cap_drops;     // writes dropped while the wear cap was exhausted
    uint32_t syncs;         // map syncs requested
    uint64_t stall_us;      // Z80 held during write-back
    uint32_t stall_max_us;  // longest single episode
    uint32_t session_blocks;  // this session (MZ-800 reset to reset), volatile ones excluded
    uint32_t session_vol;     // volatile blocks written back this session (no other victim)
    uint32_t session_drops;
    uint32_t vol_skips;     // volatile blocks passed over as victims
    uint32_t vol_forced;    // volatile blocks written back anyway (nothing else freed a slot)
    bool     capped;        // the cap was hit since power-up (latched)
};

class CowStore {
public:
    CowStore() = default;
    ~CowStore() { release(); }
    CowStore(const CowStore&) = delete;
    CowStore& operator=(const CowStore&) = delete;

    // Allocates the tables and the pool (largest first). size: a multiple
    // of 1 KB. xip_base: page 0 of the flash_fs storage region. busy:
    // non-zero during flash program/erase. Returns false when out of RAM
    // (nothing stays allocated). The sector table must then be filled
    // (sectorTable()) before the first access.
    bool setup(uint32_t size, uint32_t pool_blocks, const uint8_t* xip_base,
               CowBusy* busy) {
        release();
        // Whole 64 KB pages only: outOfRange() wraps a stray position into
        // page 0, which is in range only if the disk has at least one page
        if (!size || (size & 0xFFFFu) || !pool_blocks) return false;
        if (pool_blocks > COW_MAX_SLOTS) pool_blocks = COW_MAX_SLOTS;
        nblk_ = size >> COW_BLOCK_SHIFT;
        nsect_ = size >> COW_SECT_SHIFT;
        nslot_ = pool_blocks;
        pool_  = static_cast<uint8_t*>(malloc(nslot_ * COW_BLOCK));
        sect_  = static_cast<uint16_t*>(malloc(nsect_ * sizeof(uint16_t)));
        slot_of_ = static_cast<uint8_t*>(malloc(nblk_));
        diff_  = static_cast<uint16_t*>(malloc(nslot_ * sizeof(uint16_t)));
        owner_ = static_cast<uint16_t*>(malloc(nslot_ * sizeof(uint16_t)));
        pinned_ = static_cast<uint8_t*>(malloc(nslot_ * 2));
        hot_n_ = nblk_ < COW_HOT_MAX ? nblk_ : COW_HOT_MAX;
        hot_ = static_cast<uint8_t*>(calloc(3, COW_HOT_BYTES));
        if (!pool_ || !sect_ || !slot_of_ || !diff_ || !owner_ || !pinned_ || !hot_) { release(); return false; }
        ref_ = pinned_ + nslot_;
        memset(sect_, 0, nsect_ * sizeof(uint16_t));
        memset(slot_of_, COW_NONE, nblk_);
        for (uint32_t i = 0; i < nslot_; i++) owner_[i] = COW_FREE;
        memset(pinned_, 0, nslot_ * 2);
        size_ = size;
        xip_ = xip_base;
        busy_ = busy;
        memset(&stats, 0, sizeof(stats));
        memset(&learn, 0, sizeof(learn));
        stats.size = size;
        stats.sectors = nsect_;
        stats.pool_blocks = nslot_;
        clear();
        stats.clears = 0;
        return true;
    }

    void release() {
        free(pool_); free(sect_); free(slot_of_); free(diff_); free(owner_); free(pinned_); free(hot_);
        hot_ = nullptr; hot_n_ = 0;
        pool_ = nullptr; sect_ = nullptr; slot_of_ = nullptr; diff_ = nullptr; owner_ = nullptr;
        pinned_ = ref_ = nullptr;
        nblk_ = nsect_ = nslot_ = used_ = size_ = 0;
        wb_on_ = false;
    }

    bool ready() const { return pool_ != nullptr; }
    uint16_t* sectorTable() { return sect_; }
    uint32_t sectorCount() const { return nsect_; }
    // Bytes of heap held (pool + tables)
    uint32_t ramBytes() const { return nslot_ * (COW_BLOCK + 6) + nsect_ * 2 + nblk_ + 3 * COW_HOT_BYTES; }

    // Back to the power-up image: every block the Z80 changed reads from
    // flash again. Pinned blocks (the power-up content of image sectors
    // rewritten since, see remap) stay: flash no longer holds it. Ends
    // the pool_full state; drops stays a running total. all: the pinned
    // blocks too (learn relearn: the image is blanked on purpose).
    void clear(bool all = false) {
        if (!slot_of_) return;
        used_ = 0;
        for (uint32_t s = 0; s < nslot_; s++) {
            if (owner_[s] == COW_FREE) continue;
            if (pinned_[s] && !all) { used_++; continue; }
            pinned_[s] = 0;
            ref_[s] = 0;
            slot_of_[owner_[s]] = COW_NONE;
            owner_[s] = COW_FREE;
        }
        scan_ = 0;
        stats.used = used_;
        stats.pool_full = 0;
        stats.clears++;
    }

    ALWAYS_INLINE uint8_t imageByte(uint32_t pos) {
        if (*busy_) waitFlash();
        return xip_[((uint32_t)sect_[pos >> COW_SECT_SHIFT] << COW_SECT_SHIFT) | (pos & (COW_SECT - 1))];
    }

    ALWAYS_INLINE uint8_t read(uint32_t pos) {
        if (__builtin_expect(pos >= size_, 0)) pos = outOfRange(pos);
        const uint8_t s = slot_of_[pos >> COW_BLOCK_SHIFT];
        if (s != COW_NONE)
            return pool_[((uint32_t)s << COW_BLOCK_SHIFT) | (pos & (COW_BLOCK - 1))];
        return imageByte(pos);
    }

    ALWAYS_INLINE void write(uint32_t pos, uint8_t v) {
        if (__builtin_expect(pos >= size_, 0)) pos = outOfRange(pos);
        const uint8_t s = slot_of_[pos >> COW_BLOCK_SHIFT];
        const uint8_t img = imageByte(pos);
        if (s != COW_NONE) {
            uint8_t* p = &pool_[((uint32_t)s << COW_BLOCK_SHIFT) | (pos & (COW_BLOCK - 1))];
            diff_[s] = (uint16_t)(diff_[s] + (v != img) - (*p != img));
            *p = v;
            ref_[s] = 1;                // the clock's "recently written"
            return;
        }
        if (v == img) return;           // write-compare: nothing changes
        allocAndWrite(pos, v);
    }

    // The image sector at flash page old_entry now lives at new_entry: the
    // file was written (flash_fs never updates in place). Called before the
    // old page can be recycled - it still holds the power-up content - so
    // the block over it is kept in the pool (pinned) first, then the table
    // follows the new page (it must never point at a recyclable page).
    // Single context (flash_fs's caller), never concurrent with the bus
    // path on core 1. Returns the sectors moved.
    uint32_t remap(uint16_t old_entry, uint16_t new_entry);

    // ---- learn: write-back into the image (off until configured) ----
    void setLearn(const CowLearnIo& io, const CowLearnCfg& cfg) {
        io_ = io;
        cfg_ = cfg;
        has_io_ = io.write_sector != nullptr && io.now_us != nullptr;
        tokens_ = cfg.cap;
        per_token_us_ = cfg.cap ? (uint32_t)((uint64_t)cfg.cap_period_s * 1000000u / cfg.cap) : 0;
        if (has_io_) refill_at_ = io_.now_us(io_.ctx);
    }
    void setWriteBack(bool on) { wb_on_ = on && has_io_ && cfg_.cap; }
    bool writeBackOn() const { return wb_on_; }
    uint32_t tokens() const { return tokens_; }
    // Point sector i at entry without preserving anything (the owner's own
    // trim of the image: relearn, a fresh blank image)
    void setSector(uint32_t i, uint16_t entry) {
        if (i >= nsect_) return;
        sect_[i] = entry;
        const uint8_t s = slot_of_[i >> (COW_BLOCK_SHIFT - COW_SECT_SHIFT)];
        if (s != COW_NONE) diff_[s] = countDiff(s, i >> (COW_BLOCK_SHIFT - COW_SECT_SHIFT));
    }
    // Write back up to max_blocks of the least recently written dirty
    // blocks (clean ones are simply freed). Returns the slots freed.
    uint32_t writeBack(uint32_t max_blocks, bool ahead);
    // Session boundary (MZ-800 reset): returns the blocks written back in
    // the session and starts the next session's counters. RAM only: the
    // map sync it may call for is the owner's, later (syncPending/syncNow)
    uint32_t endSession() {
        const uint32_t n = learn.session_blocks;
        learn.session_blocks = 0;
        learn.session_vol = 0;
        learn.session_drops = 0;
        need_wb_seen_ = false;
        return n;
    }
    bool syncPending() const { return since_sync_ != 0; }
    uint64_t lastWritebackUs() const { return last_wb_us_; }
    void markSynced() { since_sync_ = 0; }

    // ---- hot blocks: written back in two sessions -> volatile ----
    // Bitmaps (COW_HOT_BYTES each, persisted in the learn trailer): sess =
    // written back this session, seen = in an earlier one, vol = volatile.
    // A volatile block is never chosen as a write-back victim while another
    // one can free a slot, and its write-backs do not count against
    // convergence: scratch that differs every session stops churning the
    // image.
    bool isVolatile(uint32_t blk) const { return blk < hot_n_ && (hot_[2 * COW_HOT_BYTES + (blk >> 3)] >> (blk & 7) & 1); }
    // Session end: sess & seen -> vol; seen |= sess; sess = 0. Returns new volatiles.
    uint32_t foldSession() {
        uint32_t n = 0;
        uint8_t* sess = hot_; uint8_t* seen = hot_ + COW_HOT_BYTES; uint8_t* vol = hot_ + 2 * COW_HOT_BYTES;
        for (uint32_t i = 0; i < COW_HOT_BYTES; i++) {
            const uint8_t nv = (uint8_t)(sess[i] & seen[i] & ~vol[i]);
            n += (uint32_t)__builtin_popcount(nv);
            vol[i] |= nv;
            seen[i] |= sess[i];
            sess[i] = 0;
        }
        return n;
    }
    uint32_t volatileCount() const {
        uint32_t n = 0;
        for (uint32_t i = 0; i < COW_HOT_BYTES; i++) n += (uint32_t)__builtin_popcount(hot_[2 * COW_HOT_BYTES + i]);
        return n;
    }
    void clearHot() { memset(hot_, 0, 3 * COW_HOT_BYTES); }
    void exportHot(uint8_t* dst) const { memcpy(dst, hot_, 3 * COW_HOT_BYTES); }
    void importHot(const uint8_t* src) { memcpy(hot_, src, 3 * COW_HOT_BYTES); }
    void syncNow() {
        if (since_sync_ && io_.sync) { io_.sync(io_.ctx); learn.syncs++; }
        since_sync_ = 0;
    }
    uint32_t dirtyCount() const {
        uint32_t d = 0;
        for (uint32_t s = 0; s < nslot_; s++)
            d += (owner_[s] != COW_FREE && diff_[s] != 0 && !pinned_[s]);
        return d;
    }
    uint32_t pinnedCount() const {
        uint32_t d = 0;
        for (uint32_t s = 0; s < nslot_; s++) d += (owner_[s] != COW_FREE && pinned_[s]);
        return d;
    }

    // Test/diagnostic access
    uint8_t slotOf(uint32_t pos) const { return slot_of_[pos >> COW_BLOCK_SHIFT]; }
    uint16_t diffOf(uint8_t slot) const { return diff_[slot]; }
    bool pinnedSlot(uint8_t slot) const { return pinned_[slot] != 0; }
    uint16_t sectorEntry(uint32_t i) const { return sect_[i]; }

    CowStats stats{};
    CowLearnStats learn{};

private:
    // Out of line, RAM_FUNC (cow_store.cpp): the slow paths
    RAM_FUNC void waitFlash();
    RAM_FUNC uint32_t outOfRange(uint32_t pos);
    RAM_FUNC int takeSlot();                                // a free or reclaimable slot, or -1
    RAM_FUNC void copyIn(uint32_t s, uint32_t blk);         // the block's 1 KB from flash
    RAM_FUNC void allocAndWrite(uint32_t pos, uint8_t v);   // a block's first differing byte
    RAM_FUNC uint16_t countDiff(uint8_t s, uint32_t blk);
    void freeSlot(uint32_t s) {
        slot_of_[owner_[s]] = COW_NONE;
        owner_[s] = COW_FREE;
        diff_[s] = 0;
        ref_[s] = 0;
        used_--;
    }
    bool takeToken(uint64_t now);
    bool writeBackSlot(uint32_t s);

    const uint8_t* xip_ = nullptr;
    CowBusy* busy_ = nullptr;
    uint8_t*  pool_ = nullptr;
    uint16_t* sect_ = nullptr;
    uint8_t*  slot_of_ = nullptr;
    uint16_t* diff_ = nullptr;
    uint16_t* owner_ = nullptr;
    uint8_t*  pinned_ = nullptr;
    uint8_t*  ref_ = nullptr;      // per slot: written since the clock hand passed (shares pinned_'s block)
    uint8_t*  hot_ = nullptr;      // sess | seen | vol bitmaps, COW_HOT_BYTES each
    uint32_t  hot_n_ = 0;
    uint32_t size_ = 0;
    uint32_t nblk_ = 0, nsect_ = 0, nslot_ = 0, used_ = 0, scan_ = 0, hand_ = 0;

    // learn
    CowLearnIo io_{};
    CowLearnCfg cfg_{};
    bool has_io_ = false;
    bool wb_on_ = false;
    uint32_t wb_blk_ = COW_NOBLK;     // the block being written back: remap follows, never pins
    uint32_t tokens_ = 0, per_token_us_ = 0;
    uint64_t refill_at_ = 0;
    uint32_t since_sync_ = 0;
    bool need_wb_seen_ = false;
    uint64_t last_need_wb_us_ = 0;
    uint64_t last_wb_us_ = 0;          // the end of the last write-back episode
};
