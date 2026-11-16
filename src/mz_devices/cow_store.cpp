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
            ref_[s] = 0;
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
    stats.used = used_;
    if (stats.used > stats.peak) stats.peak = stats.used;
}

// Slow path of write(): a block's first differing byte
RAM_FUNC void CowStore::allocAndWrite(uint32_t pos, uint8_t v) {
    int s = takeSlot();
    if (s < 0 && wb_on_) {
        // Learn, need-driven: the Z80 is waiting on this very write anyway
        writeBack(cfg_.batch, false);
        s = takeSlot();
        if (s >= 0) {
            need_wb_seen_ = true;
            last_need_wb_us_ = io_.now_us(io_.ctx);
        }
    }
    if (s < 0) {
        stats.drops++;
        stats.last_drop = pos;
        stats.pool_full = 1;
        learn.session_drops++;
        if (learn.capped && wb_on_) learn.cap_drops++;
        return;
    }
    copyIn((uint32_t)s, pos >> COW_BLOCK_SHIFT);
    pool_[((uint32_t)s << COW_BLOCK_SHIFT) | (pos & (COW_BLOCK - 1))] = v;
    diff_[s] = 1;
    ref_[s] = 1;
    stats.allocs++;

    // Ahead of need, only inside a learning burst (a need-driven write-back
    // happened moments ago: the software fills the disk with data the image
    // lacks - its first boot): keep `reserve` slots free so the burst's end
    // leaves the pool mostly clean for what follows. Never outside such a
    // burst: play's own blocks would go into the image for nothing.
    if (wb_on_ && cfg_.reserve && need_wb_seen_ && nslot_ - used_ < cfg_.reserve &&
        (uint32_t)(io_.now_us(io_.ctx) - last_need_wb_us_) < cfg_.burst_ms * 1000u)
        writeBack(cfg_.batch, true);
}

// The wear cap: a token bucket of cfg_.cap blocks, refilled over
// cfg_.cap_period_s (1024 an hour by default)
bool CowStore::takeToken(uint32_t now) {
    if (per_token_us_) {
        const uint32_t el = now - refill_at_;
        if (el >= per_token_us_) {
            const uint32_t add = el / per_token_us_;
            refill_at_ += add * per_token_us_;
            tokens_ = (tokens_ + add > cfg_.cap) ? cfg_.cap : tokens_ + add;
        }
    }
    if (tokens_ == 0) { learn.capped = true; return false; }
    tokens_--;
    return true;
}

// One dirty slot into the image: the halves that differ from it, then the
// slot is free (its block reads from flash again, now the same bytes).
// remap() sees wb_blk_ and only follows the moved page: nothing is pinned.
bool CowStore::writeBackSlot(uint32_t s) {
    const uint32_t blk = owner_[s];
    wb_blk_ = blk;
    bool ok = true;
    for (uint32_t h = 0; h < 2; h++) {
        const uint32_t i = (blk << 1) | h;
        const uint8_t* src = &pool_[(s << COW_BLOCK_SHIFT) | (h << COW_SECT_SHIFT)];
        if (memcmp(src, xip_ + ((uint32_t)sect_[i] << COW_SECT_SHIFT), COW_SECT) == 0) continue;
        const uint16_t e = io_.write_sector(io_.ctx, i, src);
        if (!e) { learn.fails++; ok = false; break; }
        sect_[i] = e;               // also for a hole (no old page: no remap call)
        learn.sectors++;
    }
    wb_blk_ = COW_NOBLK;
    if (!ok) {
        diff_[s] = countDiff((uint8_t)s, blk);   // one half may have landed
        return false;
    }
    freeSlot(s);
    return true;
}

// The write-back episode: up to max_blocks victims of a clock over the
// slots by last write (a slot written since the hand passed gets a second
// chance; pinned slots - the power-up snapshot of an externally changed
// image - are never written back). The Z80 is held throughout (EXWAIT);
// the default batch of 4 blocks is about one 4 KB flash erase + 8 page
// programs, ~50 ms.
uint32_t CowStore::writeBack(uint32_t max_blocks, bool ahead) {
    if (!wb_on_ || !max_blocks || !nslot_) return 0;
    const uint32_t t0 = io_.now_us(io_.ctx);
    uint32_t freed = 0, written = 0;
    for (uint32_t steps = 0; freed < max_blocks && steps < 2 * nslot_ + 1; steps++) {
        const uint32_t s = hand_;
        hand_ = (hand_ + 1 < nslot_) ? hand_ + 1 : 0;
        if (owner_[s] == COW_FREE || pinned_[s]) continue;
        if (diff_[s] == 0) { freeSlot(s); stats.reclaims++; freed++; continue; }
        if (ref_[s]) { ref_[s] = 0; continue; }
        if (!takeToken(io_.now_us(io_.ctx))) break;
        if (!writeBackSlot(s)) break;
        freed++;
        written++;
    }
    if (written) {
        learn.batches++;
        learn.blocks += written;
        learn.session_blocks += written;
        if (ahead) learn.ahead += written;
        since_sync_ += written;
        // Bound what a power cut can undo: the map is persisted every
        // sync_every blocks (and at the session's end)
        if (since_sync_ >= cfg_.sync_every && io_.sync) {
            io_.sync(io_.ctx);
            learn.syncs++;
            since_sync_ = 0;
        }
        const uint32_t dt = io_.now_us(io_.ctx) - t0;
        learn.stall_us += dt;
        if (dt > learn.stall_max_us) learn.stall_max_us = dt;
    }
    stats.used = used_;
    return freed;
}

uint32_t CowStore::remap(uint16_t old_entry, uint16_t new_entry) {
    uint32_t n = 0;
    if (!sect_) return 0;
    for (uint32_t i = 0; i < nsect_; i++) {
        if (sect_[i] != old_entry) continue;
        const uint32_t blk = i >> (COW_BLOCK_SHIFT - COW_SECT_SHIFT);
        if (blk == wb_blk_) {
            // The learn write-back's own rewrite: the pool block IS the
            // image now - follow the page, keep nothing
            sect_[i] = new_entry;
            continue;
        }
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
