/*
 * GPL notice preserved from original C implementation:
 *   (c) 2009 Michal Hucik <http://www.ordoz.com>
 *   (c) 2012 Bohumil Novacek <http://dzi.n.cz/8bit/>
 * Ported to C++ and integrated with MZDevice by <your-name>, 2025.
 */

#include <algorithm>
#include <cstdio>

#include "fdc.hpp"
#include "ff.h"
#include "common.hpp"
#include "file_source.hpp"
#include "fdc_dir_source.hpp"

REGISTER_MZ_DEVICE(FDCDevice)

// -------------------- Construction & registration --------------------

FDCDevice::FDCDevice() {
    for (uint8_t i = 0; i < FDC_WRITE_PORTS; ++i)
        writeMappings[i].fn   = FDCDevice::WriteThunk;
    for (uint8_t i = 0; i < FDC_READ_PORTS; ++i)
        readMappings[i].fn   = FDCDevice::ReadThunk;

    // Initialize port mappings with defaults
    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);
}

FDCDevice::~FDCDevice() {
    //for (auto& d : drive) {
     //   d.bs->flush();
        //if (d.fh.obj.fs) {
        //    f_sync(&d.fh);
        //    f_close(&d.fh);
        //}
    //}
}


// -------------------- MZDevice overrides --------------------

std::vector<uint8_t> FDCDevice::getReadPorts() const {
    std::vector<uint8_t> ports;
    for (uint8_t i = 0; i < FDC_READ_PORTS; ++i) {
        ports.push_back(FDC_DEFAULT_BASE_PORT + i);
    }
    return ports;
}

std::vector<uint8_t> FDCDevice::getWritePorts() const {
    std::vector<uint8_t> ports;
    for (uint8_t i = 0; i < FDC_WRITE_PORTS; ++i) {
        ports.push_back(FDC_DEFAULT_BASE_PORT + i);
    }
    return ports;
}

int FDCDevice::init() {
    fd0disabled = -1;
    return 1;
}

// Z80 reset: WD1793 controller state back to power-on; mounted images and
// per-drive write protection persist (they are configuration, and the
// physical head position is revalidated lazily by setTrack/seekToSector)
void FDCDevice::softReset() {
    regSTATUS = 0;
    regDATA = 0;
    regTRACK = 0;
    regSECTOR = 0;
    SIDE = 0;
    buffer_pos = 0;
    COMMAND = 0;
    MOTOR = 0;
    DENSITY = 0;
    EINT = 0;
    DATA_COUNTER = 0;
    MULTIBLOCK_RW = 0;
    STATUS_SCRIPT = 0;
    waitForInt = 0;
    write_track_stage = 0;
    write_track_counter = 0;
    rt_phase = 0;
    rt_sec_idx = 0;
    rt_remaining = 0;
    reading_status_counter = 0;
    error_int = 0;

    // Revert explorer-mounted images to the ini configuration, so a reset
    // leaves the boot order as configured (e.g. back to the menu) instead
    // of re-booting a floppy mounted on the fly
    for (int i = 0; i < FDC_NUM_DRIVES; i++) {
        if (cur_image[i] == cfg_image[i]) continue;
        if (cfg_image[i].empty()) {
            drive[i].bs.reset(); // flushes and closes the runtime image
            drive[i].dirsrc = nullptr;
            drive[i].TRACK = 0;
            drive[i].SECTOR = 0;
            drive[i].SIDE = 0;
            drive[i].track_offset = 0;
            drive[i].sector_size = 0;
            cur_image[i].clear();
        } else {
            setDriveContent(i, cfg_image[i].c_str());
        }
    }

    // Directory mounts that stay mounted: drop the dead session's in-flight
    // state (staged data of an unfinished save, uncommitted directory)
    for (int i = 0; i < FDC_NUM_DRIVES; i++) {
        if (drive[i].dirsrc)
            drive[i].dirsrc->sessionAbort();
    }
}

int FDCDevice::readConfig(dictionary *ini) {
    if (!ini) return -1;

    // write_protected sets the default for all drives; write_protected<N>
    // overrides it per drive (N = 1..4, matching image_disk<N>)
    const int wp_all = iniparser_getboolean(ini, (getDevID() + ":write_protected").c_str(), 0);
    for (int i = 0; i < FDC_NUM_DRIVES; i++) {
        // Directory mounts: fs_disk<N> picks the synthesized filesystem
        // ("basic" or "cpm"); default auto-detects from the dir contents
        const char* fs = iniparser_getstring(
            ini, (getDevID() + ":fs_disk" + std::to_string(i+1)).c_str(), "auto");
        if (fs[0] == 'b' || fs[0] == 'B')      drive[i].fs_cfg = 1;
        else if (fs[0] == 'c' || fs[0] == 'C') drive[i].fs_cfg = 2;
        else                                   drive[i].fs_cfg = 0;

        std::string image = iniparser_getstring(ini, (getDevID() + ":image_disk" + std::to_string(i+1)).c_str(), "");
        cfg_image[i] = image; // what a Z80 reset reverts the drive to
        if (!image.empty())
            setDriveContent(i, image.c_str());
        drive[i].wp = iniparser_getboolean(
            ini, (getDevID() + ":write_protected" + std::to_string(i+1)).c_str(), wp_all) ? 1 : 0;
    }
    return 0;
}

