/**
 *    _   ___ ___ _       ___          _   
 *   /_\ ( _ ) _ (_)__ _ / __|__ _ _ _| |_ 
 *  / _ \/ _ \  _/ / _/_\ (__/ _` | '_|  _|
 * /_/ \_\___/_| |_\__\_/\___\__,_|_|  \__|
 *                                         
 * 
 * Atari 8-bit cartridge for Raspberry Pi Pico
 *
 * Robin Edwards 2023
 *
 * Needs to be a release NOT debug build for the cartridge emulation to work
 */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include <stdio.h>
#include <string.h>

#include "flash_fs.h"

// Implements 512 byte FAT sectors on 4096 byte flash sectors.
//
// Two on-flash formats:
// - Legacy "RHE!FS30" (A8PicoCart): the map lives in the first FLASH_MEGS
//   flash sectors and is erased+rewritten IN PLACE on every sync - a power
//   cut in that window loses the whole map (and with it every file).
//   Legacy volumes still mount and work, with that documented risk.
// - Current "MZP1": each map meg has TWO slots (sectors 2i and 2i+1) with a
//   generation counter and checksum; a sync writes the inactive slot and
//   the old one stays valid until the new one is complete, so a torn sync
//   costs at most the changes since the previous sync, never the volume.
//
// Access is single-context by design: core 1 in MZ-800 mode, core 0 thread
// context in USB mode (the boot mode gate in main.cpp makes them exclusive).

#define HW_FLASH_STORAGE_BASE  (1024 * 1024)
#define MAGIC_8_BYTES "RHE!FS30"

#define FLASH_BYTES (PICO_FLASH_SIZE_BYTES - HW_FLASH_STORAGE_BASE)
#define FLASH_MEGS (FLASH_BYTES / 1024 / 1024)

#define NUM_FAT_SECTORS (FLASH_BYTES / 512 - 4)   // legacy count; also the fs_map array size (max of both formats)
#define NUM_FLASH_SECTORS (FLASH_BYTES / 4096) //  3840  // 15megs / 4096bytes = 3840

// Current (MZP1) format geometry: two map slots per meg reserved up front,
// and the advertised FAT sector count equals the real page capacity so a
// full volume runs out of space instead of running out of flash pages
// (the legacy count over-commits by 4/116 sectors - see getNextWriteSector).
#define MZP1_RESERVED_SECTORS (2 * FLASH_MEGS)
#define MZP1_ENTRIES_PER_MEG  ((FLASH_SECTOR_SIZE - 16) / 2)  // 2040
#define MZP1_NUM_FAT_SECTORS  ((NUM_FLASH_SECTORS - MZP1_RESERVED_SECTORS) * 8)

typedef struct {
    uint8_t header[8];
    uint16_t sectors[NUM_FAT_SECTORS];  // map FAT sectors -> flash sectors
} sector_map;

// Per-sector header of the MZP1 map format (16 bytes, then 2040 entries)
typedef struct {
    uint8_t  magic[4];      // "MZP1"
    uint32_t generation;    // higher wins between the two slots of a meg
    uint16_t meg;           // which map meg this sector carries
    uint16_t entry_count;   // MZP1_ENTRIES_PER_MEG
    uint32_t checksum;      // 32-bit sum of the payload bytes
} mzp1_map_header;

static const uint8_t MZP1_MAGIC_BYTES[4] = {'M', 'Z', 'P', '1'};

sector_map fs_map;
bool fs_map_needs_written[FLASH_MEGS];

static bool fs_is_legacy = false;                 // mounted volume format
static uint8_t map_active_slot[FLASH_MEGS];       // MZP1: 0 -> sector 2i, 1 -> 2i+1
static uint32_t map_generation = 0;               // MZP1: last written generation

uint8_t used_bitmap[NUM_FLASH_SECTORS];    // we will use 256 flash sectors for 2048 fat sectors
// Pages freed since the last successful map sync. The PERSISTED map still
// references them, so recycling one before the next sync would make a power
// cut fatal: mount would load a checksum-valid but stale map pointing at
// erased/reused pages (this exact sequence killed a volume in the field
// during a near-full bulk copy). Quarantined pages are not allocatable;
// when only they block an allocation, one forced map sync frees them.
uint8_t pending_free_bitmap[NUM_FLASH_SECTORS];

