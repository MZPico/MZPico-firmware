#include <cstring>
#include "common.hpp"
#include "embedded_mzf.hpp"
#include "ram_source.hpp"
#include "file_source.hpp"
#include "ramdisk.hpp"
#include "bus.hpp"
#include "flash_fs.h"
#include "cow_resolve.hpp"

#include <cstdio>
#include <strings.h>
#include <cctype>
#include <cstdlib>

REGISTER_MZ_DEVICE(RamDisk)

// ---- COW mode bookkeeping (one COW disk per card) ----
static RamDisk* g_cow_disk = nullptr;
static char g_cow_image[64];
static char g_cow_error[96];
static volatile bool g_cow_clear_requested = false;

// "524288", "0x80000", "512K", "512k", "1M": bytes
static uint32_t parse_size(const char* s) {
    if (!s || !*s) return 0;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 0);
    while (end && *end == ' ') end++;
    if (end && (*end == 'k' || *end == 'K')) v <<= 10;
    else if (end && (*end == 'm' || *end == 'M')) v <<= 20;
    return (uint32_t)v;
}

// flash_fs moved a sector to a new page (a rewrite of the image file):
// follow it, so the table never points at a page that gets recycled
static void cow_remap_hook(uint16_t old_entry, uint16_t new_entry) {
    if (g_cow_disk) g_cow_disk->cowRemap(old_entry, new_entry);
}

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
    uint32_t sz = parse_size(iniparser_getstring(ini, (getDevID() + ":size").c_str(), ""));
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

// mode=cow: the image file in the internal flash volume, read through XIP;
// see cow_store.hpp. Any failure skips the device (boot continues, the
// reason is in /api/ramdisk) - never halts.
int RamDisk::readConfigCow(dictionary* ini, const std::string& image, uint32_t size_given) {
    const std::string id = getDevID();
    auto fail = [&](const char* why) {
        snprintf(g_cow_error, sizeof(g_cow_error), "%s", why);
        printf("%s: mode=cow disabled: %s\n", id.c_str(), g_cow_error);
        cow.release();
        return (int)E_DEVICE_NO_MEMORY;   // = skip the device, keep booting
    };
    snprintf(g_cow_image, sizeof(g_cow_image), "%s", image.c_str());
    g_cow_error[0] = '\0';
    if (g_cow_disk) return fail("only one ramdisk can be mode=cow");
    if (image.empty()) return fail("mode=cow needs image=flash:/...");
    if (strncasecmp(image.c_str(), "flash:", 6) != 0)
        return fail("the image must be on the flash volume (flash:/...)");

    uint32_t pool_kb = (uint32_t)iniparser_getint(ini, (id + ":cow_pool").c_str(),
                                                   (int)RAMDISK_COW_DEFAULT_POOL_KB);
    if (pool_kb < 1) pool_kb = 1;
    if (pool_kb > COW_MAX_SLOTS) pool_kb = COW_MAX_SLOTS;

    FIL* f = static_cast<FIL*>(malloc(sizeof(FIL)));
    uint8_t* chk = static_cast<uint8_t*>(malloc(COW_SECT));
    if (!f || !chk) { free(f); free(chk); return fail("out of RAM"); }
    if (f_open(f, image.c_str(), FA_READ) != FR_OK) {
        free(f); free(chk);
        return fail("cannot open the image");
    }
    char why[80] = "";
    uint32_t fsize = (uint32_t)f_size(f);
    // size= omitted: the image's own size (whole 64 KB pages)
    size = size_given ? ((size_given + 0xffff) & 0xffff0000) : (fsize & 0xffff0000);
    if (!size) snprintf(why, sizeof(why), "image shorter than one 64 KB page");
    else if (fsize < size) snprintf(why, sizeof(why), "image %lu B < size %lu B",
                                    (unsigned long)fsize, (unsigned long)size);
    else if (!cow.setup(size, pool_kb, flash_fs_xip_base(), &flash_fs_op_active))
        snprintf(why, sizeof(why), "out of RAM (pool %lu KB)", (unsigned long)pool_kb);
    else if (cow_resolve_file(f, cow.sectorCount(), flash_fs_map_entry, flash_fs_num_pages(),
                              cow.sectorTable(), why, sizeof(why)) == 0) {
        // Spot check the table against FatFS's own reads: every 16th
        // sector and the last (cheap enough for the cold-boot budget)
        const uint32_t n = cow.sectorCount();
        for (uint32_t i = 0; !why[0]; i = (i + 16 < n) ? i + 16 : n - 1) {
            UINT got = 0;
            if (f_lseek(f, (FSIZE_t)i * COW_SECT) != FR_OK ||
                f_read(f, chk, COW_SECT, &got) != FR_OK || got != COW_SECT) {
                snprintf(why, sizeof(why), "read error at sector %lu", (unsigned long)i);
                break;
            }
            const uint8_t* x = flash_fs_xip_base() + ((uint32_t)cow.sectorTable()[i] << COW_SECT_SHIFT);
            if (memcmp(chk, x, COW_SECT) != 0)
                snprintf(why, sizeof(why), "table check failed at sector %lu", (unsigned long)i);
            if (i == n - 1) break;
        }
    }
    f_close(f);
    free(f);
    free(chk);
    if (why[0]) return fail(why);

    cow_ = true;
    g_cow_disk = this;
    flash_fs_remap_hook = cow_remap_hook;
    printf("%s: mode=cow %s, %lu KB, pool %lu KB\n", id.c_str(), image.c_str(),
           (unsigned long)(size >> 10), (unsigned long)pool_kb);
    return 0;
}

