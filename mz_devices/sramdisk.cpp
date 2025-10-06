#include <cstring>
#include "common.hpp"
#include "embedded_mzf.hpp"
#include "ram_source.hpp"
#include "file_source.hpp"
#include "mzf_sram_file_source.hpp"
#include "mzf_sram_ram_source.hpp"
#include "sramdisk.hpp"

REGISTER_MZ_DEVICE(SRamDisk)

SRamDisk::SRamDisk()
{
    readMappings[0].fn = SRamDisk::resetPort;
    readMappings[1].fn  = SRamDisk::readPort;
    writeMappings[0].fn = NULL;
    writeMappings[1].fn = NULL;
    writeMappings[2].fn = SRamDisk::writePort;

    readPortCount = 2;
    writePortCount = 3;
    data = NULL;
    readOnly = false;
    firstByte = -1;
    allowBoot = true;
    size = 0;
    bs = nullptr;
}

int SRamDisk::init() {
    firstByte = -1;
    return 0;
}

int SRamDisk::readConfig(dictionary *ini) {
    if (!ini) return -1;

    allowBoot = iniparser_getboolean(ini, (getDevID() + ":allow_boot").c_str(), true);
    readOnly = iniparser_getboolean(ini, (getDevID() + ":read_only").c_str(), false);
    bool in_ram = iniparser_getboolean(ini, (getDevID() + ":in_ram").c_str(), true);
    std::string image = iniparser_getstring(ini, (getDevID() + ":image").c_str(), "@menu");
    uint16_t sz = iniparser_getint(ini, (getDevID() + ":size").c_str(), 0);
    if (sz) size = sz;
    if (!size && image.empty())
        size = SRAM_DEFAULT_SIZE;
    if (!image.empty()) {
        int ret = setDriveContent(image, in_ram);
        if (ret)
            return ret;
    }
    return 0;
}

int SRamDisk::loadMzf(const uint8_t* src, size_t src_size, bool in_ram) {
    size = std::max<size_t>(size, src_size);

    if (in_ram) {
        uint8_t* buffer = new (std::nothrow) uint8_t[src_size];
        if (!buffer) return 1;

        std::memcpy(buffer, src, src_size);
        data = buffer;
    } else {
        data = const_cast<uint8_t*>(src);
        readOnly = true;
    }

    ByteSourceFactory::from_mzf_to_sram_ram(data, size, bs);
    return 0;
}

int SRamDisk::setDriveContent(const std::string& content, bool in_ram) {
    if (content == "@menu") {
        return loadMzf(mzf_menu, sizeof(mzf_menu), in_ram);
    } 
    else if (content == "@explorer") {
        return loadMzf(mzf_explorer, sizeof(mzf_explorer), in_ram);
    } 
    else {
        readOnly = true;
        ByteSourceFactory::from_mzf_to_sram_file(content.c_str(), 128, bs);
        size = SRAM_DEFAULT_SIZE;
    }

    return 0;
}
RAM_FUNC int SRamDisk::writePort(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<SRamDisk*>(self);

    if (disk->readOnly) {
        if (disk->allowBoot && disk->bs->tell() == 0) disk->firstByte = dt;
        disk->bs->next();
        return 0;
    }

    if (disk->bs->tell() == 0) {
        if (disk->allowBoot || dt != 0xa5) disk->bs->setByte(dt);
        // don't write anything if attempting to write 0xa5 to the first byte - a hack to prevent sram detection on boot
        return 0;
    }

    disk->bs->setByte(dt);
    return 0;
}

RAM_FUNC int SRamDisk::resetPort(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    auto* disk = static_cast<SRamDisk*>(self);
    disk->bs->seek(0);
    return 0;
}

RAM_FUNC int SRamDisk::readPort(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    auto* disk = static_cast<SRamDisk*>(self);

    if (disk->readOnly && disk->allowBoot && disk->bs->tell() == 0 && disk->firstByte != -1) {
        *dt = (uint8_t)disk->firstByte;
        disk->bs->next();
        return 0;
    }

    disk->bs->getByte(*dt);
    return 0;
}

