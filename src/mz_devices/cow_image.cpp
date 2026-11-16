// See cow_image.hpp
#include "cow_image.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash_fs.h"

static const char COW_TRAILER_MAGIC[8] = {'M', 'Z', 'P', 'C', 'O', 'W', 'L', '1'};

static uint32_t trailer_sum(const CowTrailer& t) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&t);
    uint32_t s = 0x5A17C0DEu;
    for (size_t i = 0; i < offsetof(CowTrailer, sum); i++) s = (s << 5) + s + p[i];
    return s;
}

void cow_trailer_init(CowTrailer& t, uint32_t disk_size) {
    memset(&t, 0, sizeof(t));
    memcpy(t.magic, COW_TRAILER_MAGIC, 8);
    t.disk_size = disk_size;
    t.mode = COW_LEARN_LEARNING;
}

bool cow_trailer_valid(const CowTrailer& t, uint32_t disk_size) {
    return memcmp(t.magic, COW_TRAILER_MAGIC, 8) == 0 && t.disk_size == disk_size &&
           t.mode >= COW_LEARN_LEARNING && t.mode <= COW_LEARN_FROZEN && t.sum == trailer_sum(t);
}

const char* cow_learn_mode_name(uint8_t mode) {
    switch (mode) {
    case COW_LEARN_LEARNING: return "learning";
    case COW_LEARN_OK: return "ok";
    case COW_LEARN_FROZEN: return "frozen";
    default: return "off";
    }
}

bool cow_learn_end_session(CowTrailer& t, const CowLearnPolicy& p, bool active,
                           uint32_t wb_blocks, uint32_t wb_sectors, uint32_t drops,
                           uint32_t erases) {
    if (!active && !wb_blocks) return false;
    const uint8_t mode0 = t.mode;
    const uint16_t clean0 = t.clean, dirty0 = t.dirty;
    t.sessions++;
    t.wb_blocks += wb_blocks;
    t.wb_sectors += wb_sectors;
    t.erases += erases;
    if (t.mode == COW_LEARN_FROZEN) return wb_blocks || erases;
    if (wb_blocks || drops) {
        // Written back (or wanted to and could not): still learning
        t.clean = 0;
        if (t.dirty < 0xFFFF) t.dirty++;
        t.mode = COW_LEARN_LEARNING;
        if (p.freeze_after && t.dirty >= p.freeze_after) {
            t.mode = COW_LEARN_FROZEN;
            t.frozen_auto = 1;
        }
    } else {
        t.dirty = 0;
        if (t.clean < p.converge) t.clean++;
        if (t.mode == COW_LEARN_LEARNING && t.clean >= p.converge) t.mode = COW_LEARN_OK;
    }
    // A clean session of a converged image changes nothing worth a flash write
    return t.mode != mode0 || t.clean != clean0 || t.dirty != dirty0 || wb_blocks || erases;
}

// ---- CowImage ----

// FatFS write of the trailer at offset size (extends the file if short)
static bool fatfs_write_trailer(const char* path, uint32_t size, const CowTrailer& t) {
    FIL* f = static_cast<FIL*>(malloc(sizeof(FIL)));
    uint8_t* b = static_cast<uint8_t*>(calloc(1, COW_SECT));
    bool ok = false;
    if (f && b && f_open(f, path, FA_WRITE | FA_OPEN_EXISTING) == FR_OK) {
        CowTrailer tt = t;
        tt.sum = trailer_sum(tt);
        memcpy(b, &tt, sizeof(tt));
        UINT w = 0;
        ok = f_lseek(f, size) == FR_OK && f_write(f, b, COW_SECT, &w) == FR_OK && w == COW_SECT;
        ok = (f_close(f) == FR_OK) && ok;
    }
    free(f);
    free(b);
    return ok;
}