uint16_t write_sector = 0;   // which flash sector we are writing to
uint8_t write_sector_bitmap = 0;   // 1 for each free 512 byte page on the sector

static inline uint16_t reserved_sectors(void) {
    return fs_is_legacy ? FLASH_MEGS : MZP1_RESERVED_SECTORS;
}

uint32_t flash_fs_num_fat_sectors(void) {
    return fs_is_legacy ? NUM_FAT_SECTORS : MZP1_NUM_FAT_SECTORS;
}

// each sector entry in the sector map is:
//  13 bits of sector (indexing 8192 4k flash sectors)
//   3 bits of offset (0->7 512 byte FAT sectors in each 4k flash sector)
uint16_t getMapSector(uint16_t mapEntry) { return (mapEntry & 0xFFF8) >> 3; }
uint8_t getMapOffset(uint16_t mapEntry) { return mapEntry & 0x7; }
uint16_t makeMapEntry(uint16_t sector, uint8_t offset) { return (sector << 3) | offset; };

// forward declns
void flash_read_sector(uint16_t sector, uint8_t offset, void *buffer, uint16_t size);
void flash_erase_sector(uint16_t sector);
void flash_write_sector(uint16_t sector, uint8_t offset, const void *buffer, uint16_t size);
void flash_erase_with_copy_sector(uint16_t sector, uint8_t preserve_bitmap);

// Set on Z80 reset (device.cpp); core 1 must stop initiating flash work so
// the core-0 reset flush can run without two symmetric lockout initiators
// deadlocking against each other. Weak so standalone binaries that don't
// link device.cpp (mzpico_format) still build; the firmware's definition
// overrides this.
__attribute__((weak)) volatile bool shutting_down = false;

// Helpers to minimize repetition and keep flash critical sections consistent
static inline bool flash_guard_enter(uint32_t *saved_ints) {
    // Core 1 yields at this (pre-lockout, safe) boundary during shutdown:
    // the whole chip reboots moments later via the reset flush on core 0.
    // IRQs stay enabled here so this core still services lockout requests.
    if (get_core_num() == 1) {
        while (shutting_down) { tight_loop_contents(); }
    }
    uint other_core = 1u - get_core_num();
    bool lockout_started = false;
    bool lockout_ready = multicore_lockout_victim_is_initialized(other_core);
    if (lockout_ready) {
        multicore_lockout_start_blocking();
        lockout_started = true;
    }
    *saved_ints = save_and_disable_interrupts();
    return lockout_started;
}

static inline void flash_guard_exit(uint32_t saved_ints, bool lockout_started) {
    restore_interrupts(saved_ints);
    if (lockout_started) {
        multicore_lockout_end_blocking();
    }
}

void debug_print_in_use() {
    return;
    // just shows first 1meg
    printf("IN USE-----------------------------------\n");
    for (int i=0; i<16; i++) {
        printf("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            used_bitmap[i*16+0], used_bitmap[i*16+1], used_bitmap[i*16+2], used_bitmap[i*16+3],
            used_bitmap[i*16+4], used_bitmap[i*16+5], used_bitmap[i*16+6], used_bitmap[i*16+7],
            used_bitmap[i*16+8], used_bitmap[i*16+9], used_bitmap[i*16+10], used_bitmap[i*16+11],
            used_bitmap[i*16+12], used_bitmap[i*16+13], used_bitmap[i*16+14], used_bitmap[i*16+15]);
    }
    printf("END--------------------------------------\n");
}

// Scratch for map-sector images and the erase-with-copy path. A 4KB stack
// local would overflow the ~2KB core stacks; access is single-context (see
// the header comment), so one static buffer is safe.
static uint8_t sector_scratch[FLASH_SECTOR_SIZE];

static uint32_t mzp1_payload_sum(const uint8_t *payload) {
    uint32_t sum = 0;
    for (int i = 0; i < FLASH_SECTOR_SIZE - 16; i++)
        sum += payload[i];
    return sum;
}

