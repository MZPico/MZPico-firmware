#include <cstring>
#include "common.hpp"
#include "embedded_mzf.hpp"
#include "ram_source.hpp"
#include "file_source.hpp"
#include "ramdisk.hpp"

REGISTER_MZ_DEVICE(RamDisk)

RamDisk::RamDisk()
{
    readMappings[0].fn = NULL;
    readMappings[1].fn = RamDisk::readData;
    writeMappings[0].fn = RamDisk::writePageAddress;
    writeMappings[1].fn = RamDisk::writeData;
    writeMappings[2].fn = RamDisk::writeAddress;

    readPortCount = 1;
    writePortCount = 3;
    data = NULL;
    readOnly = false;
    size = 0;
    bs = nullptr;
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
        ByteSourceFactory::from_file(image.c_str(), size, 128, true, bs);
    } else {
        data = (uint8_t *)malloc(size);
        if (!data)
            return 1;
        ByteSourceFactory::from_ram(data, size, bs);
    }
    return 0;
}

int RamDisk::flush() {
    return bs->flush();
}

RAM_FUNC int RamDisk::readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    uint32_t next_pos = 1;
    int ret;
    auto* disk = static_cast<RamDisk*>(self);

    if (disk->bs->tell() & 0xffff == 0xffff)
        next_pos = disk->bs->tell() & 0xffff0000;
    
    ret = disk->bs->getByte(*dt);

    if (next_pos != 1)
        disk->bs->seek(next_pos);
    return ret;
}

RAM_FUNC int RamDisk::writePageAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);
    dt = ((dt << 16) % disk->size) >> 16;
    return disk->bs->seek((static_cast<uint32_t>(dt) << 16) + (disk->bs->tell() & 0xffff));
}

RAM_FUNC int RamDisk::writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    uint32_t next_pos = 1;
    int ret;
    auto* disk = static_cast<RamDisk*>(self);

    if (disk->bs->tell() & 0xffff == 0xffff) {
        next_pos = disk->bs->tell() & 0xffff0000;
    }

    if (!disk->readOnly)
        ret = disk->bs->setByte(dt);
    else
        ret = disk->bs->next();

    if (next_pos != 1)
        disk->bs->seek(next_pos);

    return ret;
}

RAM_FUNC int RamDisk::writeAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);
    return disk->bs->seek(disk->bs->tell() & 0xffff0000 | (static_cast<uint32_t>(high_addr) << 8) | static_cast<uint32_t>(dt));
}