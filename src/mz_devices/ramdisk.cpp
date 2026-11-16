#include <cstring>
#include "common.hpp"
#include "embedded_mzf.hpp"
#include "ram_source.hpp"
#include "file_source.hpp"
#include "ramdisk.hpp"
#include "bus.hpp"
#include "flash_fs.h"
#include "cow_resolve.hpp"
#include "cow_image.hpp"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "ramdisk_pos.hpp"

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
static volatile bool g_cow_relearn_req = false;
static volatile bool g_cow_freeze_req = false;
static volatile bool g_cow_unfreeze_req = false;

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

// flash_fs is about to drop sectors (FatFS trim: a delete or truncate)
static void cow_trim_hook(uint32_t first, uint32_t count) {
    if (g_cow_disk) g_cow_disk->cowTrim(first, count);
}

// The holes of a blank (learn) image read from this page: 512 zero bytes
// in flash, page-aligned so a sector-table entry can name it
__attribute__((section(".rodata.cow_zero_page"), aligned(512)))
const uint8_t g_cow_zero_page[512] = {0};

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
// see cow_store.hpp, and cow_image.hpp for learn. Any failure skips the
// device (boot continues, the reason is in /api/ramdisk) - never halts.
static uint32_t cow_now_us(void*) { return time_us_32(); }

static int ini_int(dictionary* ini, const std::string& key, int dflt, int lo, int hi) {
    int v = iniparser_getint(ini, key.c_str(), dflt);
    return v < lo ? lo : (v > hi ? hi : v);
}

int RamDisk::readConfigCow(dictionary* ini, const std::string& image, uint32_t size_given) {
    const std::string id = getDevID();
    auto fail = [&](const char* why) {
        snprintf(g_cow_error, sizeof(g_cow_error), "%s", why);
        printf("%s: mode=cow disabled: %s\n", id.c_str(), g_cow_error);
        cow.release();
        cowImage.close();
        return (int)E_DEVICE_NO_MEMORY;   // = skip the device, keep booting
    };
    // Kept JSON-escaped: it is only ever printed into the REST state
    size_t n = 0;
    for (const char* p = image.c_str(); *p && n + 2 < sizeof(g_cow_image); ++p) {
        if (*p == '\\' || *p == '"') g_cow_image[n++] = '\\';
        else if ((unsigned char)*p < 0x20) continue;
        g_cow_image[n++] = *p;
    }
    g_cow_image[n] = '\0';
    g_cow_error[0] = '\0';
    if (g_cow_disk) return fail("only one ramdisk can be mode=cow");
    if (image.empty()) return fail("mode=cow needs image=flash:/...");
    if (strncasecmp(image.c_str(), "flash:", 6) != 0)
        return fail("the image must be on the flash volume (flash:/...)");

    const uint32_t pool_kb = (uint32_t)ini_int(ini, id + ":cow_pool",
                                               (int)RAMDISK_COW_DEFAULT_POOL_KB, 1, (int)COW_MAX_SLOTS);
    // learn: omitted = auto (on for an image the card created, off for one
    // prepared elsewhere); "frozen" = a learn image whose write-back is off
    std::string lv = iniparser_getstring(ini, (id + ":learn").c_str(), "");
    for (auto& c : lv) c = (char)tolower((unsigned char)c);
    int learn = -1;
    if (lv == "true" || lv == "1" || lv == "yes" || lv == "on" || lv == "frozen") learn = 1;
    else if (lv == "false" || lv == "0" || lv == "no" || lv == "off") learn = 0;

    CowImageCfg cfg{};
    cfg.path = image.c_str();
    cfg.size = size_given ? ((size_given + 0xffff) & 0xffff0000) : 0;
    cfg.default_size = RAMDISK_DEFAULT_SIZE;
    cfg.pool_blocks = pool_kb;
    cfg.learn = learn;
    cfg.flash_base = reinterpret_cast<const uint8_t*>(XIP_BASE);
    cfg.zero_page = g_cow_zero_page;
    cfg.busy = &flash_fs_op_active;

    // A missing or short image is created (blank) here: hold the Z80 in
    // EXWAIT meanwhile (cold-boot IPL race, see pico_rd.cpp). Creating is
    // cheap - clusters allocated, pages trimmed, no data written.
    FILINFO fno;
    const bool may_create = learn != 0 &&
        (f_stat(image.c_str(), &fno) != FR_OK || (cfg.size && fno.fsize < cfg.size));
    if (may_create) set_exwait();
    char why[96];
    const int r = cowImage.open(cfg, cow, why, sizeof(why), cow_now_us, nullptr);
    if (may_create) release_exwait();
    if (r != 0) return fail(why);
    size = cow.stats.size;

    CowLearnCfg lc;
    lc.batch = (uint32_t)ini_int(ini, id + ":learn_batch", (int)lc.batch, 1, 32);
    lc.reserve = (uint32_t)ini_int(ini, id + ":learn_reserve", (int)lc.reserve, 0, (int)pool_kb);
    lc.cap = (uint32_t)ini_int(ini, id + ":learn_cap", (int)lc.cap, 0, 1 << 20);
    lc.sync_every = (uint32_t)ini_int(ini, id + ":learn_sync", (int)lc.sync_every, 1, 4096);
    learnPolicy_.converge = (uint16_t)ini_int(ini, id + ":learn_converge", learnPolicy_.converge, 1, 1000);
    learnPolicy_.freeze_after = (uint16_t)ini_int(ini, id + ":learn_freeze_after", 0, 0, 1000);
    cow.setLearn(cowImage.io(), lc);
    if (lv == "frozen" && cowImage.mode() != COW_LEARN_FROZEN) cowImage.freeze(cow, true);
    cow.setWriteBack(cowImage.learning());

    cow_ = true;
    g_cow_disk = this;
    flash_fs_remap_hook = cow_remap_hook;
    flash_fs_trim_hook = cow_trim_hook;
    printf("%s: mode=cow %s, %lu KB, pool %lu KB, learn %s%s\n", id.c_str(), image.c_str(),
           (unsigned long)(size >> 10), (unsigned long)pool_kb,
           cow_learn_mode_name(cowImage.mode()), cowImage.created() ? " (created)" : "");
    return 0;
}

