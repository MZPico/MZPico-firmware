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
// when the pool runs out. When no slot is free and none is reclaimable, the
// write is DROPPED: counted, the address remembered, the "pool full" state
// latched - see CowStats (exposed via /api/ramdisk and /api/heap).
//
// Everything on the bus path is header-inline (inlined into the RAM_FUNC
// port handlers) or RAM_FUNC, and touches only SRAM tables and XIP: no
// FatFS, no flash-resident code. While a flash program/erase is in progress
// (*busy_ != 0) no XIP read is issued: the access spins, the Z80 held in
// /WAIT by the bus loop's EXWAIT.
//
// No pico-sdk dependencies: tests/cow_ramdisk builds this on the host.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "common.hpp"

constexpr uint32_t COW_BLOCK_SHIFT = 10;                 // 1 KB pool blocks
constexpr uint32_t COW_BLOCK = 1u << COW_BLOCK_SHIFT;
constexpr uint32_t COW_SECT_SHIFT = 9;                   // 512 B flash pages
constexpr uint32_t COW_SECT = 1u << COW_SECT_SHIFT;
constexpr uint8_t  COW_NONE = 0xFF;
constexpr uint32_t COW_MAX_SLOTS = 255;                  // slot index is a byte

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
    uint32_t image_moves;   // image sectors that moved in flash (the file rewritten)
    uint32_t flash_waits;   // accesses that waited for a flash program/erase
    uint32_t clears;        // pool clears (REST /api/ramdisk/clear at a Z80 reset)
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
               const volatile uint32_t* busy) {
        release();
        if (!size || (size & (COW_BLOCK - 1)) || !pool_blocks) return false;
        if (pool_blocks > COW_MAX_SLOTS) pool_blocks = COW_MAX_SLOTS;
        nblk_ = size >> COW_BLOCK_SHIFT;
        nsect_ = size >> COW_SECT_SHIFT;
        nslot_ = pool_blocks;
        pool_  = static_cast<uint8_t*>(malloc(nslot_ * COW_BLOCK));
        sect_  = static_cast<uint16_t*>(malloc(nsect_ * sizeof(uint16_t)));
        slot_of_ = static_cast<uint8_t*>(malloc(nblk_));
        diff_  = static_cast<uint16_t*>(malloc(nslot_ * sizeof(uint16_t)));
        owner_ = static_cast<uint16_t*>(malloc(nslot_ * sizeof(uint16_t)));
        if (!pool_ || !sect_ || !slot_of_ || !diff_ || !owner_) { release(); return false; }
        memset(sect_, 0, nsect_ * sizeof(uint16_t));
        xip_ = xip_base;
        busy_ = busy;
        memset(&stats, 0, sizeof(stats));
        stats.size = size;
        stats.sectors = nsect_;
        stats.pool_blocks = nslot_;
        clear();
        stats.clears = 0;
        return true;
    }

    void release() {
        free(pool_); free(sect_); free(slot_of_); free(diff_); free(owner_);
        pool_ = nullptr; sect_ = nullptr; slot_of_ = nullptr; diff_ = nullptr; owner_ = nullptr;
        nblk_ = nsect_ = nslot_ = used_ = 0;
    }

    bool ready() const { return pool_ != nullptr; }
    uint16_t* sectorTable() { return sect_; }
    uint32_t sectorCount() const { return nsect_; }
    // Bytes of heap held (pool + tables)
    uint32_t ramBytes() const { return nslot_ * (COW_BLOCK + 4) + nsect_ * 2 + nblk_; }

    // Back to the pristine image: every block reads from flash again
    void clear() {
        if (!slot_of_) return;
        memset(slot_of_, COW_NONE, nblk_);
        used_ = 0;
        scan_ = 0;
        stats.used = 0;
        stats.clears++;
    }

    ALWAYS_INLINE uint8_t imageByte(uint32_t pos) {
        if (*busy_) waitFlash();
        return xip_[((uint32_t)sect_[pos >> COW_SECT_SHIFT] << COW_SECT_SHIFT) | (pos & (COW_SECT - 1))];
    }

    ALWAYS_INLINE uint8_t read(uint32_t pos) {
        const uint8_t s = slot_of_[pos >> COW_BLOCK_SHIFT];
        if (s != COW_NONE)
            return pool_[((uint32_t)s << COW_BLOCK_SHIFT) | (pos & (COW_BLOCK - 1))];
        return imageByte(pos);
    }

    ALWAYS_INLINE void write(uint32_t pos, uint8_t v) {
        const uint8_t s = slot_of_[pos >> COW_BLOCK_SHIFT];
        const uint8_t img = imageByte(pos);
        if (s != COW_NONE) {
            uint8_t* p = &pool_[((uint32_t)s << COW_BLOCK_SHIFT) | (pos & (COW_BLOCK - 1))];
            diff_[s] = (uint16_t)(diff_[s] + (v != img) - (*p != img));
            *p = v;
            return;
        }
        if (v == img) return;           // write-compare: nothing changes
        allocAndWrite(pos, v);
    }

    // The image sector at flash page old_entry now lives at new_entry (the
    // file was rewritten; flash_fs never updates in place). Follow it, and
    // recount the differences of the pooled blocks over it. Single context
    // (flash_fs's caller), never concurrent with the bus path on core 1.
    uint32_t remap(uint16_t old_entry, uint16_t new_entry) {
        uint32_t n = 0;
        if (!sect_) return 0;
        for (uint32_t i = 0; i < nsect_; i++) {
            if (sect_[i] != old_entry) continue;
            sect_[i] = new_entry;
            n++;
            const uint32_t blk = i >> (COW_BLOCK_SHIFT - COW_SECT_SHIFT);
            const uint8_t s = slot_of_[blk];
            if (s != COW_NONE) diff_[s] = countDiff(s, blk);
        }
        stats.image_moves += n;
        return n;
    }

    // Test/diagnostic access
    uint8_t slotOf(uint32_t pos) const { return slot_of_[pos >> COW_BLOCK_SHIFT]; }
    uint16_t diffOf(uint8_t slot) const { return diff_[slot]; }

    CowStats stats{};

private:
    // Out of line, RAM_FUNC (cow_store.cpp): the slow paths
    RAM_FUNC void waitFlash();
    RAM_FUNC uint16_t countDiff(uint8_t s, uint32_t blk);
    RAM_FUNC void allocAndWrite(uint32_t pos, uint8_t v);   // a block's first differing byte

    const uint8_t* xip_ = nullptr;
    const volatile uint32_t* busy_ = nullptr;
    uint8_t*  pool_ = nullptr;
    uint16_t* sect_ = nullptr;
    uint8_t*  slot_of_ = nullptr;
    uint16_t* diff_ = nullptr;
    uint16_t* owner_ = nullptr;
    uint32_t nblk_ = 0, nsect_ = 0, nslot_ = 0, used_ = 0, scan_ = 0;
};