// Build the MZP1 map-sector image for one meg into sector_scratch
static void mzp1_build_map_image(int meg, uint32_t generation) {
    mzp1_map_header *hdr = (mzp1_map_header *)sector_scratch;
    memset(sector_scratch, 0, sizeof(sector_scratch));
    memcpy(hdr->magic, MZP1_MAGIC_BYTES, 4);
    hdr->generation = generation;
    hdr->meg = (uint16_t)meg;
    hdr->entry_count = MZP1_ENTRIES_PER_MEG;
    uint32_t first = meg * MZP1_ENTRIES_PER_MEG;
    uint32_t count = MZP1_ENTRIES_PER_MEG;
    if (first + count > MZP1_NUM_FAT_SECTORS)
        count = (first < MZP1_NUM_FAT_SECTORS) ? (MZP1_NUM_FAT_SECTORS - first) : 0;
    memcpy(sector_scratch + 16, &fs_map.sectors[first], count * 2);
    hdr->checksum = mzp1_payload_sum(sector_scratch + 16);
}

// Validate an MZP1 map sector read into sector_scratch; returns generation
// via *gen_out
static bool mzp1_image_valid(int meg, uint32_t *gen_out) {
    const mzp1_map_header *hdr = (const mzp1_map_header *)sector_scratch;
    if (memcmp(hdr->magic, MZP1_MAGIC_BYTES, 4) != 0) return false;
    if (hdr->meg != meg) return false;
    if (hdr->entry_count != MZP1_ENTRIES_PER_MEG) return false;
    if (hdr->checksum != mzp1_payload_sum(sector_scratch + 16)) return false;
    *gen_out = hdr->generation;
    return true;
}

void write_fs_map()
{
    debug_print_in_use();
    if (fs_is_legacy) {
        // Legacy in-place map rewrite: a power cut between the erase and
        // the program below loses the map. Documented risk of the legacy
        // format; reformat to get the double-buffered map.
        for (int i=0; i<FLASH_MEGS; i++) {
            if (fs_map_needs_written[i]) {
                flash_erase_sector(i);
                flash_write_sector(i, 0, (uint8_t*)&fs_map+(4096*i), 4096);
                fs_map_needs_written[i] = false;
            }
        }
        // The persisted map now matches RAM: quarantined pages become free
        memset(pending_free_bitmap, 0, NUM_FLASH_SECTORS);
        return;
    }

    // MZP1: write each dirty meg to its INACTIVE slot with a fresh
    // generation; the old slot stays valid until the new one verifies
    map_generation++;
    for (int i=0; i<FLASH_MEGS; i++) {
        if (!fs_map_needs_written[i]) continue;
        uint8_t target = 1 - map_active_slot[i];
        uint16_t sector = (uint16_t)(2 * i + target);
        mzp1_build_map_image(i, map_generation);
        flash_erase_sector(sector);
        flash_write_sector(sector, 0, sector_scratch, FLASH_SECTOR_SIZE);
        // Verify the whole sector before trusting it: on a bad program the
        // old slot remains the valid one and the meg stays dirty
        const uint8_t *xip = (const uint8_t *)(XIP_BASE + HW_FLASH_STORAGE_BASE
                                               + (uint32_t)sector * FLASH_SECTOR_SIZE);
        if (memcmp(xip, sector_scratch, FLASH_SECTOR_SIZE) == 0) {
            map_active_slot[i] = target;
            fs_map_needs_written[i] = false;
        } else {
            printf("flash_fs: map sector %d program failed\n", sector);
        }
    }

    // Quarantined pages become free only once EVERY dirty meg persisted:
    // a failed meg's old slot still references its pre-sync pages
    bool all_clean = true;
    for (int i=0; i<FLASH_MEGS; i++)
        if (fs_map_needs_written[i]) { all_clean = false; break; }
    if (all_clean)
        memset(pending_free_bitmap, 0, NUM_FLASH_SECTORS);
}

