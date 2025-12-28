#include <cstring>
#include "common.hpp"
#include "embedded_mzf.hpp"
#include "ram_source.hpp"
#include "file_source.hpp"
#include "ramdisk.hpp"

REGISTER_MZ_DEVICE(RamDisk)

RamDisk::RamDisk()
{
    readMappings[0].fn = RamDisk::resetCounter;
    readMappings[1].fn = RamDisk::readData;
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
    return {0xf8, RAMDISK_DEFAULT_BASE_PORT + 1};
}

std::vector<uint8_t> RamDisk::getWritePorts() const {
    return {RAMDISK_DEFAULT_BASE_PORT, RAMDISK_DEFAULT_BASE_PORT + 1, RAMDISK_DEFAULT_BASE_PORT + 2};
}

// Keep 0xf8 reset port fixed, shift only data interface ports
std::pair<std::vector<uint8_t>, std::vector<uint8_t>> RamDisk::applyBasePort(uint8_t basePort) const {
    std::vector<uint8_t> readPorts = {0xf8, static_cast<uint8_t>(basePort + 1)};
    std::vector<uint8_t> writePorts = {basePort, static_cast<uint8_t>(basePort + 1), static_cast<uint8_t>(basePort + 2)};
    return {readPorts, writePorts};
}

int RamDisk::init() {
    return 0;
}

int RamDisk::readConfig(dictionary *ini) {
    if (!ini) return -1;

    readOnly = iniparser_getboolean(ini, (getDevID() + ":read_only").c_str(), false);
    bool in_ram = iniparser_getboolean(ini, (getDevID() + ":in_ram").c_str(), true);
    std::string image = iniparser_getstring(ini, (getDevID() + ":image").c_str(), "");
    uint32_t sz = iniparser_getint(ini, (getDevID() + ":size").c_str(), 0);
    if (sz) size = (sz + 0xffff) & 0xffff0000; // align to 65536 multiples
    if (!size)
        size = RAMDISK_DEFAULT_SIZE;
    if (!image.empty()) {
        ByteSourceFactory::from_file(image.c_str(), size, 128, /* wrap= */ false, bs, /* auto_increment= */ false);
    } else {
        data = (uint8_t *)malloc(size);
        if (!data)
            return 1;
        ByteSourceFactory::from_ram(data, size, bs, /* auto_increment= */ false);
    }
    return 0;
}

int RamDisk::flush() {
    return bs->flush();
}

RAM_FUNC int RamDisk::readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);
    
    disk->bs->seek(disk->pos_);
    int ret = disk->bs->getByte(*dt);
    
    // Increment with 64K page wrapping
    disk->pos_++;
    if ((disk->pos_ & 0xffff) == 0)
        disk->pos_ &= 0xffff0000;
    
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
    dt = ((dt << 16) % disk->size) >> 16;
    disk->pos_ = (static_cast<uint32_t>(dt) << 16) | (disk->pos_ & 0xffff);
    return 0;
}

RAM_FUNC int RamDisk::writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);
    
    disk->bs->seek(disk->pos_);
    
    int ret;
    if (!disk->readOnly)
        ret = disk->bs->setByte(dt);
    
    // Increment with 64K page wrapping
    disk->pos_++;
    if ((disk->pos_ & 0xffff) == 0)
        disk->pos_ &= 0xffff0000;
    
    return ret;
}

RAM_FUNC int RamDisk::writeAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);
    disk->pos_ = (disk->pos_ & 0xffff0000) | (static_cast<uint32_t>(high_addr) << 8) | static_cast<uint32_t>(dt);
    return 0;
}