int FDCDevice::flush() {
    for (uint8_t i=0; i<FDC_NUM_DRIVES; i++) {
        if (!drive[i].bs)
            continue;
        drive[i].bs->flush();
    }
    
    return 0;
}

int FDCDevice::isInterrupt() {
    // Original behavior: if INT mode enabled and data is pending for
    bool pending = false;
    if (!EINT) return 0;
    if (error_int) return 1; // immediate command termination (e.g. WP reject)
    if (DATA_COUNTER) {
        const uint8_t t2 = (COMMAND >> 5);
        pending = (t2 == 0x03 /*READ*/ || t2 == 0x02 /*WRITE*/
                   || (COMMAND >> 4) == 0x03 /*READ ADDRESS*/
                   || (COMMAND >> 4) == 0x01 /*READ TRACK*/);
    }
    // WRITE TRACK paces the whole format stream on /INT, DATA_COUNTER or not
    if (COMMAND == 0x0f || COMMAND == 0x0b) pending = true;
    if (pending) {
        if (++waitForInt > 2) { // every ~3rd poll
            return 1;
        }
    }
    return 0;
}

// -------------------- Public helper --------------------

int FDCDevice::setDriveContent(uint8_t drive_id, const char* file_path) {
    if (drive_id >= FDC_NUM_DRIVES || !file_path) return -1;

    auto& d = drive[drive_id];
    d.bs.reset(); // flushes and closes the previous image, if any
    d.dirsrc = nullptr;
    d.TRACK = 0;
    d.SECTOR = 0;
    d.SIDE = 0;
    d.track_offset = 0;
    d.sector_size = 0;

    // A directory path mounts as a synthesized disk (BASIC or CP/M
    // filesystem, per fs_disk<N> or auto-detected from the contents)
    std::string path(file_path);
    while (path.size() > 1 && path.back() == '/' && path[path.size() - 2] != ':')
        path.pop_back();
    FILINFO fno{};
    if (f_stat(path.c_str(), &fno) == FR_OK && (fno.fattrib & AM_DIR)) {
        // One directory, one drive: two mounts would fight over the staging
        // temp file and commit conflicting models onto the same FAT dir
        for (int j = 0; j < FDC_NUM_DRIVES; j++) {
            if (j == drive_id || !drive[j].dirsrc) continue;
            std::string other = cur_image[j];
            while (other.size() > 1 && other.back() == '/' && other[other.size() - 2] != ':')
                other.pop_back();
            if (other == path) {
                printf("fdc: %s already dir-mounted on drive %d\n", path.c_str(), j + 1);
                return -1;
            }
        }
        const FDCDirSource::Fs fs =
            (d.fs_cfg == 1) ? FDCDirSource::Fs::BASIC :
            (d.fs_cfg == 2) ? FDCDirSource::Fs::CPM :
                              FDCDirSource::detectFs(path);
        if (ByteSourceFactory::from_fdcdir(path, fs, 512, d.bs) != 0) {
            d.bs.reset();
            // The constructor only fails on allocation failure, so this is
            // the out-of-RAM degrade path: drive stays empty, boot continues
            printf("fdc: dir mount %s failed: out of RAM\n", path.c_str());
            return -1;
        }
        d.dirsrc = static_cast<FDCDirSource*>(d.bs.get());
        printf("fdc: dir mount %s as %s\n", path.c_str(),
               fs == FDCDirSource::Fs::BASIC ? "basic" : "cpm");
    } else {
        // 512-byte cache: writes reach FatFS in whole FAT sectors, which
        // matters on flash where every partial write still costs a full
        // remapped page program
        if (ByteSourceFactory::from_file(file_path, 0, 512, /* wrap = */false, d.bs) != 0) {
            d.bs.reset();
            return -1;
        }
    }

    d.track_offset = getTrackOffset(drive_id, d.TRACK, d.SIDE);
    cur_image[drive_id] = file_path;
    return 1;
}

// -------------------- Static thunks --------------------

int FDCDevice::ReadThunk(MZDevice* dev, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    auto* self = static_cast<FDCDevice*>(dev);
    return self->fdcRead(port, dt, high_addr);
}

int FDCDevice::WriteThunk(MZDevice* dev, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* self = static_cast<FDCDevice*>(dev);
    return self->fdcWrite(port, dt, high_addr);
}

// -------------------- Private helpers (ported) --------------------

int32_t FDCDevice::getTrackOffset(uint8_t drive_id, uint8_t track, uint8_t side) {
    // Based on FDC_GetTrackOffset(): seek to 0x34, sum table bytes for (track*2+side),
    // if (drive_id >= FDC_NUM_DRIVES || !drive[drive_id].fh.obj.fs) return 0;
    if (drive_id >= FDC_NUM_DRIVES || !drive[drive_id].bs) return 0;

    uint32_t offset = 0;
    uint8_t  b = 0;
    uint32_t rlen = 0;

    const int32_t tbl = 0x34;
    if (drive[drive_id].bs->seek(tbl) != 0) return 0;

    for (uint8_t i = 0; i < static_cast<uint8_t>(track * 2 + side); ++i) {
        drive[drive_id].bs->get(&b, 1, rlen);
        if (rlen != 1) return 0;
        if (b == 0x00) return 0;
        if (i == 1 && b == 0x25) b = 0x11; // bugfix from original
        offset += b * 0x100u;
    }
    offset += 0x100u;
    return static_cast<int32_t>(offset);
}

