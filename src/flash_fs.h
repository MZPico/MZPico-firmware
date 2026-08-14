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

#ifdef __cplusplus
}
#endif

#endif