void RamDisk::softReset() {
    pos_ = 0;
    if (!cow_) return;
    // Z80 in reset, bus quiet: the one safe moment for REST-armed changes
    // and the session's end. Flash work here (the trailer, a map sync, a
    // relearn's sync: ~50-100 ms) holds EXWAIT so a short reset pulse
    // cannot let the Z80 run into an unserved bus; the bus loop releases it.
    if (get_core_num() != 1) return;
    set_exwait();
    if (g_cow_freeze_req) { g_cow_freeze_req = false; cowImage.freeze(cow, true); }
    if (g_cow_unfreeze_req) { g_cow_unfreeze_req = false; cowImage.freeze(cow, false); }
    cowImage.endSession(cow, learnPolicy_, active_);
    active_ = false;
    if (g_cow_relearn_req) {
        g_cow_relearn_req = false;
        cowImage.relearn(cow);
        g_cow_clear_requested = false;
    }
    if (g_cow_clear_requested) {
        cow.clear();
        g_cow_clear_requested = false;
    }
}

extern "C" void ramdisk_cow_request_clear(void) {
    g_cow_clear_requested = true;
}

extern "C" int ramdisk_cow_request(const char* what) {
    if (!g_cow_disk || g_cow_disk->cowImageRef().mode() == COW_LEARN_OFF) return -1;
    if (!strcmp(what, "relearn")) g_cow_relearn_req = true;
    else if (!strcmp(what, "freeze")) g_cow_freeze_req = true;
    else if (!strcmp(what, "unfreeze")) g_cow_unfreeze_req = true;
    else return -1;
    return 0;
}

static const char* cow_state(const CowStats& st) {
    // image_changed (blocks lost, until power-up) outranks pool_full
    // (writes dropped, until a clear)
    return st.lost ? "image_changed" : (st.pool_full ? "pool_full" : "ok");
}

// learn's state as shown: a converged image that is writing back again is
// learning again (persisted at the next reset)
static const char* cow_learn_state(const CowImage& im, const CowStore& cs) {
    if (im.mode() == COW_LEARN_OFF) return "off";
    if (im.gone()) return "image_gone";
    if (cs.learn.capped && cs.tokens() == 0) return "capped";
    if (cs.learn.fails) return "flash_full";
    if (im.mode() == COW_LEARN_OK && cs.learn.session_blocks) return "learning";
    return cow_learn_mode_name(im.mode());
}

extern "C" int ramdisk_cow_json(char* buf, size_t len) {
    if (!g_cow_disk) {
        return snprintf(buf, len, "{\"mode\":\"%s\",\"error\":\"%s\"}",
                        g_cow_error[0] ? "cow" : "off", g_cow_error);
    }
    const CowStore& cs = g_cow_disk->cowStore();
    const CowStats& st = cs.stats;
    return snprintf(buf, len,
        "{\"mode\":\"cow\",\"state\":\"%s\",\"learn\":\"%s\",\"used\":%lu,\"peak\":%lu,"
        "\"drops\":%lu,\"lost\":%lu,\"wb_blocks\":%lu,\"ram\":%lu}",
        cow_state(st), cow_learn_state(g_cow_disk->cowImageRef(), cs),
        (unsigned long)st.used, (unsigned long)st.peak, (unsigned long)st.drops,
        (unsigned long)st.lost, (unsigned long)cs.learn.blocks, (unsigned long)cs.ramBytes());
}