uint8_t FDCDevice::seekToSector(uint8_t drive_id, uint8_t sector) {
    if (drive_id >= FDC_NUM_DRIVES || !drive[drive_id].bs) return 1;
    auto& d = drive[drive_id];
    d.sector_size = 0;

    const int32_t hdr = d.track_offset + 0x15;
    uint32_t rlen = 0;
    uint8_t sector_count = 0;

    if (d.bs->seek(hdr) != 0) return 1;
    d.bs->get(&sector_count, 1, rlen);
    if (rlen != 1) return 1;

    if (d.bs->seek(d.track_offset + 0x18) != 0) return 1;

    uint16_t acc = 0;
    uint8_t  desc[8];
    for (uint8_t i = 0; i < sector_count; ++i) {
        d.bs->get(desc, 8, rlen);
        if (rlen != 8) return 1;
        if (sector == desc[2]) {
            d.sector_size = static_cast<int16_t>(desc[3] * 0x100);
            break;
        }
        acc += desc[3] * 0x100u;
    }
    if (d.sector_size == 0) return 1;

    const int32_t data_pos = d.track_offset + acc + 0x100;
    if (d.bs->seek(data_pos) == 0) {
        d.SECTOR = sector;
        return 0;
    }
    d.SECTOR = 0;
    d.sector_size = 0;
    return 1;
}

uint8_t FDCDevice::setTrack() {
    const uint8_t drv = (MOTOR & 0x03);
    auto& d = drive[drv];

    if (d.TRACK != regTRACK || d.SIDE != SIDE) {
        d.SECTOR = 0;
        d.sector_size = 0;
        const int32_t off = getTrackOffset(drv, regTRACK, SIDE);
        if (!off) return 1;

        d.track_offset = off;
        d.TRACK = regTRACK;
        d.SIDE  = SIDE;
    }
    return 0;
}

// -------------------- Core I/O (ported main state machine) --------------------