// Returns a map entry for a free 512-byte page, or 0 when the volume is
// physically full. 0 is never a valid allocation (the map sectors occupy
// the low flash sectors, so real entries are always >= reserved * 8).
// The legacy geometry over-commits FAT sectors by a few pages; without
// this guard a full volume fabricated an offset-8 entry and programmed
// the NEXT flash sector's first page - corrupting unrelated data.
static bool any_pending_free(void)
{
    for (int i=0; i<NUM_FLASH_SECTORS; i++)
        if (pending_free_bitmap[i]) return true;
    return false;
}

uint16_t getNextWriteSector()
{
    static uint16_t search_start_pos = 0;
    int i;
    if (write_sector == 0 || write_sector_bitmap == 0)
    {
        // Occupancy is used | pending: quarantined pages must survive both
        // selection and the erase below (their content is what the
        // persisted map still points at)
        for (int attempt = 0; ; attempt++) {
            // first try to find a completely free sector
            for (i=0; i<NUM_FLASH_SECTORS; i++) {
                uint16_t s = (i + search_start_pos) % NUM_FLASH_SECTORS;
                if ((uint8_t)(used_bitmap[s] | pending_free_bitmap[s]) == 0)
                    break;
            }
            if (i < NUM_FLASH_SECTORS) {
                write_sector = (i + search_start_pos) % NUM_FLASH_SECTORS;
                write_sector_bitmap = 0xFF;
                flash_erase_sector(write_sector);
                break;
            }
            // no completely free sector, first sector with eligible space
            for (i=0; i<NUM_FLASH_SECTORS; i++) {
                uint16_t s = (i + search_start_pos) % NUM_FLASH_SECTORS;
                if ((uint8_t)(used_bitmap[s] | pending_free_bitmap[s]) != 0xFF)
                    break;
            }
            if (i < NUM_FLASH_SECTORS) {
                write_sector = (i + search_start_pos) % NUM_FLASH_SECTORS;
                uint8_t occupied = used_bitmap[write_sector] | pending_free_bitmap[write_sector];
                write_sector_bitmap = (uint8_t)~occupied;
                flash_erase_with_copy_sector(write_sector, occupied);
                break;
            }
            // Nothing eligible. If quarantined pages are what blocks us,
            // persist the map once - that makes their frees real - and
            // retry; otherwise (or if the sync failed) the volume is full.
            if (attempt > 0 || !any_pending_free()) {
                printf("flash_fs: volume full\n");
                return 0;
            }
            write_fs_map();
        }
        search_start_pos = (i + search_start_pos) % NUM_FLASH_SECTORS;
    }
    // if we get here, then at least one 512 byte page is free on the write_sector
    for (i=0; i<8; i++) {
        if (write_sector_bitmap & (1 << i))
            break;
    }
    if (i == 8) return 0; // defensive: no free page despite selection
    // mark the offset used
    write_sector_bitmap &= ~(1 << i);
    return makeMapEntry(write_sector, i);
}

void init_used_bitmap() {
    memset(used_bitmap, 0, NUM_FLASH_SECTORS);
    memset(pending_free_bitmap, 0, NUM_FLASH_SECTORS);
    for (int i=0; i<reserved_sectors(); i++)
        used_bitmap[i] = 0xFF;    // flash sectors used by the fs map

    for (uint32_t i=0; i<flash_fs_num_fat_sectors(); i++) {
        uint16_t mapEntry = fs_map.sectors[i];
        if (mapEntry)
            used_bitmap[getMapSector(mapEntry)] |= (1 << getMapOffset(mapEntry));
    }
    write_sector = 0;
}