// Free flash pages for the learning ahead: flash_fs keeps a deleted file's
// pages until its sectors are rewritten unless FatFS trims them, and files
// deleted before TRIM (or over USB, where the host's FAT driver writes the
// volume directly) never were. Allocating every free cluster to a scratch
// file (an extension by seek: FAT sectors only, no data) and deleting it
// trims them all.
static void trim_free_space(const char* image_path) {
    FATFS* fs = nullptr;
    DWORD nfree = 0;
    char drive[8] = "flash:";
    const char* colon = strchr(image_path, ':');
    if (colon && (size_t)(colon - image_path) < sizeof(drive) - 1) {
        memcpy(drive, image_path, (size_t)(colon - image_path) + 1);
        drive[colon - image_path + 1] = '\0';
    }
    if (f_getfree(drive, &nfree, &fs) != FR_OK || nfree == 0) return;
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%s/COWTRIM.TMP", drive);
    FIL* f = static_cast<FIL*>(malloc(sizeof(FIL)));
    if (!f) return;
    if (f_open(f, tmp, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        f_lseek(f, (FSIZE_t)nfree * fs->csize * 512u);
        f_close(f);
        f_unlink(tmp);
    }
    free(f);
}

// A blank image: size bytes of holes plus the trailer
static bool create_blank(const char* path, uint32_t size, const CowTrailer& t, char* err, size_t err_len) {
    FIL* f = static_cast<FIL*>(malloc(sizeof(FIL)));
    if (!f) { snprintf(err, err_len, "out of RAM"); return false; }
    f_unlink(path);                        // an old (short) image: trimmed too
    trim_free_space(path);
    FRESULT fr = f_open(f, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        free(f);
        snprintf(err, err_len, "cannot create the image (FatFS %d)", (int)fr);
        return false;
    }
    const FSIZE_t total = (FSIZE_t)size + COW_SECT;
    fr = f_expand(f, total, 1);            // contiguous: one run
    if (fr == FR_DENIED) {                 // no contiguous space: any clusters
        fr = f_lseek(f, total);
        if (fr == FR_OK && f_tell(f) != total) fr = FR_DENIED;
    }
    const FRESULT fc = f_close(f);
    free(f);
    if (fr != FR_OK || fc != FR_OK) {
        f_unlink(path);
        snprintf(err, err_len, "no room for a %lu KB image on flash:", (unsigned long)(total >> 10));
        return false;
    }
    if (!fatfs_write_trailer(path, size, t)) {
        f_unlink(path);
        snprintf(err, err_len, "cannot write the image trailer");
        return false;
    }
    return true;
}

int CowImage::open(const CowImageCfg& cfg, CowStore& store, char* err, size_t err_len,
                   uint32_t (*now_us)(void*), void* now_ctx) {
    close();
    err[0] = '\0';
    now_us_ = now_us;
    now_ctx_ = now_ctx;
    fs_xip_ = flash_fs_xip_base();
    const uintptr_t zoff = (uintptr_t)(cfg.zero_page - cfg.flash_base);
    const uintptr_t boff = (uintptr_t)(fs_xip_ - cfg.flash_base);
    if ((zoff & (COW_SECT - 1)) || (boff & (COW_SECT - 1)) || (zoff >> COW_SECT_SHIFT) >= 0xFFFF ||
        (boff >> COW_SECT_SHIFT) + flash_fs_num_pages() > 0x10000) {
        snprintf(err, err_len, "flash layout unsupported");
        return -1;
    }
    zero_entry_ = (uint16_t)(zoff >> COW_SECT_SHIFT);
    base_ = (uint32_t)(boff >> COW_SECT_SHIFT);

    FILINFO fno;
    const bool exists = f_stat(cfg.path, &fno) == FR_OK;
    const uint32_t fsize0 = exists ? (uint32_t)fno.fsize : 0;
    uint32_t size = cfg.size ? cfg.size : (exists ? (fsize0 & 0xffff0000u) : cfg.default_size);
    if (!size) { snprintf(err, err_len, "image shorter than one 64 KB page"); return -1; }
    nsect_ = size >> COW_SECT_SHIFT;

    // Missing: a blank learn image (unless learn=false). Short: the same
    // only for a learn image (its trailer at its end: a larger size= was
    // set) or with learn=true - a short image prepared elsewhere is never
    // thrown away silently
    bool short_learn = false;
    if (exists && fsize0 < size && fsize0 > COW_SECT && cfg.learn != 0) {
        FIL* f = static_cast<FIL*>(malloc(sizeof(FIL)));
        CowTrailer t;
        UINT got = 0;
        if (f && f_open(f, cfg.path, FA_READ) == FR_OK) {
            short_learn = f_lseek(f, fsize0 - COW_SECT) == FR_OK &&
                          f_read(f, &t, sizeof(t), &got) == FR_OK && got == sizeof(t) &&
                          cow_trailer_valid(t, fsize0 - COW_SECT);
            f_close(f);
        }
        free(f);
    }
    if (!exists || fsize0 < size) {
        if (exists && cfg.learn == -1 && !short_learn) {
            snprintf(err, err_len, "image %lu B < size %lu B (learn=true recreates it blank)",
                     (unsigned long)fsize0, (unsigned long)size);
            return -1;
        }
        if (cfg.learn == 0) {
            if (exists) snprintf(err, err_len, "image %lu B < size %lu B",
                                 (unsigned long)fsize0, (unsigned long)size);
            else snprintf(err, err_len, "cannot open the image");
            return -1;
        }
        const uint32_t t0 = now_us_ ? now_us_(now_ctx_) : 0;
        CowTrailer t;
        cow_trailer_init(t, size);
        t.created = 1;
        if (!create_blank(cfg.path, size, t, err, err_len)) return -1;
        created_ = true;
        create_us_ = now_us_ ? now_us_(now_ctx_) - t0 : 0;
    }

    FIL* f = static_cast<FIL*>(malloc(sizeof(FIL)));
    uint8_t* chk = static_cast<uint8_t*>(malloc(COW_SECT));
    if (!f || !chk) { free(f); free(chk); snprintf(err, err_len, "out of RAM"); return -1; }
    int ret = -1;
    if (f_open(f, cfg.path, FA_READ) != FR_OK) {
        snprintf(err, err_len, "cannot open the image");
        free(f); free(chk);
        return -1;
    }
    const uint32_t fsize = (uint32_t)f_size(f);
    // The trailer: a learn image
    has_trailer_ = false;
    if (fsize >= size + COW_SECT) {
        UINT got = 0;
        if (f_lseek(f, size) == FR_OK && f_read(f, chk, COW_SECT, &got) == FR_OK && got == COW_SECT) {
            memcpy(&tr_, chk, sizeof(tr_));
            has_trailer_ = cow_trailer_valid(tr_, size);
        }
    }
    // learn: auto = a learn image (created now or earlier); on/off as given
    if (cfg.learn == 0) mode_ = COW_LEARN_OFF;
    else if (has_trailer_) mode_ = tr_.mode;
    else if (cfg.learn == 1) {
        // An image prepared elsewhere, learn=true: add the trailer
        cow_trailer_init(tr_, size);
        f_close(f);
        if (fatfs_write_trailer(cfg.path, size, tr_)) has_trailer_ = true;
        if (f_open(f, cfg.path, FA_READ) != FR_OK) {
            snprintf(err, err_len, "cannot reopen the image");
            free(f); free(chk);
            return -1;
        }
        mode_ = COW_LEARN_LEARNING;     // without a trailer: in RAM only
    } else mode_ = COW_LEARN_OFF;

    if (!store.setup(size, cfg.pool_blocks, cfg.flash_base, cfg.busy)) {
        snprintf(err, err_len, "out of RAM (pool %lu KB)", (unsigned long)cfg.pool_blocks);
    } else if (cow_resolve_file(f, store.sectorCount(), flash_fs_map_entry, flash_fs_num_pages(),
                                base_, zero_entry_, store.sectorTable(), &runs_, err, err_len) == 0) {
        if (created_) trimDisk(&store);
        // Spot check the table against FatFS's own reads: every 16th
        // sector and the last (cheap enough for the cold-boot budget)
        const uint32_t n = store.sectorCount();
        for (uint32_t i = 0; !err[0]; i = (i + 16 < n) ? i + 16 : n - 1) {
            UINT got = 0;
            if (f_lseek(f, (FSIZE_t)i * COW_SECT) != FR_OK ||
                f_read(f, chk, COW_SECT, &got) != FR_OK || got != COW_SECT) {
                snprintf(err, err_len, "read error at sector %lu", (unsigned long)i);
                break;
            }
            const uint8_t* x = cfg.flash_base + ((uint32_t)store.sectorTable()[i] << COW_SECT_SHIFT);
            if (memcmp(chk, x, COW_SECT) != 0)
                snprintf(err, err_len, "table check failed at sector %lu", (unsigned long)i);
            if (i == n - 1) break;
        }
        if (!err[0]) ret = 0;
    }
    f_close(f);
    free(f);
    free(chk);
    if (ret) { store.release(); runs_.release(); }
    return ret;
}

void CowImage::close() {
    runs_.release();
    has_trailer_ = created_ = gone_ = self_trim_ = false;
    mode_ = COW_LEARN_OFF;
    erases_ = trailer_writes_ = create_us_ = 0;
    pending_write_ = false;
    session_sectors0_ = session_erases0_ = 0;
}

// The disk's sectors (not the trailer) become holes: zeros, no pages
void CowImage::trimDisk(CowStore* store) {
    self_trim_ = true;
    uint32_t done = 0;
    for (uint32_t k = 0; k < runs_.n && done < nsect_; k++) {
        uint32_t c = runs_.run[k].count;
        if (c > nsect_ - done) c = nsect_ - done;
        flash_fs_trim_FAT_sectors(runs_.run[k].lba, c);
        done += c;
    }
    self_trim_ = false;
    if (store)
        for (uint32_t i = 0; i < nsect_; i++) store->setSector(i, zero_entry_);
}

// Called before flash_fs drops the pages: they still hold the content
void CowImage::onTrim(CowStore& store, uint32_t first, uint32_t count) {
    if (self_trim_) return;             // trimDisk() sets the table itself
    bool hit = false;
    for (uint32_t k = 0; k < runs_.n; k++) {
        const uint32_t a = runs_.run[k].lba, n = runs_.run[k].count;
        const uint32_t lo = first > a ? first : a;
        const uint32_t hi = (first + count < a + n) ? first + count : a + n;
        for (uint32_t l = lo; l < hi; l++) {
            hit = true;
            const uint16_t e = flash_fs_map_entry(l);
            if (e) store.remap((uint16_t)(base_ + e), zero_entry_);   // preserved (pinned) or lost
        }
    }
    if (hit) {
        // The image file was deleted or truncated under the running disk:
        // the write-back must never write into clusters that may already
        // belong to another file
        gone_ = true;
        store.setWriteBack(false);
    }
}

CowLearnIo CowImage::io() {
    CowLearnIo io{};
    io.write_sector = ioWrite;
    io.sync = ioSync;
    io.now_us = ioNow;
    io.ctx = this;
    return io;
}

uint16_t CowImage::ioWrite(void* ctx, uint32_t sector, const uint8_t* data) {
    CowImage* self = static_cast<CowImage*>(ctx);
    if (self->gone_ || sector >= self->nsect_) return 0;
    const uint32_t lba = self->runs_.lba(sector);
    if (lba == UINT32_MAX || lba > 0xFFFF) return 0;
    const uint32_t e0 = flash_fs_erases;
    const bool ok = flash_fs_write_FAT_sector((uint16_t)lba, data);
    self->erases_ += flash_fs_erases - e0;
    if (!ok) return 0;
    const uint16_t e = flash_fs_map_entry(lba);
    if (!e || memcmp(self->fs_xip_ + ((uint32_t)e << COW_SECT_SHIFT), data, COW_SECT) != 0) return 0;
    return (uint16_t)(self->base_ + e);
}

void CowImage::ioSync(void* ctx) {
    CowImage* self = static_cast<CowImage*>(ctx);
    const uint32_t e0 = flash_fs_erases;
    flash_fs_sync();
    self->erases_ += flash_fs_erases - e0;
}

uint32_t CowImage::ioNow(void* ctx) {
    CowImage* self = static_cast<CowImage*>(ctx);
    return self->now_us_ ? self->now_us_(self->now_ctx_) : 0;
}

bool CowImage::writeTrailer() {
    if (!has_trailer_ || gone_) return false;
    const uint32_t lba = runs_.lba(nsect_);
    if (lba == UINT32_MAX || lba > 0xFFFF) return false;
    uint8_t* b = static_cast<uint8_t*>(calloc(1, COW_SECT));
    if (!b) return false;
    tr_.sum = trailer_sum(tr_);
    memcpy(b, &tr_, sizeof(tr_));
    const uint32_t e0 = flash_fs_erases;
    const bool ok = flash_fs_write_FAT_sector((uint16_t)lba, b);
    erases_ += flash_fs_erases - e0;
    free(b);
    if (ok) trailer_writes_++;
    return ok;
}

bool CowImage::endSession(CowStore& store, const CowLearnPolicy& p, bool active) {
    const uint32_t drops = store.learn.session_drops;
    const uint32_t wb = store.endSession();            // syncs the map if written
    if (mode_ == COW_LEARN_OFF || !has_trailer_) {
        // learn without a trailer (it could not be added): RAM-only state
        if (mode_ != COW_LEARN_OFF) {
            cow_learn_end_session(tr_, p, active, wb, 0, drops, 0);
            mode_ = tr_.mode;
            store.setWriteBack(learning() && !gone_);
        }
        return wb != 0;
    }
    const uint32_t secs = store.learn.sectors - session_sectors0_;
    session_sectors0_ = store.learn.sectors;
    bool changed = cow_learn_end_session(tr_, p, active, wb, secs, drops, erases_ - session_erases0_);
    mode_ = tr_.mode;
    store.setWriteBack(learning() && !gone_);
    if (!(changed || pending_write_)) {
        session_erases0_ = erases_;
        return wb != 0;
    }
    pending_write_ = false;
    const bool ok = writeTrailer();
    ioSync(this);
    session_erases0_ = erases_;
    return ok || wb;
}

void CowImage::freeze(CowStore& store, bool frozen) {
    if (mode_ == COW_LEARN_OFF) return;
    if (frozen) {
        tr_.mode = COW_LEARN_FROZEN;
        tr_.frozen_auto = 0;
    } else if (tr_.mode == COW_LEARN_FROZEN) {
        tr_.mode = COW_LEARN_LEARNING;
        tr_.frozen_auto = 0;
        tr_.clean = tr_.dirty = 0;
    }
    mode_ = tr_.mode;
    store.setWriteBack(learning() && !gone_);
    pending_write_ = true;      // persisted at the next MZ-800 reset
}

bool CowImage::relearn(CowStore& store) {
    if (mode_ == COW_LEARN_OFF || gone_) return false;
    store.clear(true);
    trimDisk(&store);
    const CowTrailer old = tr_;
    cow_trailer_init(tr_, old.disk_size ? old.disk_size : nsect_ << COW_SECT_SHIFT);
    tr_.sessions = old.sessions;
    tr_.wb_blocks = old.wb_blocks;
    tr_.wb_sectors = old.wb_sectors;
    tr_.erases = old.erases;
    tr_.created = old.created + 1;
    tr_.relearns = (uint16_t)(old.relearns + 1);
    mode_ = tr_.mode;
    writeTrailer();
    ioSync(this);
    pending_write_ = false;
    store.setWriteBack(true);
    return true;
}