int FDCDevice::fdcWrite(uint8_t port, uint8_t dt, uint8_t /*high_addr*/) {
    // Register offset relative to the configured base (write port 0)
    const uint8_t off = static_cast<uint8_t>(port - writeMappings[0].port) & 0x07;

    // Convenience aliases
    auto drvIdx = [&]() -> uint8_t   { return static_cast<uint8_t>(MOTOR & 0x03); };
    auto curDrv = [&]() -> FDDrive& { return drive[MOTOR & 0x03]; };

    switch (off) {
    case 0: { // COMMAND / STATUS register write: process command
        if (waitForInt || error_int) { waitForInt = 0; error_int = 0; release_interrupt(); } // drop /INT on write to cmd
        COMMAND = dt;
        reading_status_counter = 0;

        // ---- Type I (seek family) ----
        if (COMMAND & 0x80) {
            regSTATUS = 0x00;
            if (!curDrv().bs) { regSTATUS = 0x80; return 1; } // not ready

            const uint8_t top_nibble = static_cast<uint8_t>(COMMAND >> 4);
            if (top_nibble == 0x0F) {          // RESTORE
                regTRACK = 0; SIDE = 0;
            } else if (top_nibble == 0x0E) {   // SEEK (to regDATA)
                regTRACK = regDATA;
            } else if ((COMMAND >> 5) == 0x05) { // STEP IN
                ++regTRACK; STATUS_SCRIPT = 1;
            } else if ((COMMAND >> 5) == 0x04) { // STEP OUT
                if (regTRACK) --regTRACK;
            }
            COMMAND = 0x00; DATA_COUNTER = 0; buffer_pos = 0;
            if (regTRACK == 0) regSTATUS |= 0x04; // TRK00
            // NB: write-protect is deliberately NOT reported in Type I status;
            // the Sharp ROM expects clean status here (matches mz800emu and
            // the original unicard). WP surfaces on write-command rejection.
            STATUS_SCRIPT = 1;                   // one BUSY, next READY
            return 0;
        }

        // ---- Type II (read/write sector) ----
        if ((COMMAND >> 6) == 0x01) {
            regSTATUS = 0; DATA_COUNTER = 0; buffer_pos = 0; STATUS_SCRIPT = 1;
            if (!curDrv().bs) { regSTATUS = 0x80; return 1; } // not ready

            // READ SECTOR (t2 == 3) or WRITE SECTOR (t2 == 2)
            const uint8_t t2 = static_cast<uint8_t>(COMMAND >> 5);
            if (t2 == 0x02 && isProtected(curDrv())) {
                regSTATUS = 0x40; COMMAND = 0x00; return 1; // write protect
            }

            MULTIBLOCK_RW = (COMMAND & 0x10) ? 0 : 1; // original inverted meaning
            if (setTrack()) { STATUS_SCRIPT = 3; return 1; }

            // sector select and seek
            if (seekToSector(drvIdx(), regSECTOR)) { STATUS_SCRIPT = 3; return 1; }

            if (t2 == 0x03) {
                // Preload first chunk into buffer
                uint16_t chunk = (curDrv().sector_size < sizeof(buffer))
                                  ? curDrv().sector_size
                                  : sizeof(buffer);
                uint32_t rlen = 0;
                curDrv().bs->get(buffer, chunk, rlen);
                if (rlen != chunk) { COMMAND = 0x00; regSTATUS = 0x08; return 1; }
            }
            DATA_COUNTER = curDrv().sector_size;
            regSTATUS |= 0x01; // BUSY
            regSTATUS |= 0x02; // DRQ
            return 0;
        }

        // ---- Type III: READ ADDRESS (0x3F) ----
        if ((COMMAND >> 4) == 0x03) {
            if (!curDrv().bs) { regSTATUS = 0x80; return 1; } // not ready
            if (setTrack()) return 1;
            if (!curDrv().SECTOR || !curDrv().sector_size) {
                if (seekToSector(drvIdx(), 1)) return 1;
            }
            regSECTOR   = curDrv().SECTOR;
            buffer[0]   = curDrv().TRACK;
            buffer[1]   = curDrv().SECTOR;
            buffer[2]   = curDrv().SIDE;
            buffer[3]   = static_cast<uint8_t>(curDrv().sector_size / 0x100);
            buffer[4] = buffer[5] = 0;
            DATA_COUNTER = 6;
            buffer_pos = 0;
            regSTATUS = 0x00;
            regSTATUS |= 0x01; // BUSY
            regSTATUS |= 0x02; // DRQ
            STATUS_SCRIPT = 1;
            return 0;
        }

        // ---- Type III: READ TRACK (real 0xE0-0xEF -> 0x10-0x1F) ----
        // Streams a synthetic track image (mz800emu layout: index mark, then
        // per sector 0xFE, C, H, R, N, 0xFB, data — no gaps, no CRC bytes);
        // CP/M uses this for the post-format verify pass.
        if ((COMMAND >> 4) == 0x01) {
            DATA_COUNTER = 0; buffer_pos = 0; STATUS_SCRIPT = 1;
            if (!curDrv().bs) { regSTATUS = 0x80; return 1; } // not ready
            if (setTrack()) { regSTATUS = 0x10; COMMAND = 0x00; return 1; } // RNF

            auto& d = curDrv();
            uint32_t rlen = 0;
            uint8_t nsec = 0;
            if (d.bs->seek(d.track_offset + 0x15) != 0) { regSTATUS = 0x10; COMMAND = 0x00; return 1; }
            d.bs->get(&nsec, 1, rlen);
            if (rlen != 1 || !nsec || nsec > 29) { regSTATUS = 0x10; COMMAND = 0x00; return 1; }

            // Collect the sector IDs (and size code) from the descriptors
            if (d.bs->seek(d.track_offset + 0x18) != 0) { regSTATUS = 0x10; COMMAND = 0x00; return 1; }
            uint8_t size_code = 0;
            for (uint8_t i = 0; i < nsec; ++i) {
                uint8_t desc[8];
                d.bs->get(desc, 8, rlen);
                if (rlen != 8) { regSTATUS = 0x10; COMMAND = 0x00; return 1; }
                buffer[8 + i] = desc[2];
                size_code = desc[3]; // uniform per track on MZ formats
            }
            buffer[4] = size_code;
            buffer[5] = nsec;

            const uint32_t total =
                1u + static_cast<uint32_t>(nsec) * (6u + static_cast<uint32_t>(size_code) * 0x100u);
            if (!size_code || total > 0xFFFF) { regSTATUS = 0x10; COMMAND = 0x00; return 1; }

            // Sector data streams sequentially from the track's data area
            if (d.bs->seek(d.track_offset + 0x100) != 0) { regSTATUS = 0x10; COMMAND = 0x00; return 1; }

            rt_phase = 0;
            rt_sec_idx = 0;
            rt_remaining = 0;
            DATA_COUNTER = static_cast<uint16_t>(total);
            regSTATUS = 0x03; // BUSY | DRQ
            return 0;
        }

        // ---- Type III: WRITE TRACK / format (real 0xF0/0xF4 -> 0x0f/0x0b) ----
        if (COMMAND == 0x0f || COMMAND == 0x0b) {
            // Rejections terminate immediately: one BUSY status pulse for
            // pollers, and a completion /INT for INT-paced format routines
            // (mz800emu asserts INTRQ here too) — without the /INT a routine
            // waiting for the first data-request interrupt hangs forever.
            STATUS_SCRIPT = 1;
            if (!curDrv().bs) { // not ready
                regSTATUS = 0x80; COMMAND = 0x00; error_int = 1; return 1;
            }
            if (isProtected(curDrv())) { // write protect
                regSTATUS = 0x40; COMMAND = 0x00; error_int = 1; return 1;
            }
            write_track_stage = 0;
            write_track_counter = 0;
            DATA_COUNTER = 0;
            buffer_pos = 0;
            regSTATUS = 0x03; // BUSY | DRQ
            STATUS_SCRIPT = 1;
            return 0;
        }

        // ---- Type IV: INTERRUPT (0x27/0x2f) ----
        if (COMMAND == 0x27 || COMMAND == 0x2f) {
            DATA_COUNTER = 0; buffer_pos = 0; COMMAND = 0x00; regSTATUS = 0x00; STATUS_SCRIPT = 0;
            return 0;
        }

        // Unknown command -> ignore gracefully
        return 0;
    }

    case 1: // TRACK register (note: 0xFF is ignored per original quirk)
        if (dt != 0xFF) regTRACK = static_cast<uint8_t>(~dt);
        return 0;

    case 2: // SECTOR register
        regSECTOR = static_cast<uint8_t>(~dt);
        return 0;

    case 3: { // DATA register (data byte, format stream, or regDATA staging)
        if (waitForInt || error_int) { waitForInt = 0; error_int = 0; release_interrupt(); }

        // WRITE TRACK (format) consumes the raw byte stream
        if (COMMAND == 0x0f || COMMAND == 0x0b)
            return writeTrackByte(dt);

        // Only an active WRITE SECTOR streams to disk; anything else just
        // stages regDATA (e.g. the SEEK target)
        if (!DATA_COUNTER || (COMMAND >> 5) != 0x02) {
            regDATA = static_cast<uint8_t>(~dt);
            return 0;
        }

        // WRITE SECTOR data path (only if a drive is mounted)
        if (!curDrv().bs) { regSTATUS = 0x80; return 1; }

        // Stream into buffer and flush chunk-sized blocks to disk
        const uint16_t chunk = (curDrv().sector_size < sizeof(buffer))
                                 ? curDrv().sector_size
                                 : sizeof(buffer);

        buffer[buffer_pos] = static_cast<uint8_t>(~dt);
        --DATA_COUNTER;

        if (buffer_pos == chunk - 1) {
            uint32_t wlen = 0;
            buffer_pos = 0;
            curDrv().bs->set(buffer, chunk, wlen);
            if (wlen != chunk) { // write fault: terminate the command
                DATA_COUNTER = 0; COMMAND = 0x00; STATUS_SCRIPT = 0;
                regSTATUS = 0x20;
                return 1;
            }
        } else {
            ++buffer_pos;
        }

        // Sector finished?
        if (!DATA_COUNTER) {

            curDrv().bs->flush();
            // A directory mount reports dropped/failed physical writes
            // (e.g. the backing medium is full) as a write fault - the
            // guest must not believe a write that never landed
            if (curDrv().dirsrc && curDrv().dirsrc->takeWriteError()) {
                DATA_COUNTER = 0; COMMAND = 0x00; STATUS_SCRIPT = 0;
                regSTATUS = 0x20;
                return 1;
            }
            if (MULTIBLOCK_RW) {
                // advance to next sector id on track
                regSECTOR = static_cast<uint8_t>(curDrv().SECTOR + 1);
                if (seekToSector(drvIdx(), regSECTOR)) {
                    regSECTOR--; STATUS_SCRIPT = 4; // RNF once, then 0x00
                } else {
                    DATA_COUNTER = curDrv().sector_size;
                    buffer_pos   = 0;
                    STATUS_SCRIPT = 2; // one BUSY then BUSY+DRQ
                }
            } else {
                COMMAND = 0x00; regSTATUS = 0x00; STATUS_SCRIPT = 0;
            }
        }
        return 0;
    }

    case 4: // MOTOR (drive select & motor on)
        if (dt & 0x04) {
            MOTOR = static_cast<uint8_t>(dt & 0x83);
        } else {
            if (dt & 0x80) MOTOR = static_cast<uint8_t>(MOTOR | 0x80);
            else           MOTOR = static_cast<uint8_t>(MOTOR & 0x03);
        }
        return 0;

    case 5: // SIDE select
        SIDE = static_cast<uint8_t>(dt & 0x01);
        return 0;

    case 6: // DENSITY
        DENSITY = static_cast<uint8_t>(dt & 0x01);
        return 0;

    case 7: // EINT (interrupt mode)
        EINT = static_cast<uint8_t>(dt & 0x01);
        if (!EINT) { waitForInt = 0; error_int = 0; release_interrupt(); }
        return 0;

    default:
        return 0;
    }
}