// Mount the MZP1 format: per meg pick the valid slot with the higher
// generation. Returns 0 on success.
static int mzp1_try_mount(void)
{
    memset(fs_map.sectors, 0, sizeof(fs_map.sectors));
    map_generation = 0;

    for (int meg = 0; meg < FLASH_MEGS; meg++) {
        int best_slot = -1;
        uint32_t best_gen = 0;
        for (int slot = 0; slot < 2; slot++) {
            uint32_t gen;
            flash_read_sector((uint16_t)(2 * meg + slot), 0, sector_scratch, FLASH_SECTOR_SIZE);
            if (mzp1_image_valid(meg, &gen)) {
                if (best_slot < 0 || (int32_t)(gen - best_gen) > 0) {
                    best_slot = slot;
                    best_gen = gen;
                }
            }
        }
        if (best_slot < 0)
            return 1; // no valid copy of this meg: not an MZP1 volume (or damaged)

        // Re-read the winning slot (scratch holds the last one read)
        flash_read_sector((uint16_t)(2 * meg + best_slot), 0, sector_scratch, FLASH_SECTOR_SIZE);
        uint32_t first = meg * MZP1_ENTRIES_PER_MEG;
        uint32_t count = MZP1_ENTRIES_PER_MEG;
        if (first + count > MZP1_NUM_FAT_SECTORS)
            count = (first < MZP1_NUM_FAT_SECTORS) ? (MZP1_NUM_FAT_SECTORS - first) : 0;
        memcpy(&fs_map.sectors[first], sector_scratch + 16, count * 2);
        map_active_slot[meg] = (uint8_t)best_slot;
        if ((int32_t)(best_gen - map_generation) > 0)
            map_generation = best_gen;
    }
    return 0;
}

int flash_fs_mount()
{
    for (int i=0; i<FLASH_MEGS; i++)
        fs_map_needs_written[i] = false;

    // Current format first
    if (mzp1_try_mount() == 0) {
        fs_is_legacy = false;
        init_used_bitmap();
        debug_print_in_use();
        return 0;
    }

    // Legacy format: header + map in the first FLASH_MEGS sectors
    flash_read_sector(0, 0, &fs_map, 4096);
    if (memcmp(fs_map.header, MAGIC_8_BYTES, 8) != 0) {
        printf("mountFlashFS() - no filesystem found\n");
        return 1;
    }
    for (int i=1; i<FLASH_MEGS; i++)
        flash_read_sector(i, 0, (uint8_t*)&fs_map+(4096*i), 4096);

    fs_is_legacy = true;
    printf("mountFlashFS() - legacy format (no torn-sync protection); reformat to upgrade\n");
    init_used_bitmap();
    debug_print_in_use();
    return 0;
}

bool flash_fs_region_is_blank(void)
{
    // Erased NOR flash reads 0xFF everywhere. Any non-FF byte means the
    // region held (or holds) data - possibly a volume whose map sector was
    // lost mid-sync - and reformatting would destroy it.
    const uint8_t *p = (const uint8_t *)(XIP_BASE + HW_FLASH_STORAGE_BASE);
    for (uint32_t i = 0; i < (uint32_t)FLASH_BYTES; i += 4) {
        if (*(const uint32_t *)(p + i) != 0xFFFFFFFFu)
            return false;
    }
    return true;
}

void flash_fs_create()
{
    printf("flash_fs_create()\n");
    fs_is_legacy = false;
    memset(&fs_map, 0, sizeof(fs_map));
    map_generation = 0;
    // Erase BOTH slots of every meg: a stale higher-generation slot from a
    // previous filesystem would win the next mount over the fresh map
    for (int i=0; i<MZP1_RESERVED_SECTORS; i++)
        flash_erase_sector((uint16_t)i);
    for (int i=0; i<FLASH_MEGS; i++) {
        map_active_slot[i] = 1;         // first sync lands in slot 0
        fs_map_needs_written[i] = true;
    }
    write_fs_map();
    init_used_bitmap();
}

void flash_fs_sync()
{
    write_fs_map();
}

void flash_fs_read_FAT_sector(uint16_t fat_sector, void *buffer)
{
    if (fat_sector >= flash_fs_num_fat_sectors()) {
        memset(buffer, 0, 512);
        return;
    }
    int mapEntry = fs_map.sectors[fat_sector];
    if (mapEntry)
        flash_read_sector(getMapSector(mapEntry), getMapOffset(mapEntry), buffer, 512);
    else
        memset(buffer, 0, 512);
    return;
}

static void mark_map_meg_dirty(uint16_t fat_sector)
{
    if (fs_is_legacy) {
        // Legacy map layout: 2044 entries in the headered first sector,
        // 2048 in each following one
        if (fat_sector < 2044)
            fs_map_needs_written[0] = true;
        else
            fs_map_needs_written[1+((fat_sector-2044)/2048)] = true;
    } else {
        fs_map_needs_written[fat_sector / MZP1_ENTRIES_PER_MEG] = true;
    }
}