extern "C" int ramdisk_cow_json_full(char* buf, size_t len) {
    if (!g_cow_disk) return ramdisk_cow_json(buf, len);
    const CowStore& cs = g_cow_disk->cowStore();
    const CowImage& im = g_cow_disk->cowImageRef();
    const CowStats& st = cs.stats;
    const CowLearnStats& L = cs.learn;
    const CowTrailer& t = im.trailer();
    // Wear: lifetime erases of the learn path over the volume's data
    // sectors, in parts per million of a 100,000-cycle endurance
    const uint32_t data_sectors = flash_fs_num_pages() / 8 - 2;
    const uint64_t erases_total = (uint64_t)t.erases + im.sessionErases();
    const unsigned long wear_ppm = (unsigned long)(erases_total * 10u / (data_sectors ? data_sectors : 1));
    int n = snprintf(buf, len,
        "{\"mode\":\"cow\",\"state\":\"%s\",\"image\":\"%s\",\"size\":%lu,"
        "\"pool_blocks\":%lu,\"used\":%lu,\"peak\":%lu,\"allocs\":%lu,\"reclaims\":%lu,"
        "\"drops\":%lu,\"last_drop\":%lu,\"image_moves\":%lu,\"preserved\":%lu,"
        "\"lost\":%lu,\"pinned\":%lu,\"out_of_range\":%lu,\"flash_waits\":%lu,"
        "\"clears\":%lu,\"clear_pending\":%s,\"ram\":%lu,",
        cow_state(st), g_cow_image, (unsigned long)st.size,
        (unsigned long)st.pool_blocks, (unsigned long)st.used, (unsigned long)st.peak,
        (unsigned long)st.allocs, (unsigned long)st.reclaims, (unsigned long)st.drops,
        (unsigned long)st.last_drop, (unsigned long)st.image_moves,
        (unsigned long)st.preserved, (unsigned long)st.lost, (unsigned long)cs.pinnedCount(),
        (unsigned long)st.out_of_range,
        (unsigned long)st.flash_waits, (unsigned long)st.clears,
        g_cow_clear_requested ? "true" : "false", (unsigned long)cs.ramBytes());
    if (n < 0 || (size_t)n >= len) return n;
    n += snprintf(buf + n, len - (size_t)n,
        "\"learn\":{\"state\":\"%s\",\"created\":%s,\"create_ms\":%lu,\"dirty\":%lu,"
        "\"writebacks\":%lu,\"blocks\":%lu,\"ahead\":%lu,\"sectors\":%lu,\"flash_bytes\":%lu,"
        "\"fails\":%lu,\"stall_ms\":%lu,\"stall_max_ms\":%lu,\"syncs\":%lu,\"erases\":%lu,"
        "\"tokens\":%lu,\"cap_drops\":%lu,\"session_blocks\":%lu,"
        "\"sessions\":%lu,\"clean_sessions\":%u,\"dirty_sessions\":%u,\"relearns\":%u,"
        "\"blocks_total\":%lu,\"erases_total\":%lu,\"wear_ppm\":%lu,"
        "\"fs_erases\":%lu,\"fs_copy_erases\":%lu,\"fs_map_syncs\":%lu,\"pending\":\"%s\"}}",
        cow_learn_state(im, cs), im.created() ? "true" : "false",
        (unsigned long)(im.createUs() / 1000), (unsigned long)cs.dirtyCount(),
        (unsigned long)L.batches, (unsigned long)L.blocks, (unsigned long)L.ahead,
        (unsigned long)L.sectors, (unsigned long)L.sectors * 512u, (unsigned long)L.fails,
        (unsigned long)(L.stall_us / 1000), (unsigned long)(L.stall_max_us / 1000),
        (unsigned long)L.syncs, (unsigned long)im.erases(),
        (unsigned long)cs.tokens(), (unsigned long)L.cap_drops, (unsigned long)L.session_blocks,
        (unsigned long)t.sessions, (unsigned)t.clean, (unsigned)t.dirty, (unsigned)t.relearns,
        (unsigned long)(t.wb_blocks + L.session_blocks), (unsigned long)erases_total, wear_ppm,
        (unsigned long)flash_fs_erases, (unsigned long)flash_fs_copy_erases,
        (unsigned long)flash_fs_map_syncs,
        g_cow_relearn_req ? "relearn" : (g_cow_freeze_req ? "freeze" : (g_cow_unfreeze_req ? "unfreeze" : "")));
    return n;
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
        *dt = disk->cow.read(disk->pos_);
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
    disk->active_ = true;

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
    disk->pos_ = mr1r18_next_pos(disk->pos_);
    
    return ret;
}

RAM_FUNC int RamDisk::writeAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* disk = static_cast<RamDisk*>(self);
    disk->active_ = true;
    disk->pos_ = (disk->pos_ & 0xffff0000) | (static_cast<uint32_t>(high_addr) << 8) | static_cast<uint32_t>(dt);
    return 0;
}

// Diagnostic: reading the address port echoes the high address byte (B when
// using IN r,(C)). Verifies 16-bit read addressing end to end (rdtest.mzf).
RAM_FUNC int RamDisk::readHighAddr(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    *dt = high_addr;
    return 0;
}