// -------------------- WRITE TRACK (format), ported from unicard fdc.c --------------------
//
// The Z80 streams a raw WD1793 MFM track image through the DATA register;
// the state machine picks out the marks (bytes arrive inverted: 0x03 = index
// 0xFC, 0x01 = ID mark 0xFE, 0x04/0x07 = data mark 0xFB/0xF8), collects the
// sector IDs and size, and rebuilds the DSK track block when the trailing
// gap runs out. buffer[] doubles as scratch: [0..7] track header fields,
// [8..] the sector ID list, [255] the sector fill byte.

int FDCDevice::abortTrackWrite() {
    regSTATUS = 0x00;
    STATUS_SCRIPT = 0;
    COMMAND = 0x00;
    return 1;
}

int FDCDevice::writeTrackByte(uint8_t dt) {
    auto& d = drive[MOTOR & 0x03];
    if (!d.bs) { regSTATUS = 0x80; return 1; }

    switch (write_track_stage) {
    case 0: // waiting for the index mark
        if (dt == 0x03) { // ~0xfc
            write_track_stage = 1;
            write_track_counter = 0;
            std::memset(buffer, 0x00, sizeof(buffer));
            if (regTRACK == 0 && SIDE == 0) {
                // First track: stamp the DSK header and clear the track
                // table; offsets rebuild as the tracks are formatted
                static const uint8_t dsk_hdr[18] =
                    {'U','n','i','c','a','r','d',' ','v','1','.','0','0',0,0,2,0,0};
                uint32_t wlen = 0;
                if (d.bs->size() < 0x100 && d.bs->resize(0x100) != 0)
                    return abortTrackWrite();
                // Write the whole header sector: resize() expands without
                // zero-filling, so bytes 0x00..0x21 of a fresh image are
                // undefined until written here
                if (d.bs->seek(0) != 0) return abortTrackWrite();
                d.bs->set(buffer, 0x22, wlen); // zero 0x00..0x21
                if (wlen != 0x22) return abortTrackWrite();
                d.bs->set(dsk_hdr, sizeof(dsk_hdr), wlen);
                if (wlen != sizeof(dsk_hdr)) return abortTrackWrite();
                d.bs->set(buffer, 204, wlen); // zero the table, 0x34..0xff
                if (wlen != 204) return abortTrackWrite();
            }
        } else if (write_track_counter > 100) {
            return abortTrackWrite(); // format never started
        }
        break;

    case 1: // waiting for the first ID address mark
        if (dt == 0x01) { // ~0xfe
            write_track_stage = 2;
            write_track_counter = 0;
            buffer[0] = regTRACK;
            buffer[1] = SIDE;
            // [2..3] unused, [4] sector size code (filled below)
            buffer[5] = 1;    // number of sectors on the track
            buffer[6] = 0x4e; // GAP#3 length
            buffer[7] = 0xe5; // filler byte
            buffer_pos = 8;   // the sector ID list grows from here
        } else if (write_track_counter > 100) {
            return abortTrackWrite();
        }
        break;

    case 2: // ID field: C, H, R, N at counters 1..4, then the data mark
        if (write_track_counter <= 4) {
            if (write_track_counter == 3)
                buffer[buffer_pos++] = static_cast<uint8_t>(~dt); // sector ID
            else if (write_track_counter == 4)
                buffer[4] = static_cast<uint8_t>(~dt);            // size code
        } else if (dt == 0x04 || dt == 0x07) { // ~0xfb / ~0xf8
            write_track_stage = 3;
            write_track_counter = 0;
            DATA_COUNTER = static_cast<uint16_t>(buffer[4] * 0x100);
        } else if (write_track_counter > 100) {
            return abortTrackWrite();
        }
        break;

    case 3: // sector data: remember the fill byte, count to the end
        if (write_track_counter == 1)
            buffer[sizeof(buffer) - 1] = static_cast<uint8_t>(~dt);
        if (write_track_counter > DATA_COUNTER) {
            write_track_stage = 4;
            write_track_counter = 0;
            DATA_COUNTER = 0;
        }
        break;

    case 4: // either the next sector's ID mark, or the trailing gap
        if (dt == 0x01) { // ~0xfe: another sector follows
            write_track_stage = 2;
            write_track_counter = 0;
            buffer[5]++;
        } else if (write_track_counter > 200) {
            return finishTrackWrite(); // end of the track
        }
        break;

    default: // stage 5: track finished, ignore trailing bytes
        break;
    }

    write_track_counter++;
    return 0;
}

