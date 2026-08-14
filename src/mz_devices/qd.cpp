#include "qd.hpp"
#include "ff.h"
#include "iniparser.h"
#include "file_source.hpp"
#include "qd_dir_source.hpp"

REGISTER_MZ_DEVICE(QDDevice)

// -------------------------------- Lifecycle --------------------------------

QDDevice::QDDevice() {
    for (int i = 0; i < QD_PORTS; i++)
        readMappings[i].fn  = QDDevice::readByte;
    for (int i = 0; i < QD_PORTS; i++)
        writeMappings[i].fn  = QDDevice::writeByte;
    
    // Initialize port mappings with defaults
    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);
    bs = nullptr;
}

QDDevice::~QDDevice() { close(); }

std::vector<uint8_t> QDDevice::getReadPorts() const {
    std::vector<uint8_t> ports;
    for (uint8_t i = 0; i < QD_PORTS; ++i) {
        ports.push_back(QD_DEFAULT_BASE_PORT + i);
    }
    return ports;
}

std::vector<uint8_t> QDDevice::getWritePorts() const {
    std::vector<uint8_t> ports;
    for (uint8_t i = 0; i < QD_PORTS; ++i) {
        ports.push_back(QD_DEFAULT_BASE_PORT + i);
    }
    return ports;
}

int QDDevice::init() {

    memset(channel, 0, sizeof(channel));
    out_crc16 = 0;
    image_position = 0;
    writeProtected = false;
    stdPath.clear();

    channel[0].name = 'A';
    channel[1].name = 'B';
    driveReset();

    // Defaults
    connected = QDISK_CONNECTED;
    status = QDSTS_NO_DISC;

    // No auto-open here; let caller set paths and call open()
    return 0;
}

int QDDevice::isInterrupt() { return 0; }

int QDDevice::readConfig(dictionary *ini) {
    if (!ini) return -1;

    std::string image = iniparser_getstring(ini, (getDevID() + ":image").c_str(), "");
    setWriteProtected(iniparser_getboolean(ini, (getDevID() + ":write_protected").c_str(), false));
    if (!image.empty())
        setDriveContent(image);
    return 0;
}

// ----------------------------- State management ----------------------------

void QDDevice::setConnected(bool on) {
    connected = on ? QDISK_CONNECTED : QDISK_DISCONNECTED;
    if (!on) {
        close();
        status = QDSTS_NO_DISC;
    }
}

void QDDevice::setWriteProtected(bool on) {
    writeProtected = on;
    if (status & QDSTS_IMG_READY) {
        close();
        open();
    }
}

bool QDDevice::isWriteProtected() const { return writeProtected; }

void QDDevice::setDriveContent(const std::string& path)  { stdPath  = path; open(); }

// --------------------------------- Helpers ---------------------------------

void QDDevice::driveReset() {
    image_position = 0;
    status |= QDSTS_HEAD_HOME;
}

int QDDevice::flush() {
    if (!bs)
        return -1;
    
    return bs->flush();
}

// --------------------------------- Opening ---------------------------------

void QDDevice::open(void) {
    FILINFO fno;

    if (connected != QDISK_CONNECTED) {
        status = QDSTS_NO_DISC;
        return;
    }

    close(); // flush the previous image while status still says READY
    driveReset();
    status = QDSTS_NO_DISC;

    if (stdPath.empty())
        return;

    FRESULT fr = f_stat(stdPath.c_str(), &fno);
    if (fr != FR_OK)
        return;

    int ret;
    dirsrc = nullptr;
    if (fno.fattrib & AM_DIR) {
        ret = ByteSourceFactory::from_qddir(stdPath, 128, bs);
        if (ret == 0) dirsrc = static_cast<QDDirSource*>(bs.get());
    } else {
        ret = ByteSourceFactory::from_file(stdPath, 0, 128, /* wrap = */false, bs);
    }
    if (ret != 0) { // mount failed: report no disk instead of a dead drive
        bs.reset();
        return;
    }

    status = QDSTS_IMG_READY | QDSTS_HEAD_HOME;
    // Reported via CTS in channel A RR0 and enforced in testDiskIsWriteable.
    // Directory mounts are writable through the save engine, so only the
    // ini flag protects them; file mounts also honor a read-only image.
    if (writeProtected || bs->readOnly())
        status |= QDSTS_IMG_READONLY;
}

