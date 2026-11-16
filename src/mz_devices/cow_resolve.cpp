// See cow_resolve.hpp
#include "cow_resolve.hpp"

#include <stdio.h>
#include <stdlib.h>

int cow_resolve_file(FIL* fp, uint32_t nsect, uint16_t (*map_entry)(uint32_t lba),
                     uint32_t max_entry, uint16_t* out, char* err, size_t err_len) {
    static_assert(FF_MAX_SS == 512 && FF_MIN_SS == 512, "the table assumes 512-byte FAT sectors");
    FATFS* fs = fp->obj.fs;
    if ((uint64_t)f_size(fp) < (uint64_t)nsect * 512) {
        snprintf(err, err_len, "image %lu B, shorter than the disk", (unsigned long)f_size(fp));
        return -1;
    }
    // Cluster link map: [size, (run length, first cluster)..., 0]. Start
    // small, grow to the size FatFS reports when the file is fragmented.
    DWORD n = 32;
    DWORD* clmt = nullptr;
    FRESULT fr;
    for (int attempt = 0; attempt < 2; attempt++) {
        free(clmt);
        clmt = static_cast<DWORD*>(malloc(n * sizeof(DWORD)));
        if (!clmt) { snprintf(err, err_len, "no RAM for the cluster map"); return -1; }
        clmt[0] = n;
        fp->cltbl = clmt;
        fr = f_lseek(fp, CREATE_LINKMAP);
        fp->cltbl = nullptr;
        if (fr != FR_NOT_ENOUGH_CORE) break;
        n = clmt[0];
    }
    if (fr != FR_OK) {
        free(clmt);
        snprintf(err, err_len, "cluster map: FatFS error %d", (int)fr);
        return -1;
    }

    const uint32_t csize = fs->csize;
    uint32_t done = 0;
    for (const DWORD* p = clmt + 1; *p && done < nsect; p += 2) {
        const uint32_t run = p[0], first = p[1];
        for (uint32_t c = 0; c < run && done < nsect; c++) {
            const uint64_t lba = (uint64_t)fs->database + (uint64_t)csize * (first + c - 2);
            for (uint32_t k = 0; k < csize && done < nsect; k++) {
                const uint64_t sec = lba + k;
                const uint16_t e = (sec >> 32) ? 0 : map_entry((uint32_t)sec);
                if (e == 0 || e >= max_entry) {
                    free(clmt);
                    snprintf(err, err_len, "sector %lu (LBA %lu) %s", (unsigned long)done,
                             (unsigned long)sec, e ? "outside the flash region" : "has no flash page");
                    return -1;
                }
                out[done++] = e;
            }
        }
    }
    free(clmt);
    if (done < nsect) {
        snprintf(err, err_len, "cluster chain ends after %lu sectors", (unsigned long)done);
        return -1;
    }
    return 0;
}
