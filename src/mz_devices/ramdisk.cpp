#include <cstring>
#include "common.hpp"
#include "embedded_mzf.hpp"
#include "ram_source.hpp"
#include "file_source.hpp"
#include "ramdisk.hpp"
#include "bus.hpp"
#include "flash_fs.h"
#include "cow_resolve.hpp"
#include "cow_disk.hpp"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "ramdisk_pos.hpp"

#include <cstdio>
#include <strings.h>
#include <cctype>
#include <cstdlib>

REGISTER_MZ_DEVICE(RamDisk)

RamDisk::RamDisk()
{
    readMappings[0].fn = RamDisk::resetCounter;
    readMappings[1].fn = RamDisk::readData;
    readMappings[2].fn = RamDisk::readHighAddr;
    writeMappings[0].fn = RamDisk::writePageAddress;
    writeMappings[1].fn = RamDisk::writeData;
    writeMappings[2].fn = RamDisk::writeAddress;

    // Initialize port mappings with defaults
    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);
    data = NULL;
    readOnly = false;
    size = 0;
    pos_ = 0;
    bs = nullptr;
}

std::vector<uint8_t> RamDisk::getReadPorts() const {
    return {0xf8, RAMDISK_DEFAULT_BASE_PORT + 1, RAMDISK_DEFAULT_BASE_PORT + 2};
}

std::vector<uint8_t> RamDisk::getWritePorts() const {
    return {RAMDISK_DEFAULT_BASE_PORT, RAMDISK_DEFAULT_BASE_PORT + 1, RAMDISK_DEFAULT_BASE_PORT + 2};
}

// Keep 0xf8 reset port fixed, shift only data interface ports
std::pair<std::vector<uint8_t>, std::vector<uint8_t>> RamDisk::applyBasePort(uint8_t basePort) const {
    std::vector<uint8_t> readPorts = {0xf8, static_cast<uint8_t>(basePort + 1), static_cast<uint8_t>(basePort + 2)};
    std::vector<uint8_t> writePorts = {basePort, static_cast<uint8_t>(basePort + 1), static_cast<uint8_t>(basePort + 2)};
    return {readPorts, writePorts};
}

int RamDisk::init() {
    return 0;
}

int RamDisk::readConfig(dictionary *ini) {
    if (!ini) return -1;

    readOnly = iniparser_getboolean(ini, (getDevID() + ":read_only").c_str(), false);
    std::string image = iniparser_getstring(ini, (getDevID() + ":image").c_str(), "");
    uint32_t sz = cow_parse_size(iniparser_getstring(ini, (getDevID() + ":size").c_str(), ""));
    std::string mode = iniparser_getstring(ini, (getDevID() + ":mode").c_str(), "");
    for (auto& c : mode) c = (char)tolower((unsigned char)c);
    if (mode == "cow")
        return readConfigCow(ini, image, sz);
    if (!mode.empty() && mode != "file" && mode != "ram")
        printf("%s: unknown mode '%s', ignored\n", getDevID().c_str(), mode.c_str());
    if (sz) size = (sz + 0xffff) & 0xffff0000; // align to 65536 multiples
    if (!size)
        size = RAMDISK_DEFAULT_SIZE;
    if (!image.empty()) {
        // Missing/short image: freeze the Z80 with EXWAIT while creating
        // it - see pico_rd.cpp for the full rationale (cold-boot IPL race)
        FILINFO fno;
        const bool creating =
            (f_stat(image.c_str(), &fno) != FR_OK || fno.fsize < size);
        if (creating) set_exwait();
        const int ret = ByteSourceFactory::from_file(image.c_str(), size, 128,
                                                     /* wrap= */ false, bs,
                                                     /* auto_increment= */ false);
        if (creating) release_exwait();
        if (ret != 0) {
            bs.reset();
            return E_DEVICE_NO_MEMORY;
        }
    } else {
        data = (uint8_t *)malloc(size);
        if (!data)
            return E_DEVICE_NO_MEMORY;
        ByteSourceFactory::from_ram(data, size, bs, /* auto_increment= */ false);
    }
    return 0;
}

// mode=cow: the card's flash-image backend (cow_disk.hpp); any failure
// skips the device (the reason in /api/ramdisk) - never halts
int RamDisk::readConfigCow(dictionary* ini, const std::string& image, uint32_t size_given) {
    const int r = CowDisk::open(ini, getDevID(), image, size_given, RAMDISK_DEFAULT_SIZE, &cow_);
    if (r) return r;
    size = cow_->size();
    return 0;
}

void RamDisk::softReset() {
    pos_ = 0;
    if (cow_) cow_->softReset();
}

int RamDisk::flush() {
    if (cow_)
        return 0;   // the pool is RAM, like a real MR1R18; learn persists at softReset()
    if (!bs)
        return -1;

    return bs->flush();
}

RAM_FUNC int RamDisk::readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);

    int ret = 0;
    if (disk->cow_) {
        *dt = disk->cow_->read(disk->pos_);
    } else {
        disk->bs->seek(disk->pos_);
        ret = disk->bs->getByte(*dt);
    }
    
    // Increment with 64K page wrapping
    disk->pos_ = mr1r18_next_pos(disk->pos_);
    
    return ret;
}

RAM_FUNC int RamDisk::resetCounter(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);
    *dt = 0;
    disk->pos_ = 0;
    return 0;
}

RAM_FUNC int RamDisk::writePageAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);
    // learn: session activity, and the deferred flash work after a reset
    // (the Z80 held by EXWAIT here, as for a write-back)
    if (disk->cow_) disk->cow_->touch();

    uint32_t pageAddr = (static_cast<uint32_t>(dt) << 16) % disk->size;
    disk->pos_ = (pageAddr & 0xFFFF0000) | (disk->pos_ & 0xffff);
    return 0;
}

RAM_FUNC int RamDisk::writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);

    int ret = 0;
    if (disk->cow_) {
        if (!disk->readOnly)
            disk->cow_->write(disk->pos_, dt);
    } else {
        disk->bs->seek(disk->pos_);
        if (!disk->readOnly)
            ret = disk->bs->setByte(dt);
    }
    
    // Increment with 64K page wrapping
    disk->pos_ = mr1r18_next_pos(disk->pos_);
    
    return ret;
}

RAM_FUNC int RamDisk::writeAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);
    // learn: session activity, and the deferred flash work after a reset
    // (the Z80 held by EXWAIT here, as for a write-back)
    if (disk->cow_) disk->cow_->touch();
    disk->pos_ = (disk->pos_ & 0xffff0000) | (static_cast<uint32_t>(high_addr) << 8) | static_cast<uint32_t>(dt);
    return 0;
}

// Diagnostic: reading the address port echoes the high address byte (B when
// using IN r,(C)). Verifies 16-bit read addressing end to end (rdtest.mzf).
RAM_FUNC int RamDisk::readHighAddr(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    *dt = high_addr;
    return 0;
}