void RamDisk::softReset() {
    pos_ = 0;
    // Z80 in reset, bus quiet: the one safe moment for a REST-armed clear
    if (cow_ && g_cow_clear_requested) {
        cow.clear();
        g_cow_clear_requested = false;
    }
}

extern "C" void ramdisk_cow_request_clear(void) {
    g_cow_clear_requested = true;
}

extern "C" int ramdisk_cow_json(char* buf, size_t len) {
    if (!g_cow_disk) {
        return snprintf(buf, len, "{\"mode\":\"%s\",\"error\":\"%s\"}",
                        g_cow_error[0] ? "cow" : "off", g_cow_error);
    }
    const CowStats& st = g_cow_disk->cowStore().stats;
    return snprintf(buf, len,
        "{\"mode\":\"cow\",\"state\":\"%s\",\"image\":\"%s\",\"size\":%lu,"
        "\"pool_blocks\":%lu,\"used\":%lu,\"peak\":%lu,\"allocs\":%lu,\"reclaims\":%lu,"
        "\"drops\":%lu,\"last_drop\":%lu,\"image_moves\":%lu,\"flash_waits\":%lu,"
        "\"clears\":%lu,\"clear_pending\":%s,\"ram\":%lu}",
        st.drops ? "pool_full" : "ok", g_cow_image, (unsigned long)st.size,
        (unsigned long)st.pool_blocks, (unsigned long)st.used, (unsigned long)st.peak,
        (unsigned long)st.allocs, (unsigned long)st.reclaims, (unsigned long)st.drops,
        (unsigned long)st.last_drop, (unsigned long)st.image_moves,
        (unsigned long)st.flash_waits, (unsigned long)st.clears,
        g_cow_clear_requested ? "true" : "false", (unsigned long)g_cow_disk->cowStore().ramBytes());
}

int RamDisk::flush() {
    if (cow_)
        return 0;   // nothing to persist: the pool is RAM, like a real MR1R18
    if (!bs)
        return -1;

    return bs->flush();
}

RAM_FUNC int RamDisk::readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);

    int ret = 0;
    if (disk->cow_) {
        *dt = disk->cow.read(disk->pos_);
    } else {
        disk->bs->seek(disk->pos_);
        ret = disk->bs->getByte(*dt);
    }
    
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

    uint32_t pageAddr = (static_cast<uint32_t>(dt) << 16) % disk->size;
    disk->pos_ = (pageAddr & 0xFFFF0000) | (disk->pos_ & 0xffff);
    return 0;
}

RAM_FUNC int RamDisk::writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);

    int ret = 0;
    if (disk->cow_) {
        if (!disk->readOnly)
            disk->cow.write(disk->pos_, dt);
    } else {
        disk->bs->seek(disk->pos_);
        if (!disk->readOnly)
            ret = disk->bs->setByte(dt);
    }
    
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

// Diagnostic: reading the address port echoes the high address byte (B when
// using IN r,(C)). Verifies 16-bit read addressing end to end (rdtest.mzf).
RAM_FUNC int RamDisk::readHighAddr(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    *dt = high_addr;
    return 0;
}