void QDDevice::close() {
    if (connected == QDISK_CONNECTED && (status & QDSTS_IMG_READY)) {
        if (bs && bs->flush() != 0) {
            std::fprintf(stderr, "QuickDisk: flush error\n");
        }
        status &= ~QDSTS_IMG_READY;
    }
}

// ----------------------------- Data path (read) ----------------------------

uint8_t QDDevice::readByteFromDrive() {
    uint8_t retval = 0xff;

    if ((status & QDSTS_IMG_READY) == 0) return 0xff;            // no media
    if ((channel[QDSIO_CHANNEL_B].Wreg[QDSIO_REGADDR_5] & 0x80) == 0x00) return 0xff; // motor off

    if (QDISK_IMAGE_MAX_SIZE <= image_position) {
        if (QDISK_IMAGE_MAX_SIZE == image_position) image_position++;
        return 0xff;
    }

    // Keep the source in step with the head: write events on directory
    // mounts (and failed reads) move image_position without moving bs.
    // An unseekable position (past the backing store) reads as 0xff.
    if (bs->tell() != image_position && bs->seek(image_position) != 0) {
        image_position++;
        return 0xff;
    }

    // Past the backing store (which is smaller than QDISK_IMAGE_MAX_SIZE) or
    // on a read error the drive must deliver 0xff, as mz800emu does — a
    // garbage byte here can spuriously match the sync pair during hunt
    if (bs->getByte(retval) != 0) retval = 0xff;
    image_position++;
    return retval;
}

// ----------------------------- Data path (write) ---------------------------

int QDDevice::testDiskIsWriteable() {
    if ((status & QDSTS_IMG_READY) == 0) return 0;
    if (status & QDSTS_IMG_READONLY)    return 0;
    if ((channel[QDSIO_CHANNEL_B].Wreg[QDSIO_REGADDR_5] & 0x80) == 0x00) return 0; // motor off
    if ((channel[QDSIO_CHANNEL_A].Wreg[QDSIO_REGADDR_5] & 0x08) == 0x00) return 0; // output mode not set
    return 1;
}

void QDDevice::writeByteIntoDrive(uint8_t value) {
    if (0 == testDiskIsWriteable()) return;

    if (dirsrc) {
        // Directory mount: the save engine parses the stream into MZF
        // files; the synthesized backing store is never written directly.
        // Clamp the position so a long format stream can't trip the RR1
        // CRC-error flag (mz800emu never advances position while formatting)
        dirsrc->wrDataEvent(value);
        if (image_position < QDISK_IMAGE_MAX_SIZE) image_position++;
        return;
    }

    if (QDISK_IMAGE_MAX_SIZE <= image_position) {
        if (QDISK_IMAGE_MAX_SIZE == image_position) image_position++;
        return;
    };
    bs->setByte(value);
    image_position++;
}

// -------------------------------- Registers --------------------------------