// One byte of the synthetic READ TRACK stream (real, uninverted value).
// buffer[4] = size code, buffer[5] = sector count, buffer[8..] = ID list,
// all cached by the command setup; data bytes come sequentially from the
// image via the ByteSource cache.
uint8_t FDCDevice::readTrackByte() {
    auto& d = drive[MOTOR & 0x03];

    switch (rt_phase) {
    case 0: rt_phase = 1; return 0xfc;                    // index address mark
    case 1: rt_phase = 2; return 0xfe;                    // ID address mark
    case 2: rt_phase = 3; return regTRACK;                // C
    case 3: rt_phase = 4; return static_cast<uint8_t>(SIDE & 0x01); // H
    case 4: rt_phase = 5; return buffer[8 + rt_sec_idx];  // R (sector ID)
    case 5: rt_phase = 6; return buffer[4];               // N (size code)
    case 6:
        rt_phase = 7;
        rt_remaining = static_cast<uint16_t>(buffer[4] * 0x100);
        return 0xfb;                                      // data address mark
    default: { // phase 7: sector data
        uint8_t b = 0;
        if (!d.bs || d.bs->getByte(b) != 0) b = 0xe5;     // filler on failure
        if (--rt_remaining == 0) { ++rt_sec_idx; rt_phase = 1; }
        return b;
    }
    }
}