bool flash_fs_write_FAT_sector(uint16_t fat_sector, const void *buffer)
{
    if (fat_sector >= flash_fs_num_fat_sectors()) return false;

    uint16_t oldEntry = fs_map.sectors[fat_sector];
    if (oldEntry)
    {   // The previous page is dead in RAM but the persisted map may still
        // reference it: quarantine it until the next successful map sync
        // (a forced sync inside the allocator can still reclaim it - after
        // syncing, the only content at risk is this very sector's, which
        // an in-flight write puts at risk regardless)
        used_bitmap[getMapSector(oldEntry)] &= ~(1 << getMapOffset(oldEntry));
        pending_free_bitmap[getMapSector(oldEntry)] |= (1 << getMapOffset(oldEntry));
    }
    uint16_t mapEntry = getNextWriteSector();
    if (mapEntry == 0) {
        // Volume full: restore the old page's bookkeeping; the previously
        // stored data and the map entry are untouched
        if (oldEntry) {
            used_bitmap[getMapSector(oldEntry)] |= (1 << getMapOffset(oldEntry));
            pending_free_bitmap[getMapSector(oldEntry)] &= ~(1 << getMapOffset(oldEntry));
        }
        return false;
    }
    fs_map.sectors[fat_sector] = mapEntry;
    mark_map_meg_dirty(fat_sector);

    used_bitmap[getMapSector(mapEntry)] |= (1 << getMapOffset(mapEntry));

    flash_write_sector(getMapSector(mapEntry), getMapOffset(mapEntry), buffer, 512);
    return true;
}

bool flash_fs_verify_FAT_sector(uint16_t fat_sector, const void *buffer)
{
    uint8_t read_buf[512];
    flash_fs_read_FAT_sector(fat_sector, read_buf);
    if (memcmp(buffer, read_buf, 512) == 0) return true;
    return false;
}

/* Low level flash functions */

void flash_read_sector(uint16_t sector, uint8_t offset, void *buffer, uint16_t size)
{
//  printf("[FS] READ: %d, %d (%d)\n", sector, offset, size);
    uint32_t fs_start = XIP_BASE + HW_FLASH_STORAGE_BASE;
    uint32_t addr = fs_start + (sector * FLASH_SECTOR_SIZE) + (offset * 512);   
    memcpy(buffer, (unsigned char *)addr, size);
}

void flash_erase_sector(uint16_t sector)
{
//  printf("[FS] ERASE: %d\n", sector);
    uint32_t fs_start = HW_FLASH_STORAGE_BASE;
    uint32_t offset = fs_start + (sector * FLASH_SECTOR_SIZE);
    uint32_t ints;
    bool lockout_started = flash_guard_enter(&ints);
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_guard_exit(ints, lockout_started);
    return;
}

void flash_write_sector(uint16_t sector, uint8_t offset, const void *buffer, uint16_t size)
{
//  printf("[FS] WRITE: %d, %d (%d)\n", sector, offset, size);
    uint32_t fs_start = HW_FLASH_STORAGE_BASE;
    uint32_t addr = fs_start + (sector * FLASH_SECTOR_SIZE) + (offset * 512);
    uint32_t ints;
    bool lockout_started = flash_guard_enter(&ints);
    flash_range_program(addr, (const uint8_t *)buffer, size);
    flash_guard_exit(ints, lockout_started);
}

void flash_erase_with_copy_sector(uint16_t sector, uint8_t preserve_bitmap)
{
//  printf("[FS] ERASE with COPY: %d\n", sector);
    // sector_scratch, not a stack local: 4KB would overflow the ~2KB core
    // stacks (this path triggers on a well-filled volume)
    flash_read_sector(sector, 0, sector_scratch, FLASH_SECTOR_SIZE);
    flash_erase_sector(sector);
    for (int i=0; i<8; i++) {
        if (preserve_bitmap & (1 << i))
           flash_write_sector(sector, i, sector_scratch + (i * 512), 512);
    }
}