int QDDevice::readByte(MZDevice* self_, uint8_t port, uint8_t *dt, uint8_t /*high_addr*/) {
    auto* self = static_cast<QDDevice*>(self_);
    // Register offset relative to the configured base (works for any base_port)
    uint8_t SIO_addr = static_cast<uint8_t>(port - self->readMappings[0].port) & 0x03;
    st_QDSIO_CHANNEL* channel = &self->channel[SIO_addr & 0x01];

    switch (SIO_addr) {
        case QDSIO_ADDR_CTRL_A: {
            // Hunt phase
            if ( (channel->Wreg[QDSIO_REGADDR_3] & 0x11) == 0x11 ) {
                channel->Rreg[QDSIO_REGADDR_0] |= 0x10;
                self->status &= ~QDSTS_IMG_SYNC;

                uint8_t sync1 = self->readByteFromDrive();
                for (int i = 0; i < 8; i++) {
                    uint8_t sync2 = self->readByteFromDrive();
                    if ( (sync1 == channel->Wreg[QDSIO_REGADDR_6]) &&
                         (sync2 == channel->Wreg[QDSIO_REGADDR_7]) ) {
                        channel->Rreg[QDSIO_REGADDR_0] &= 0xef; // end hunt
                        self->status |= QDSTS_IMG_SYNC;
                        break;
                    }
                    sync1 = sync2;
                }
            }

            channel->Rreg[QDSIO_REGADDR_0] |= 0x01; // at least one byte in RX
            channel->Rreg[QDSIO_REGADDR_0] |= 0x04; // TX buffer empty

            if (self->status & QDSTS_IMG_READY) channel->Rreg[QDSIO_REGADDR_0] |= 0x08; // DCD 1: present
            else                            channel->Rreg[QDSIO_REGADDR_0] &= ~0x08;

            if (self->status & QDSTS_IMG_READONLY) channel->Rreg[QDSIO_REGADDR_0] &= ~0x20; // CTS 0: write-protected
            else                                channel->Rreg[QDSIO_REGADDR_0] |= 0x20;

            if (QDISK_IMAGE_MAX_SIZE < self->image_position) channel->Rreg[QDSIO_REGADDR_1] |= 0x40; // CRC error
            else                                         channel->Rreg[QDSIO_REGADDR_1] &= ~0x40;

            *dt = channel->Rreg[channel->REG_addr & 0x03];
            break;
        }

        case QDSIO_ADDR_CTRL_B: {
            channel->Rreg[QDSIO_REGADDR_0] = (self->status & QDSTS_HEAD_HOME) ? 0x08 : 0x00;

            // NB: must test channel A's WR5 (TX mode) — `channel` points at
            // channel B here, so indexing it would silently read B's WR5
            // (mz800emu: g_qdisk.channel[QDSIO_CHANNEL_A])
            if ( (self->channel[QDSIO_CHANNEL_A].Wreg[QDSIO_REGADDR_5] & 0x1a) == 0x0a ) {
                if (self->out_crc16 != 0) {
                    self->writeByteIntoDrive('C');
                    self->writeByteIntoDrive('R');
                    self->writeByteIntoDrive('C');
                }
            }

            if (QDSIO_REGADDR_0 == channel->REG_addr) *dt = 0xff;
            else *dt = channel->Rreg[channel->REG_addr & 0x03];
            break;
        }

        case QDSIO_ADDR_DATA_A:
            self->status &= ~QDSTS_HEAD_HOME;
            if (self->status & QDSTS_IMG_READY) {
                // The count byte (position 4) starting a fresh read pass
                // rebuilds the directory listing in natural order; the
                // post-save verify pass never re-reads the count block, so
                // its saved-file-last ordering survives (mz800emu rule)
                if (self->dirsrc && self->image_position == 4)
                    self->dirsrc->rdCountEvent();
                *dt = self->readByteFromDrive();
            }
            else *dt = 0xff;
            break;

        case QDSIO_ADDR_DATA_B:
            *dt = 0xff;
            break;
    }

    channel->REG_addr = QDSIO_REGADDR_0;
    return 0;
}