int FDCDevice::finishTrackWrite() {
    const uint8_t drv = static_cast<uint8_t>(MOTOR & 0x03);
    auto& d = drive[drv];

    COMMAND = 0x00;
    regSTATUS = 0x00;
    STATUS_SCRIPT = 0;
    write_track_stage = 5;

    const uint8_t nsec = buffer[5];
    const uint8_t size_code = buffer[4];
    if (!nsec || !size_code) { regSTATUS = 0x10; return 1; }

    // The table entries for all preceding tracks are in place (they were
    // formatted before this one), so the offset can be recomputed fresh
    const int32_t off = getTrackOffset(drv, regTRACK, SIDE);
    if (!off) { regSTATUS = 0x10; return 1; }
    d.track_offset = off;
    d.TRACK = regTRACK;
    d.SIDE = SIDE;
    d.SECTOR = 0;
    d.sector_size = 0;

    // Size the DSK to end exactly at this track: grows a fresh image,
    // truncates leftovers of a previous layout
    const uint32_t data_bytes = static_cast<uint32_t>(nsec) * size_code * 0x100u;
    const uint32_t track_end = static_cast<uint32_t>(off) + 0x100u + data_bytes;
    if (d.bs->resize(track_end) != 0) { regSTATUS = 0x20; return 1; }

    uint32_t wlen = 0;

    // 0x100-byte track header: signature, track info, 29 sector descriptors
    static const uint8_t tinfo[16] =
        {'T','r','a','c','k','-','I','n','f','o',0x0d,0x0a,0,0,0,0};
    if (d.bs->seek(static_cast<uint32_t>(off)) != 0) { regSTATUS = 0x20; return 1; }
    d.bs->set(tinfo, sizeof(tinfo), wlen);
    if (wlen != sizeof(tinfo)) { regSTATUS = 0x20; return 1; }
    d.bs->set(buffer, 8, wlen); // track, side, -, -, size, count, gap, filler
    if (wlen != 8) { regSTATUS = 0x20; return 1; }

    const uint8_t fill = buffer[sizeof(buffer) - 1];

    // Descriptor template: track, side, sector ID, size code, two FDC
    // status bytes, actual sector size little-endian (size_code * 0x100)
    buffer[3] = size_code;
    buffer[4] = 0x00;
    buffer[5] = 0x00;
    buffer[6] = 0x00;
    buffer[7] = size_code;
    buffer_pos = 8; // walk the ID list collected during formatting
    for (uint8_t i = 1; i <= 29; ++i) {
        if (buffer_pos != 0) {
            if (buffer[buffer_pos] != 0x00) {
                buffer[2] = buffer[buffer_pos++];
            } else { // list exhausted: pad with zeroed descriptors
                std::memset(buffer, 0x00, 8);
                buffer_pos = 0;
            }
        }
        d.bs->set(buffer, 8, wlen);
        if (wlen != 8) { regSTATUS = 0x20; return 1; }
    }

    // Sector data, filled with the byte captured from the format stream
    std::memset(buffer, fill, sizeof(buffer));
    uint32_t remaining = data_bytes;
    while (remaining) {
        const uint32_t len = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;
        d.bs->set(buffer, len, wlen);
        if (wlen != len) { regSTATUS = 0x20; return 1; }
        remaining -= len;
    }

    // File-level bookkeeping: track count at 0x30, size entry in the table
    uint8_t b = (SIDE == 1) ? static_cast<uint8_t>(regTRACK + 1) : regTRACK;
    if (d.bs->seek(0x30) != 0) { regSTATUS = 0x20; return 1; }
    d.bs->set(&b, 1, wlen);
    if (wlen != 1) { regSTATUS = 0x20; return 1; }

    b = static_cast<uint8_t>(nsec * size_code + 1); // in 0x100 units incl. header
    if (d.bs->seek(0x34u + regTRACK * 2u + SIDE) != 0) { regSTATUS = 0x20; return 1; }
    d.bs->set(&b, 1, wlen);
    if (wlen != 1) { regSTATUS = 0x20; return 1; }

    if (d.bs->flush() != 0) return 1;
    if (d.dirsrc && d.dirsrc->takeWriteError()) { regSTATUS = 0x20; return 1; }
    return 0;
}

