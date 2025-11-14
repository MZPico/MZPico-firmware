#include "qd.hpp"
#include <cerrno>
#include <sys/stat.h>
#include "ff.h"
#include "iniparser.h"
#include "file_source.hpp"
#include "qd_dir_source.hpp"

REGISTER_MZ_DEVICE(QDDevice)

// -------------------------------- Lifecycle --------------------------------

QDDevice::QDDevice() {
    for (int i = 0; i < readPortCount; i++)
        readMappings[i].fn  = QDDevice::readByte;
    for (int i = 0; i < writePortCount; i++)
        writeMappings[i].fn  = QDDevice::writeByte;
    readPortCount = QD_PORTS;
    writePortCount = QD_PORTS;
    bs = nullptr;
}

QDDevice::~QDDevice() { close(); }

int QDDevice::init() {

    memset(channel, 0, sizeof(channel));
    memset(&image_file, 0, sizeof(image_file));
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

    status = QDSTS_NO_DISC;
    driveReset();
    close();

    if (stdPath.empty()) {
        status = QDSTS_NO_DISC;
        return;
    }

    FRESULT fr = f_stat(stdPath.c_str(), &fno);
    if (fr != FR_OK)
        return;

    status = QDSTS_IMG_READY | QDSTS_HEAD_HOME;
    if (fno.fattrib & AM_DIR) {
        ByteSourceFactory::from_qddir(stdPath, 128, bs);
    } else {
        ByteSourceFactory::from_file(stdPath, 0, 128, /* wrap = */false, bs);
    }


/*
    unsigned roflag = writeProtected ? QDSTS_IMG_READONLY : 0;

    if (FR_OK == f_open(&image_file, stdPath.c_str(), (writeProtected ? FA_READ : (FA_READ | FA_WRITE)))) {
        status = QDSTS_IMG_READY | QDSTS_HEAD_HOME | roflag;
    } else {
        // fallback to RO
        if (FR_OK == f_open(&image_file, stdPath.c_str(), FA_READ)) {
            status = QDSTS_IMG_READY | QDSTS_HEAD_HOME | QDSTS_IMG_READONLY;
        } else {
            std::fprintf(stderr, "QuickDisk: Can't open file '%s': %s\n",
                         stdPath.c_str(), std::strerror(errno));
        }
    }
*/
}

void QDDevice::close() {
    if (connected == QDISK_CONNECTED) {
        if (status & QDSTS_IMG_READY) {
            if (bs->flush() != 0)  {
                std::fprintf(stderr, "QuickDisk: fsync() error: %s\n", std::strerror(errno));
            }
            //f_close(&image_file);
            status &= ~QDSTS_IMG_READY;
        }
    }
}

// ----------------------------- Data path (read) ----------------------------

uint8_t QDDevice::readByteFromDrive() {
    uint32_t readlen;
    uint8_t retval;

    if ((status & QDSTS_IMG_READY) == 0) return 0xff;            // no media
    if ((channel[QDSIO_CHANNEL_B].Wreg[QDSIO_REGADDR_5] & 0x80) == 0x00) return 0xff; // motor off

    if (QDISK_IMAGE_MAX_SIZE <= image_position) {
        if (QDISK_IMAGE_MAX_SIZE == image_position) image_position++;
        return 0xff;
    }

    bs->getByte(retval);
    //bs->get(&retval, 1, readlen);
    //if (readlen != 1) std::fprintf(stderr, "QuickDisk: fread() error\n");
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
    uint32_t len;

    if (0 == testDiskIsWriteable()) return;

    if (QDISK_IMAGE_MAX_SIZE <= image_position) {
        if (QDISK_IMAGE_MAX_SIZE == image_position) image_position++;
        return;
    };
    bs->setByte(value);
    //if (bs->set(&value, 1, len) != 0) {
    //    std::fprintf(stderr, "QuickDisk: fwrite() error: %s\n", std::strerror(errno));
    //};
    image_position++;
    return;
}

// -------------------------------- Registers --------------------------------

int QDDevice::readByte(MZDevice* self_, uint8_t port, uint8_t *dt, uint8_t /*high_addr*/) {
    auto* self = static_cast<QDDevice*>(self_);
    uint8_t SIO_addr = port & 0x03;
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

            if ( (channel[QDSIO_CHANNEL_A].Wreg[QDSIO_REGADDR_5] & 0x1a) == 0x0a ) {
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
            if (self->status & QDSTS_IMG_READY) *dt = self->readByteFromDrive();
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
    st_QDSIO_CHANNEL* channel = &self->channel[port & 0x01];

    if (port & 0x02) {
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
                            // TX interrupt + TX enable
                            self->writeByteIntoDrive(0x00);
                        } else if ( (channel->Wreg[QDSIO_REGADDR_5] & 0x1a) == 0x0a ) {
                            // TX interrupt + TX enable + RTS => write sync mark
                            self->writeByteIntoDrive(channel->Wreg[QDSIO_REGADDR_6]);
                            self->writeByteIntoDrive(channel->Wreg[QDSIO_REGADDR_7]);
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
