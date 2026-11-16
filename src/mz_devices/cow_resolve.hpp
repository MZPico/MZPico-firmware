#pragma once

// Image-file -> XIP page table for the COW RAM disk (see cow_store.hpp).
//
// FAT clusters (the file's chain, via FatFS's fast-seek cluster link map:
// FAT reads only, no data reads) -> FAT sectors (database + csize *
// (cluster - 2)) -> flash_fs map entries (page index from the start of the
// storage region; one 512-byte page per FAT sector, so each table entry is
// contiguous for its 512 bytes by construction).
//
// Pure FatFS + a map-lookup callback: tests/cow_ramdisk runs it on the
// host against a real FatFS volume.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "ff.h"

// The file's sectors as runs of consecutive FAT sectors (LBAs), in file
// order: what the learn write-back needs to address a sector it rewrites.
struct CowRun {
    uint32_t lba;      // first FAT sector of the run
    uint32_t count;    // sectors in the run
};

struct CowRuns {
    CowRun*  run = nullptr;   // malloc'd
    uint32_t n = 0;
    uint32_t sectors = 0;     // total sectors covered
    // LBA of the file's sector i (i < sectors), or UINT32_MAX
    uint32_t lba(uint32_t i) const {
        for (uint32_t k = 0; k < n; k++) {
            if (i < run[k].count) return run[k].lba + i;
            i -= run[k].count;
        }
        return UINT32_MAX;
    }
    // The file's sector index of an LBA, or UINT32_MAX
    uint32_t index(uint32_t lba) const {
        uint32_t base = 0;
        for (uint32_t k = 0; k < n; k++) {
            if (lba >= run[k].lba && lba - run[k].lba < run[k].count) return base + (lba - run[k].lba);
            base += run[k].count;
        }
        return UINT32_MAX;
    }
    void release() { free(run); run = nullptr; n = sectors = 0; }
};

// map_entry(lba) -> flash page index of that FAT sector, 0 = no page.
// max_entry: entries at or above it are rejected (outside the region).
// Resolves the first nsect 512-byte sectors of the open file fp into
// out[0..nsect): base + entry for a sector with a page; hole_entry for a
// sector never written or trimmed (it reads as zeros through FatFS), or -
// when hole_entry is 0 - the file is refused. runs (optional): filled with
// the file's whole cluster chain (every sector of the file, not only nsect).
// Returns 0, or -1 with a reason in err.
int cow_resolve_file(FIL* fp, uint32_t nsect, uint16_t (*map_entry)(uint32_t lba),
                     uint32_t max_entry, uint32_t base, uint16_t hole_entry,
                     uint16_t* out, CowRuns* runs, char* err, size_t err_len);
