#pragma once

// The COW RAM disk's image file on the flash volume (see cow_store.hpp):
// opening it (creating a blank one for [ramdisk] learn), resolving it into
// the store's sector table, and the learn side - the image's trailer (the
// persisted learn state), the session/convergence rules, and the write-back
// of a 512 B sector into the file through flash_fs.
//
// Blank image: the file's clusters are allocated (f_expand) and their flash
// pages trimmed, so every sector is a hole: no page, reads as 0x00 through
// FatFS, and served from one shared 512 B zero page in flash (the table
// entry zero_entry). Creating 512 KB this way writes a handful of FAT and
// directory sectors plus one map sync instead of 1,024 pages (128 erases).
//
// Trailer: one 512 B sector right after the disk's `size` bytes (the file is
// size + 512). Present = the image is a learn image. Layout: the CowTrailer
// header (64 B), the store's three hot-block bitmaps (sess, seen, vol,
// COW_HOT_BYTES each), a checksum over the rest in the last 4 bytes. It is
// rewritten with every learn map sync, so what the map holds and what the
// trailer says about the session always match - in particular the current
// session's write-backs (cur_*), which close the session at the next
// power-up when no MZ-800 reset did.
//
// Pure FatFS + flash_fs.h: tests/cow_ramdisk runs it on the host on the
// real flash_fs over an emulated flash chip.

#include <stdint.h>
#include <stddef.h>
#include "ff.h"
#include "cow_store.hpp"
#include "cow_resolve.hpp"

enum CowLearnMode : uint8_t {
    COW_LEARN_OFF = 0,        // plain cow: the image is never written
    COW_LEARN_LEARNING = 1,   // write-back on, not converged yet
    COW_LEARN_OK = 2,         // converged; write-back still on (a new version relearns)
    COW_LEARN_FROZEN = 3,     // write-back off (REST freeze, or learn_freeze_after)
};

struct CowTrailer {
    char     magic[8];        // "MZPCOWL2" ("MZPCOWL1": 15d9e36's, read and upgraded)
    uint32_t disk_size;
    uint8_t  mode;            // CowLearnMode (never OFF on flash)
    uint8_t  frozen_auto;     // frozen by learn_freeze_after, not by the user
    uint16_t clean;           // consecutive sessions without (non-volatile) write-back
    uint16_t dirty;           // consecutive sessions with write-back
    uint16_t relearns;
    uint32_t sessions;        // sessions with RAM-disk activity, ever
    uint32_t wb_blocks;       // blocks written back, ever
    uint32_t wb_sectors;      // sectors written back, ever
    uint32_t erases;          // flash erases caused by the learn path, ever
    uint32_t created;         // creations / relearns from blank
    uint32_t l1_sum;          // MZPCOWL1's checksum (0 in v2: the sector's last word)
    uint8_t  cur_active;      // the current session is open (closed at the next power-up)
    uint8_t  pad0;
    uint16_t cur_wb;          // ... its non-volatile write-backs so far
    uint16_t cur_vol;         // ... its volatile write-backs so far
    uint16_t pad1;
    uint8_t  reserved[12];
};
static_assert(sizeof(CowTrailer) == 64, "the trailer header is 64 B");

struct CowLearnPolicy {
    uint16_t converge = 2;        // clean sessions: learning -> ok
    uint16_t freeze_after = 0;    // consecutive dirty sessions -> frozen (0 = never)
};

// A session ended (MZ-800 reset). active: the Z80 addressed the disk.
// Returns true when the trailer changed (to be persisted).
bool cow_learn_end_session(CowTrailer& t, const CowLearnPolicy& p, bool active,
                           uint32_t wb_blocks, uint32_t wb_sectors, uint32_t drops,
                           uint32_t erases);
void cow_trailer_init(CowTrailer& t, uint32_t disk_size);
bool cow_trailer_valid(const uint8_t* sector, uint32_t disk_size);   // a 512 B trailer sector
const char* cow_learn_mode_name(uint8_t mode);

struct CowImageCfg {
    CowLearnPolicy policy;    // also applied when a power-up closes the last session
    const char* path;
    uint32_t size;            // bytes, multiple of 64 KB; 0 = the image's (whole 64 KB pages)
    uint32_t default_size;    // creating with size 0
    uint32_t pool_blocks;
    int      learn;           // -1 auto, 0 off, 1 on
    const uint8_t* flash_base;    // XIP address of flash page 0 of the table's entries
    const uint8_t* zero_page;     // 512 zero bytes, 512-aligned, inside the XIP window
    CowBusy* busy;
};

