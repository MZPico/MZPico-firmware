#include "ram_source.hpp"
#include "device.hpp"
#include "file_source.hpp"
#include "pico_rd.hpp"
#include "bus.hpp"
#include <cctype>

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

    // Initialize port mappings with defaults
    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);

    readOnly = false;
    bs = nullptr;
    size = 0;
    data = nullptr;
}

std::vector<uint8_t> PicoRD::getReadPorts() const {
    std::vector<uint8_t> ports;
    for (uint8_t i = 0; i < PICO_RD_READ_PORT_COUNT; ++i) {
        ports.push_back(PICO_RD_DEFAULT_BASE_PORT + i);
    }
    return ports;
}

std::vector<uint8_t> PicoRD::getWritePorts() const {
    std::vector<uint8_t> ports;
    for (uint8_t i = 0; i < PICO_RD_WRITE_PORT_COUNT; ++i) {
        ports.push_back(PICO_RD_DEFAULT_BASE_PORT + i);
    }
    return ports;
}

int PicoRD::init() {
    a_ = PicoRdAddr();
    return 0;
}

int PicoRD::readConfig(dictionary *ini) {
    if (!ini) return -1;

    readOnly = iniparser_getboolean(ini, (getDevID() + ":read_only").c_str(), false);
    bool in_ram = iniparser_getboolean(ini, (getDevID() + ":in_ram").c_str(), false);
    (void)in_ram;
    std::string image = iniparser_getstring(ini, (getDevID() + ":image").c_str(), "");
    // Bytes; K/M suffixes accepted ("512K", "1M"), as in [ramdisk]
    uint32_t sz = cow_parse_size(iniparser_getstring(ini, (getDevID() + ":size").c_str(), ""));
    std::string mode = iniparser_getstring(ini, (getDevID() + ":mode").c_str(), "");
    for (auto& c : mode) c = (char)tolower((unsigned char)c);
    if (mode == "cow") {
        // The card's flash-image backend, as [ramdisk] mode=cow (cow_disk.hpp):
        // whole 64 KB pages, size= omitted = the image's own size
        const int r = CowDisk::open(ini, getDevID(), image, sz, PICO_RD_DEFAULT_SIZE, &cow_);
        if (r) return r;
        size = cow_->size();
        a_.setSize(size);
        return 0;
    }
    if (!mode.empty() && mode != "file" && mode != "ram")
        printf("%s: unknown mode '%s', ignored\n", getDevID().c_str(), mode.c_str());
    if (sz) size = sz;
    if (!size && image.empty())
        size = PICO_RD_DEFAULT_SIZE;
    if (image.empty())
    {
        data = (uint8_t *)malloc(size);
        if (!data)
          return E_DEVICE_NO_MEMORY;
        ByteSourceFactory::from_ram(data, size, bs);
    } else {
       // Creating (or growing) a missing image zero-fills it through the
       // filesystem - hundreds of ms on flash:, enough to lose the cold-
       // boot IPL race (field-observed: no menu on the first boot after a
       // format). Freeze the Z80 with EXWAIT for the duration: at this
       // point of boot it is still in power-on reset or stalls on its
       // first bus cycle, so the one-time cost is invisible. (DRAM
       // refresh stalls too; harmless before the monitor has run.)
       // Creation failure disables the device like any other resource
       // shortfall - boot continues without it.
       FILINFO fno;
       const bool creating =
           (f_stat(image.c_str(), &fno) != FR_OK || fno.fsize < size);
       if (creating) set_exwait();
       const int ret = ByteSourceFactory::from_file(image, size, 128,
                                                    /* wrap =*/true, bs);
       if (creating) release_exwait();
       if (ret != 0) {
           bs.reset();
           return E_DEVICE_NO_MEMORY;
       }
    }
    // size= omitted with an image: the bound is the file's length (what the
    // file source adopted), as before
    // An empty image with size= omitted has no bound: refuse the device
    // (boot continues without it) rather than serve a disk whose every
    // access fails
    if (!a_.setSize(size, bs ? bs->size() : 0)) {
        printf("%s: image '%s' is empty and size= is not set: device skipped\n",
               getDevID().c_str(), image.c_str());
        bs.reset();
        return E_DEVICE_NO_MEMORY;
    }
    if (!size) size = a_.size;
    return 0;
}

void PicoRD::setDriveContent(std::string content, bool in_ram) {
    (void)content; (void)in_ram;
}

void PicoRD::softReset() {
    a_.control();
    if (cow_) cow_->softReset();
}

int PicoRD::flush() {
    if (cow_)
        return 0;   // the pool is RAM; learn persists after a reset (cow_disk.hpp)
    if (!bs)
        return -1;
    return bs->flush();
}

// The bus path (RAM_FUNC, core 1, the Z80 held by EXWAIT). The position is
// PicoRD's own register (pico_rd_addr.hpp); the storage is the byte source
// or the cow disk, addressed with a position below size.

RAM_FUNC int PicoRD::writeControl(MZDevice* self, uint8_t, uint8_t, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->a_.control();
    if (rd->cow_) rd->cow_->touch();
    return 0;
}

RAM_FUNC int PicoRD::readControl(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->a_.control();
    *dt = 0;
    return 0;
}

RAM_FUNC int PicoRD::writeData(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    const uint32_t p = rd->a_.take();
    if (rd->readOnly) return 0;
    if (rd->cow_) {
        rd->cow_->write(p, dt);
        return 0;
    }
    // Sequential access needs no seek (the source advanced with the last
    // byte); a seek to the append position would flush the file cache on
    // every byte
    if (rd->bs->tell() != p) rd->bs->seek(p);
    rd->bs->setByte(dt);
    return 0;
}

RAM_FUNC int PicoRD::readData(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    const uint32_t p = rd->a_.take();
    if (rd->cow_) {
        *dt = rd->cow_->read(p);
        return 0;
    }
    if (rd->bs->tell() != p) rd->bs->seek(p);
    rd->bs->getByte(*dt);
    return 0;
}

RAM_FUNC int PicoRD::writeAddr2(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->a_.setByte(2, dt);
    if (rd->cow_) rd->cow_->touch();
    return 0;
}

RAM_FUNC int PicoRD::readAddr2(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    *dt = static_cast<PicoRD*>(self)->a_.getByte(2);
    return 0;
}

RAM_FUNC int PicoRD::writeAddr1(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->a_.setByte(1, dt);
    if (rd->cow_) rd->cow_->touch();
    return 0;
}

RAM_FUNC int PicoRD::readAddr1(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    *dt = static_cast<PicoRD*>(self)->a_.getByte(1);
    return 0;
}

RAM_FUNC int PicoRD::writeAddr0(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->a_.setByte(0, dt);
    if (rd->cow_) rd->cow_->touch();
    return 0;
}

RAM_FUNC int PicoRD::readAddr0(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    *dt = static_cast<PicoRD*>(self)->a_.getByte(0);
    return 0;
}

RAM_FUNC int PicoRD::writeAddrs(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->a_.addrs(dt);
    if (rd->cow_) rd->cow_->touch();
    return 0;
}

RAM_FUNC int PicoRD::writeAddri(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* rd = static_cast<PicoRD*>(self);
    rd->a_.addri(dt);
    if (rd->cow_) rd->cow_->touch();
    return 0;
}
