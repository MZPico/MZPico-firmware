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

REGISTER_MZ_DEVICE(FDCDevice)

// -------------------- Construction & registration --------------------

FDCDevice::FDCDevice() {
    readPortCount  = FDC_READ_PORTS;
    writePortCount = FDC_WRITE_PORTS;

    for (uint8_t i = 0; i < readPortCount; ++i)
        readMappings[i].fn  = FDCDevice::ReadThunk;
    for (uint8_t i = 0; i < writePortCount; ++i)
        writeMappings[i].fn = FDCDevice::WriteThunk;
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

int FDCDevice::init() {
    fd0disabled = -1;
    return 1;
}

int FDCDevice::readConfig(dictionary *ini) {
    if (!ini) return -1;

    for (int i = 0; i < FDC_NUM_DRIVES; i++) {
        std::string image = iniparser_getstring(ini, (getDevID() + ":image_disk" + std::to_string(i+1)).c_str(), "");
        if (!image.empty())
            setDriveContent(i, image.c_str());
    }
        
    //setWriteProtected(iniparser_getboolean(ini, (getDevID() + ":write_protected").c_str(), false));
    //if (!image.empty())
        //setDriveContent(image);
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
    if (DATA_COUNTER) {
        const uint8_t t2 = (COMMAND >> 5);
        pending = (t2 == 0x03 /*READ*/ || t2 == 0x02 /*WRITE*/ || COMMAND == 0x3f
                   || COMMAND == 0x0f || COMMAND == 0x0b /*WRITE TRACK stages*/
        );
    }
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
    //if (d.fh.obj.fs) {
    //    if (FR_OK != f_sync(&d.fh)) return -1;
    //    f_close(&d.fh);
    //}
    std::memset(&d, 0x00, sizeof(d));
    //std::strncpy(d.path, file_path, sizeof(d.path) - 1);
    ByteSourceFactory::from_file(file_path, 0, 128, /* wrap = */false, d.bs);
    
    //if (FR_OK != f_open(&d.fh, d.path, FA_READ | FA_WRITE)) {
    //    d.path[0] = 0;
    //    d.filename[0] = 0;
    //    return -1;
    //}

    d.track_offset = getTrackOffset(drive_id, d.TRACK, d.SIDE);
    //if (!d.track_offset) {
    //    f_close(&d.fh);
    //    d.path[0] = 0;
    //    d.filename[0] = 0;
    //    return -1;
    //}
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
    if (drive_id >= FDC_NUM_DRIVES) return 1;
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
    const uint8_t off = static_cast<uint8_t>(port - basePort) & 0x07;

    // Convenience aliases
    auto drvIdx = [&]() -> uint8_t   { return static_cast<uint8_t>(MOTOR & 0x03); };
    auto curDrv = [&]() -> FDDrive& { return drive[MOTOR & 0x03]; };

    switch (off) {
    case 0: { // COMMAND / STATUS register write: process command
        if (waitForInt) { waitForInt = 0; release_interrupt(); } // drop /INT on write to cmd
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
            STATUS_SCRIPT = 1;                   // one BUSY, next READY
            return 0;
        }

        // ---- Type II (read/write sector) ----
        if ((COMMAND >> 6) == 0x01) {
            regSTATUS = 0; DATA_COUNTER = 0; buffer_pos = 0; STATUS_SCRIPT = 1;
            if (!curDrv().bs) { regSTATUS = 0x80; return 1; } // not ready

            MULTIBLOCK_RW = (COMMAND & 0x10) ? 0 : 1; // original inverted meaning
            if (setTrack()) { STATUS_SCRIPT = 3; return 1; }

            // sector select and seek
            if (seekToSector(drvIdx(), regSECTOR)) { STATUS_SCRIPT = 3; return 1; }

            // READ SECTOR (t2 == 3) or WRITE SECTOR (t2 == 2)
            const uint8_t t2 = static_cast<uint8_t>(COMMAND >> 5);
            if (t2 == 0x03) {
                // Preload first chunk into buffer
                uint16_t chunk = (curDrv().sector_size < sizeof(buffer))
                                  ? curDrv().sector_size
                                  : sizeof(buffer);
                uint32_t rlen = 0;
                curDrv().bs->get(buffer, chunk, rlen);
                if (rlen != chunk) return 1;
            }
            DATA_COUNTER = curDrv().sector_size;
            regSTATUS |= 0x01; // BUSY
            regSTATUS |= 0x02; // DRQ
            return 0;
        }

        // ---- Type III: READ ADDRESS (0x3F) ----
        if ((COMMAND >> 4) == 0x03) {
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
            regSTATUS = 0x00;
            regSTATUS |= 0x01; // BUSY
            regSTATUS |= 0x02; // DRQ
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

    case 3: { // DATA register (either data byte or staged writes)
        if (waitForInt) { waitForInt = 0; release_interrupt(); }

        // If no active transfer, this is just regDATA staging for SEEK
        if (!DATA_COUNTER) { regDATA = static_cast<uint8_t>(~dt); return 0; }

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
            if (wlen != chunk) return 1;
        } else {
            ++buffer_pos;
        }

        // Sector finished?
        if (!DATA_COUNTER) {
            
            curDrv().bs->flush();
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
        if (!EINT) { waitForInt = 0; release_interrupt(); }
        return 0;

    default:
        return 0;
    }
}

int FDCDevice::fdcRead(uint8_t port, uint8_t* dt, uint8_t /*high_addr*/) {
    const uint8_t off = static_cast<uint8_t>(port - basePort) & 0x07;

    auto curDrv = [&]() -> FDDrive& { return drive[MOTOR & 0x03]; };
    auto drvIdx = [&]() -> uint8_t   { return static_cast<uint8_t>(MOTOR & 0x03); };

    switch (off) {
    case 0: { // STATUS register read (with STATUS_SCRIPT choreography)
        // Timeout/“lazy next sector” hacks from original implementation:
        // If controller is in READ SECTOR and host keeps polling STATUS without reading DATA,
        if (regSTATUS != 0x18) {
            if ((DATA_COUNTER == curDrv().sector_size) && ((COMMAND >> 5) == 0x03)) {
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
                            if (rlen != chunk) return 1;
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
        if (waitForInt) { waitForInt = 0; release_interrupt(); }
        reading_status_counter = 0;

        if (!curDrv().bs) { regSTATUS = 0x80; *dt = 0xFF; return 1; }

        if (COMMAND == 0x3f) { // READ ADDRESS payload (6 bytes)
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
                if (rlen != chunk) return 1;
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
                        if (rlen != c2) return 1;
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
