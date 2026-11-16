// See cow_store.hpp
#include "cow_store.hpp"

RAM_FUNC void CowStore::waitFlash() {
    stats.flash_waits++;
    while (*busy_) { }
}

RAM_FUNC uint16_t CowStore::countDiff(uint8_t s, uint32_t blk) {
    uint16_t d = 0;
    const uint8_t* p = &pool_[(uint32_t)s << COW_BLOCK_SHIFT];
    const uint32_t base = blk << COW_BLOCK_SHIFT;
    for (uint32_t o = 0; o < COW_BLOCK; o++)
        d = (uint16_t)(d + (p[o] != imageByte(base + o)));
    return d;
}

// Slow path of write(): a block's first differing byte
RAM_FUNC void CowStore::allocAndWrite(uint32_t pos, uint8_t v) {
    uint32_t s = 0;
    if (used_ < nslot_) {
        s = used_++;
    } else {
        // Pool full: take back a block that is equal to the image again
        uint32_t i = 0;
        for (; i < nslot_; i++) {
            s = scan_ + i;
            if (s >= nslot_) s -= nslot_;
            if (diff_[s] == 0) break;
        }
        if (i == nslot_) {
            stats.drops++;
            stats.last_drop = pos;
            return;
        }
        scan_ = (s + 1 < nslot_) ? s + 1 : 0;
        slot_of_[owner_[s]] = COW_NONE;
        stats.reclaims++;
    }
    const uint32_t blk = pos >> COW_BLOCK_SHIFT;
    // 1 KB in from flash: two 512 B pages, word copies (both sides are
    // 4-aligned: pool slots are 1 KB apart from a malloc'd base, flash
    // pages 512 B aligned). A loop, not memcpy: this runs from RAM.
    uint32_t* dst = reinterpret_cast<uint32_t*>(&pool_[s << COW_BLOCK_SHIFT]);
    for (uint32_t h = 0; h < 2; h++) {
        if (*busy_) waitFlash();
        const uint32_t* src = reinterpret_cast<const uint32_t*>(
            xip_ + ((uint32_t)sect_[(blk << 1) | h] << COW_SECT_SHIFT));
        for (uint32_t w = 0; w < COW_SECT / 4; w++) *dst++ = src[w];
    }
    pool_[(s << COW_BLOCK_SHIFT) | (pos & (COW_BLOCK - 1))] = v;
    diff_[s] = 1;
    owner_[s] = (uint16_t)blk;
    slot_of_[blk] = (uint8_t)s;
    stats.allocs++;
    if (used_ > stats.used) stats.used = used_;
    if (stats.used > stats.peak) stats.peak = stats.used;
}