class CowImage {
public:
    // Opens (creating if needed) and resolves the image into store. 0, or
    // -1 with the reason in err. Creation time in createUs().
    int open(const CowImageCfg& cfg, CowStore& store, char* err, size_t err_len,
             uint64_t (*now_us)(void*), void* now_ctx);
    void close();

    // ---- learn ----
    bool learning() const { return mode_ == COW_LEARN_LEARNING || mode_ == COW_LEARN_OK; }
    uint8_t mode() const { return mode_; }
    bool created() const { return created_; }
    bool gone() const { return gone_; }
    uint32_t createUs() const { return create_us_; }
    const CowTrailer& trailer() const { return tr_; }
    bool hasTrailer() const { return has_trailer_; }
    uint32_t erases() const { return erases_; }        // learn path, since power-up
    uint32_t sessionErases() const { return erases_ - session_erases0_; }   // not yet in the trailer
    uint32_t trailerWrites() const { return trailer_writes_; }
    uint32_t trailerFails() const { return trailer_fails_; }
    uint16_t zeroEntry() const { return zero_entry_; }
    uint32_t pageBase() const { return base_; }
    const CowRuns& runs() const { return runs_; }

    CowLearnIo io();                 // for CowStore::setLearn
    // MZ-800 reset (core 1, Z80 in reset): the session accounting, in RAM
    // only - no flash operation and no EXWAIT at a reset (a reset path that
    // waits on flash collides with the reset escape hatch). What must reach
    // flash (the trailer, the map) waits for persist().
    void endSession(CowStore& store, const CowLearnPolicy& p, bool active);
    void freeze(CowStore& store, bool frozen);
    // Back to a blank image (the disk's sectors trimmed: map edits only),
    // learning, pool empty; persisted by persist()
    bool relearn(CowStore& store);
    // Deferred flash work: true when persist() has something to write
    // (A learning session is marked open in the trailer at its first access,
    // so a power-off can close it at the next power-up.)
    bool persistPending(const CowStore& store) const {
        return mode_ != COW_LEARN_OFF && !gone_ && has_trailer_ &&
               (pending_write_ || store.syncPending() ||
                (mode_ == COW_LEARN_LEARNING && !session_marked_));
    }
    // The trailer if it changed, then the map sync: one page program + one
    // or two map syncs, ~50-150 ms. Called by the owner from a RAM-disk
    // port access (core 1, the Z80 held by EXWAIT), like a write-back.
    void persist(CowStore& store);
    uint32_t volatileCount(const CowStore& store) const { return store.volatileCount(); }

    // flash_fs hooks (installed by the owner, both modes): page moves, and
    // trims (FatFS CTRL_TRIM: the image deleted or truncated). An external
    // trim keeps the power-up snapshot like a rewrite (remap() to the zero
    // page pins the blocks) and stops the write-back for good: the clusters
    // may already belong to another file.
    void onRemap(CowStore& store, uint16_t old_fs, uint16_t new_fs) {
        store.remap((uint16_t)(base_ + old_fs), (uint16_t)(base_ + new_fs));
    }
    void onTrim(CowStore& store, uint32_t first, uint32_t count);

private:
    static uint16_t ioWrite(void* ctx, uint32_t sector, const uint8_t* data);
    static void ioSync(void* ctx);
    static uint64_t ioNow(void* ctx);
    bool writeTrailer();
    bool syncWithTrailer();         // the trailer (with the session record), then the map
    void trimDisk(CowStore* store);

    CowRuns runs_;
    CowTrailer tr_{};
    bool has_trailer_ = false;
    bool created_ = false;
    bool gone_ = false;
    bool self_trim_ = false;
    uint8_t mode_ = COW_LEARN_OFF;
    uint32_t nsect_ = 0;
    uint32_t base_ = 0;             // table entry of flash_fs page 0
    uint16_t zero_entry_ = 0;
    const uint8_t* fs_xip_ = nullptr;
    uint32_t create_us_ = 0;
    uint32_t erases_ = 0;
    uint32_t trailer_writes_ = 0;
    uint32_t trailer_fails_ = 0;
    uint64_t (*now_us_)(void*) = nullptr;
    void* now_ctx_ = nullptr;
    bool pending_write_ = false;        // the trailer changed: persist()
    bool session_marked_ = false;       // the trailer says this session is open
    CowStore* store_ = nullptr;
    uint32_t session_sectors0_ = 0, session_erases0_ = 0;
};
