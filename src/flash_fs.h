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

#ifndef __FLASH_FS_H__
#define __FLASH_FS_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int flash_fs_mount();
void flash_fs_create();
void flash_fs_sync();
void flash_fs_read_FAT_sector(uint16_t fat_sector, void *buffer);
// Returns false when the write cannot be honored (volume full or out of
// range); the previously stored sector content is preserved in that case.
bool flash_fs_write_FAT_sector(uint16_t fat_sector, const void *buffer);
bool flash_fs_verify_FAT_sector(uint16_t fat_sector, const void *buffer);

// Sector count of the MOUNTED volume (legacy and current formats differ);
// before a successful mount this reports the current-format count.
uint32_t flash_fs_num_fat_sectors(void);

// True when the whole flash FS region is erased (never formatted). Used to
// decide whether auto-creating a filesystem is safe: a region that is
// non-blank but fails to mount is damaged and must NOT be reformatted.
bool flash_fs_region_is_blank(void);

// ---- Direct (XIP) access to file data, for the RAM disk's COW mode ----
//
// A FAT sector's data lives in one 512-byte flash page; its map entry is
// the page's index from the start of the storage region, so the page is
// readable at flash_fs_xip_base() + entry * 512. 0 = never written (the
// sector reads as zeros and has no page).
uint16_t flash_fs_map_entry(uint32_t fat_sector);
const uint8_t *flash_fs_xip_base(void);
uint32_t flash_fs_num_pages(void);   // valid entries are below this

// Non-zero while a flash program/erase is in progress (set before the
// other core is locked out, cleared after it is released). XIP reads of
// the storage region must not be issued meanwhile.
extern volatile uint32_t flash_fs_op_active;

// Called (single context, from inside flash_fs_write_FAT_sector, after the
// new page is programmed and the map updated) whenever a sector that HAD a
// page moves to a new page: log-structured writes never update in place,
// and the old page is recycled after the next map sync. A holder of XIP
// pointers into file data must follow the move. NULL = no listener.
extern void (*flash_fs_remap_hook)(uint16_t old_entry, uint16_t new_entry);

// Forget sectors [first, first + count): they read as zeros again and their
// pages are freed (quarantined until the next map sync, like a rewrite's
// old page). No flash operation. FatFS's CTRL_TRIM (cluster chains freed by
// a delete or truncate) and the COW RAM disk's blank image use it.
void flash_fs_trim_FAT_sectors(uint32_t first, uint32_t count);
// Called first by flash_fs_trim_FAT_sectors (single context). NULL = none.
extern void (*flash_fs_trim_hook)(uint32_t first_sector, uint32_t count);

// Erases that writing fat_sector and then syncing the map will cost (the
// allocation's fresh sector, one map-sector erase per dirty meg); a forced
// sync or an erase-with-copy inside the allocation is not foreseen
uint32_t flash_fs_predict_erases(uint16_t fat_sector);

// Reset escape hatch (core 0, after setting shutting_down): wait until core
// 1 is out of any flash operation - it then parks at its next one - so the
// flush that follows never runs a second lockout initiator alongside core
// 1's. Bounded; false on timeout.
bool flash_fs_quiesce_core1(uint32_t timeout_us);

// Counters since power-up (diagnostics, the COW RAM disk's wear figures):
// sector erases, bytes programmed, erases of the erase-with-copy path,
// map syncs (MZP1: one map-sector erase + program per dirty meg).
extern volatile uint32_t flash_fs_erases;
extern volatile uint32_t flash_fs_programmed;
extern volatile uint32_t flash_fs_copy_erases;
extern volatile uint32_t flash_fs_map_syncs;

#ifdef __cplusplus
}
#endif

#endif