int FDCDevice::fdcRead(uint8_t port, uint8_t* dt, uint8_t /*high_addr*/) {
    const uint8_t off = static_cast<uint8_t>(port - readMappings[0].port) & 0x07;

    auto curDrv = [&]() -> FDDrive& { return drive[MOTOR & 0x03]; };
    auto drvIdx = [&]() -> uint8_t   { return static_cast<uint8_t>(MOTOR & 0x03); };

    switch (off) {
    case 0: { // STATUS register read (with STATUS_SCRIPT choreography)
        // A status read acknowledges an immediate-termination /INT (real
        // WD1793 clears INTRQ on status read); the transfer-pacing
        // waitForInt deliberately survives status polls, as in the original.
        if (error_int) { error_int = 0; release_interrupt(); }

        // Timeout/“lazy next sector” hacks from original implementation:
        // If controller is in READ SECTOR and host keeps polling STATUS without reading DATA,
        if (regSTATUS != 0x18) {
            if (curDrv().bs && DATA_COUNTER &&
                (DATA_COUNTER == curDrv().sector_size) && ((COMMAND >> 5) == 0x03)) {
                if (++reading_status_counter > 10) {
                    reading_status_counter = 0;
                    if (MULTIBLOCK_RW) {
                        regSECTOR = static_cast<uint8_t>(curDrv().SECTOR + 1);
                        if (seekToSector(drvIdx(), regSECTOR)) {
                            regSECTOR--; STATUS_SCRIPT = 4;
                        } else {
                            uint32_t rlen = 0;
                            const uint16_t chunk = (curDrv().sector_size < sizeof(buffer))
                                                     ? curDrv().sector_size
                                                     : sizeof(buffer);
                            curDrv().bs->get(buffer, chunk, rlen);
                            if (rlen != chunk) {
                                DATA_COUNTER = 0; COMMAND = 0x00; STATUS_SCRIPT = 0;
                                regSTATUS = 0x08;
                                return 1;
                            }
                            buffer_pos = 0;
                            DATA_COUNTER = curDrv().sector_size;
                            STATUS_SCRIPT = 2;
                        }
                    } else {
                        DATA_COUNTER = 0; COMMAND = 0x00; regSTATUS = 0x00; STATUS_SCRIPT = 0;
                    }
                }
            }
        }

        switch (STATUS_SCRIPT) {
        case 1: *dt = static_cast<uint8_t>(~(regSTATUS | 0x01)); STATUS_SCRIPT = 0; break; // 1x BUSY
        case 2: *dt = static_cast<uint8_t>(~0x01); regSTATUS = 0x03; STATUS_SCRIPT = 0; break; // BUSY then BUSY+DRQ
        case 3: *dt = static_cast<uint8_t>(~0x01); regSTATUS = 0x18; STATUS_SCRIPT = 0; break; // cp/m hack
        case 4: *dt = static_cast<uint8_t>(~0x11); regSTATUS = 0x00; STATUS_SCRIPT = 0; break; // RNF once
        case 0xFF: *dt = static_cast<uint8_t>(~(regSTATUS & ~0x06)); ++regSTATUS; break;       // experimental
        default: *dt = static_cast<uint8_t>(~regSTATUS); break;
        }
        return 0;
    }

    case 1: { // TRACK
        if (regTRACK == 0x5a) {
            if (!drive[0].bs) { *dt = 0xFF; return 0; }
            if (fd0disabled < 0) {
                FIL tmp{};
                if (FR_OK == f_open(&tmp, "/unicard/fd0disabled.cfg", FA_READ)) {
                    f_close(&tmp); fd0disabled = 1;
                } else fd0disabled = 0;
            }
            if (fd0disabled) { *dt = 0xFF; return 0; }
        }
        *dt = static_cast<uint8_t>(~regTRACK);
        return 0;
    }

    case 2: { // SECTOR (if motor is on, report actual)
        if (MOTOR & 0x80) regSECTOR = curDrv().SECTOR;
        *dt = static_cast<uint8_t>(~regSECTOR);
        return 0;
    }

    case 3: { // DATA
        if (waitForInt || error_int) { waitForInt = 0; error_int = 0; release_interrupt(); }
        reading_status_counter = 0;

        if (!curDrv().bs) { regSTATUS = 0x80; *dt = 0xFF; return 1; }

        if ((COMMAND >> 4) == 0x01) { // READ TRACK stream
            if (!DATA_COUNTER) { *dt = 0xFF; return 1; }
            *dt = static_cast<uint8_t>(~readTrackByte());
            if (--DATA_COUNTER == 0) { COMMAND = 0x00; regSTATUS = 0x00; STATUS_SCRIPT = 0; }
            return 0;
        }

        if ((COMMAND >> 4) == 0x03) { // READ ADDRESS payload (6 bytes)
            *dt = static_cast<uint8_t>(~buffer[6 - DATA_COUNTER]);
            if (--DATA_COUNTER == 0) { COMMAND = 0x00; regSTATUS = 0x00; STATUS_SCRIPT = 0; }
            return 0;
        }

        if (DATA_COUNTER) {
            const uint16_t chunk = (curDrv().sector_size < sizeof(buffer))
                                     ? curDrv().sector_size
                                     : sizeof(buffer);
            *dt = static_cast<uint8_t>(~buffer[buffer_pos]);
            --DATA_COUNTER;

            if (buffer_pos == chunk - 1) {
                buffer_pos = 0;
                uint32_t rlen = 0;
                curDrv().bs->get(buffer, chunk, rlen);
                if (rlen != chunk) { // data error: terminate the command
                    DATA_COUNTER = 0; COMMAND = 0x00; STATUS_SCRIPT = 0;
                    regSTATUS = 0x08;
                    return 1;
                }
            } else {
                ++buffer_pos;
            }

            if (!DATA_COUNTER) {
                if (MULTIBLOCK_RW) {
                    STATUS_SCRIPT = 2;
                    MULTIBLOCK_RW = 1; // BASIC hack from original
                    regSECTOR = static_cast<uint8_t>(curDrv().SECTOR + 1);
                    if (seekToSector(drvIdx(), regSECTOR)) {
                        regSECTOR--; STATUS_SCRIPT = 4;
                    } else {
                        uint32_t rlen = 0;
                        const uint16_t c2 = (curDrv().sector_size < sizeof(buffer))
                                             ? curDrv().sector_size
                                             : sizeof(buffer);
                        curDrv().bs->get(buffer, c2, rlen);
                        if (rlen != c2) {
                            DATA_COUNTER = 0; COMMAND = 0x00; STATUS_SCRIPT = 0;
                            regSTATUS = 0x08;
                            return 1;
                        }
                        buffer_pos = 0;
                        DATA_COUNTER = curDrv().sector_size;
                        STATUS_SCRIPT = 2;
                    }
                } else {
                    COMMAND = 0x00; regSTATUS = 0x00; STATUS_SCRIPT = 0;
                }
            }
            return 0;
        } else {
            // Host asked for DATA but none is pending
            *dt = 0xFF;
            return 1;
        }
    }

    default:
        return 0;
    }
}
