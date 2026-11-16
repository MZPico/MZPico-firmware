// See cow_store.hpp
#include "cow_store.hpp"

RAM_FUNC void CowStore::waitFlash() {
    stats.flash_waits++;
    while (*busy_) { }
}

// A position past the end (cannot happen through the MR1R18 port logic,
// whose counter wraps inside its page): wrap it into page 0 the way the
// counter would, rather than index past the tables
RAM_FUNC uint32_t CowStore::outOfRange(uint32_t pos) {
    stats.out_of_range++;
    return pos & 0xFFFF;   // size_ >= 64 KB: always in range
}

// An empty slot, else one whose block is equal to the image again (and
// not pinned); -1 when the pool holds nothing it can give back
RAM_FUNC int CowStore::takeSlot() {
    const bool any_free = used_ < nslot_;
    for (uint32_t i = 0; i < nslot_; i++) {
        uint32_t s = scan_ + i;
        if (s >= nslot_) s -= nslot_;
        if (any_free ? owner_[s] == COW_FREE
                     : (diff_[s] == 0 && !pinned_[s])) {
            scan_ = (s + 1 < nslot_) ? s + 1 : 0;
            if (any_free) {
                used_++;
            } else {
                slot_of_[owner_[s]] = COW_NONE;
                stats.reclaims++;
            }
            owner_[s] = COW_FREE;
            pinned_[s] = 0;
            return (int)s;
        }
    }
    return -1;
}

// 1 KB in from flash: two 512 B pages, word copies (both sides are
// 4-aligned: pool slots are 1 KB apart from a malloc'd base, flash pages
// 512 B aligned). A loop, not memcpy: this runs from RAM.
RAM_FUNC void CowStore::copyIn(uint32_t s, uint32_t blk) {
    uint32_t* dst = reinterpret_cast<uint32_t*>(&pool_[s << COW_BLOCK_SHIFT]);
    for (uint32_t h = 0; h < 2; h++) {
        if (*busy_) waitFlash();
        const uint32_t* src = reinterpret_cast<const uint32_t*>(
            xip_ + ((uint32_t)sect_[(blk << 1) | h] << COW_SECT_SHIFT));
        for (uint32_t w = 0; w < COW_SECT / 4; w++) *dst++ = src[w];
    }
    diff_[s] = 0;
    owner_[s] = (uint16_t)blk;
    slot_of_[blk] = (uint8_t)s;
    if (used_ > stats.used) stats.used = used_;
    if (stats.used > stats.peak) stats.peak = stats.used;
}

// Slow path of write(): a block's first differing byte
RAM_FUNC void CowStore::allocAndWrite(uint32_t pos, uint8_t v) {
    const int s = takeSlot();
    if (s < 0) {
        stats.drops++;
        stats.last_drop = pos;
        stats.pool_full = 1;
        return;
    }
    copyIn((uint32_t)s, pos >> COW_BLOCK_SHIFT);
    pool_[((uint32_t)s << COW_BLOCK_SHIFT) | (pos & (COW_BLOCK - 1))] = v;
    diff_[s] = 1;
    stats.allocs++;
}

uint32_t CowStore::remap(uint16_t old_entry, uint16_t new_entry) {
    uint32_t n = 0;
    if (!sect_) return 0;
    for (uint32_t i = 0; i < nsect_; i++) {
        if (sect_[i] != old_entry) continue;
        const uint32_t blk = i >> (COW_BLOCK_SHIFT - COW_SECT_SHIFT);
        int s = slot_of_[blk];
        if (s == COW_NONE) {
            // Keep the power-up content: copy the block in while the old
            // page (and the block's other page) still hold it
            s = takeSlot();
            if (s >= 0) {
                copyIn((uint32_t)s, blk);
                stats.preserved++;
            } else {
                stats.lost++;   // the block now reads the new file: image_changed
            }
        }
        if (s >= 0) pinned_[s] = 1;
        sect_[i] = new_entry;   // never left pointing at a recyclable page
        n++;
    }
    stats.image_moves += n;
    return n;
}
