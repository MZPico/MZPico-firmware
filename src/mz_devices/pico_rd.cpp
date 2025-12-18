#include "ram_source.hpp"
#include "device.hpp"
#include "file_source.hpp"
#include "pico_rd.hpp"

REGISTER_MZ_DEVICE(PicoRD)

PicoRD::PicoRD()
{
    readMappings[PICO_RD_CONTROL_PORT_INDEX].fn = PicoRD::readControl;
    writeMappings[PICO_RD_CONTROL_PORT_INDEX].fn = PicoRD::writeControl;

    readMappings[PICO_RD_DATA_PORT_INDEX].fn = PicoRD::readData;
    writeMappings[PICO_RD_DATA_PORT_INDEX].fn = PicoRD::writeData;

    readMappings[PICO_RD_ADDR2_PORT_INDEX].fn = PicoRD::readAddr2;
    writeMappings[PICO_RD_ADDR2_PORT_INDEX].fn = PicoRD::writeAddr2;

    readMappings[PICO_RD_ADDR1_PORT_INDEX].fn = PicoRD::readAddr1;
    writeMappings[PICO_RD_ADDR1_PORT_INDEX].fn = PicoRD::writeAddr1;

    readMappings[PICO_RD_ADDR0_PORT_INDEX].fn = PicoRD::readAddr0;
    writeMappings[PICO_RD_ADDR0_PORT_INDEX].fn = PicoRD::writeAddr0;

    writeMappings[PICO_RD_ADDRS_PORT_INDEX].fn = PicoRD::writeAddrs;
    writeMappings[PICO_RD_ADDRI_PORT_INDEX].fn = PicoRD::writeAddri;

    readPortCount = PICO_RD_READ_PORT_COUNT;
    writePortCount = PICO_RD_WRITE_PORT_COUNT;

    readOnly = false;
    bs = nullptr;
    size = 0;
    data = nullptr;
}

int PicoRD::init() {
    addr_idx = 0;
    return 0;
}

int PicoRD::readConfig(dictionary *ini) {
    if (!ini) return -1;

    readOnly = iniparser_getboolean(ini, (getDevID() + ":read_only").c_str(), false);
    bool in_ram = iniparser_getboolean(ini, (getDevID() + ":in_ram").c_str(), false);
    std::string image = iniparser_getstring(ini, (getDevID() + ":image").c_str(), "");
    uint32_t sz = iniparser_getint(ini, (getDevID() + ":size").c_str(), 0);
    if (sz) size = sz;
    if (!size && image.empty())
        size = PICO_RD_DEFAULT_SIZE;
    if (image.empty())
    {
        data = (uint8_t *)malloc(size);
        if (!data)
          return 1;
        ByteSourceFactory::from_ram(data, size, bs);
    } else {
       ByteSourceFactory::from_file(image, size, 128, /* wrap =*/true, bs);
       //     setDriveContent(image, in_ram);
    }
    return 0;
}

void PicoRD::setDriveContent(std::string content, bool in_ram) {
/*    if (content == "@menu") {
        if (size < sizeof(firmware)) size = sizeof(firmware);
        if (in_ram) {
            std::memcpy(data, firmware, sizeof(firmware));
            data = (uint8_t *)malloc(sizeof(firmware));
        } else {
            data = (uint8_t *)firmware;
            size = sizeof(firmware);
            readOnly = true;
        }
    }
    */
}

int PicoRD::flush() {
    if (!bs)
        return -1;
    
    return bs->flush();
}

int PicoRD::writeControl(MZDevice* self, uint8_t, uint8_t, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->bs->seek(0);
    rd->addr_idx = 0;
    return 0;
}

int PicoRD::readControl(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->bs->seek(0);
    rd->addr_idx = 0;
    *dt = 0;
    return 0;
}

int PicoRD::writeData(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    uint32_t wtn;

    if (rd->readOnly)
        rd->bs->next();
    else
        rd->bs->setByte(dt);
    rd->addr_idx = 0;
    return 0;
}

int PicoRD::readData(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    uint32_t br;

    rd->bs->getByte(*dt);
    rd->addr_idx = 0;
    return 0;
}

int PicoRD::writeAddr2(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->bs->seek((rd->bs->tell() & 0x00FFffff) | ((uint32_t)dt << 16));
    rd->addr_idx = 0;
    return 0;
}

int PicoRD::readAddr2(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    *dt = (rd->bs->tell() >> 16) & 0xFF;
    rd->addr_idx = 0;
    return 0;
}

int PicoRD::writeAddr1(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->bs->seek((rd->bs->tell() & 0xFF00ffff) | ((uint32_t)dt << 8));
    rd->addr_idx = 0;
    return 0;
}

int PicoRD::readAddr1(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    *dt = (rd->bs->tell() >> 8) & 0xFF;
    rd->addr_idx = 0;
    return 0;
}

int PicoRD::writeAddr0(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->bs->seek((rd->bs->tell() & 0xFFFFff00) | dt);
    rd->addr_idx = 0;
    return 0;
}

int PicoRD::readAddr0(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    *dt = rd->bs->tell() & 0xFF;
    rd->addr_idx = 0;
    return 0;
}

int PicoRD::writeAddrs(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->bs->seek((rd->bs->tell() & ~(0xFF << (rd->addr_idx * 8))) | ((uint32_t)dt << (rd->addr_idx * 8)));
    rd->addr_idx++;
    if (rd->addr_idx > 2) rd->addr_idx = 0;
    return 0;
}

int PicoRD::writeAddri(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);

    uint32_t new_index = rd->bs->tell() + dt;
    if (new_index >= rd->size) new_index -= rd->size;
    rd->bs->seek(new_index);
    return 0;
}

