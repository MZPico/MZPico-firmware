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
#include "ff.h"

// map_entry(lba) -> flash page index of that FAT sector, 0 = no page.
// max_entry: entries at or above it are rejected (outside the region).
// Resolves the first nsect 512-byte sectors of the open file fp into
// out[0..nsect). Returns 0, or -1 with a reason in err.
int cow_resolve_file(FIL* fp, uint32_t nsect, uint16_t (*map_entry)(uint32_t lba),
                     uint32_t max_entry, uint16_t* out, char* err, size_t err_len);