int QDDevice::writeByte(MZDevice* self_, uint8_t port, uint8_t dt, uint8_t /*high_addr*/) {
    auto* self = static_cast<QDDevice*>(self_);
    const uint8_t SIO_addr = static_cast<uint8_t>(port - self->writeMappings[0].port) & 0x03;
    st_QDSIO_CHANNEL* channel = &self->channel[SIO_addr & 0x01];

    if (SIO_addr & 0x02) {
        // CTRL write
        channel->Wreg[channel->REG_addr] = dt;

        if (QDSIO_REGADDR_0 == channel->REG_addr) {
            channel->REG_addr = static_cast<en_QDSIO_REGADRR>(dt & 0x07);
            en_QDSIO_WR0CMD wr0cmd = static_cast<en_QDSIO_WR0CMD>((dt >> 3) & 0x07);

            if ((dt & 0xc0) == 0x80) self->out_crc16 = 0; // reset outgoing CRC calc

            switch (wr0cmd) {
                case QDSIO_WR0CMD_RESET:
                    std::memset(&channel->Wreg, 0x00, sizeof(channel->Wreg));
                    break;
                case QDSIO_WR0CMD_NONE:
                case QDSIO_WR0CMD_RESET_INTF:
                case QDSIO_WR0CMD_SDLC_STOP:
                case QDSIO_WR0CMD_ENABLE_INT:
                case QDSIO_WR0CMD_RESET_OUTBUF_INT:
                case QDSIO_WR0CMD_RESET_ERRFL:
                case QDSIO_WR0CMD_RETI:
                    break;
            }
        } else {
            switch (channel->REG_addr) {
                case QDSIO_REGADDR_2:
                    if (channel->name == 'B') channel->Rreg[channel->REG_addr] = dt;
                    break;

                case QDSIO_REGADDR_3:
                    if (channel->Wreg[QDSIO_REGADDR_3] & 0x10) {
                        channel->Rreg[QDSIO_REGADDR_0] |= 0x10; // enter Hunt
                    }
                    break;

                case QDSIO_REGADDR_5:
                    if (channel->name == 'B') {
                        if ( (channel->Wreg[QDSIO_REGADDR_5] & 0x80) == 0x00 ) {
                            // Motor off: abandon an unfinished save, rewind
                            if (self->dirsrc) self->dirsrc->wrAbortEvent();
                            if (self->status & QDSTS_IMG_READY) {
                                if (self->bs->seek(0) != 0) {
                                    std::fprintf(stderr, "QuickDisk: fseek() error\n");
                                }
                                self->bs->flush();
                            }
                            self->driveReset();
                        }
                    } else {
                        if ( (channel->Wreg[QDSIO_REGADDR_5] & 0x18) == 0x18 ) {
                            // TX interrupt + TX enable: gap byte (image mounts
                            // only; the save engine ignores gaps, as mz800emu)
                            if (!self->dirsrc) self->writeByteIntoDrive(0x00);
                        } else if ( (channel->Wreg[QDSIO_REGADDR_5] & 0x1a) == 0x0a ) {
                            // TX interrupt + TX enable + RTS => write sync mark
                            if (self->dirsrc) {
                                // Block boundary: at position 0 the ROM is
                                // rewriting the count block, otherwise a
                                // header/body block begins. The next data
                                // byte lands just past 00 16 16 A5.
                                if (self->testDiskIsWriteable())
                                    self->dirsrc->wrSyncEvent(self->image_position == 0);
                                self->image_position = 3;
                            } else {
                                self->writeByteIntoDrive(channel->Wreg[QDSIO_REGADDR_6]);
                                self->writeByteIntoDrive(channel->Wreg[QDSIO_REGADDR_7]);
                            }
                        }
                    }
                    break;

                case QDSIO_REGADDR_0:
                case QDSIO_REGADDR_1:
                case QDSIO_REGADDR_4:
                case QDSIO_REGADDR_6:
                case QDSIO_REGADDR_7:
                    break;
            }

            channel->REG_addr = QDSIO_REGADDR_0;
        }
    } else {
        // DATA write
        if (channel->name == 'A') {
            self->out_crc16 ^= dt;
            self->writeByteIntoDrive(dt);
        }
    }
    return 0;
}
