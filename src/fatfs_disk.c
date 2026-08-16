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

#include "ff.h"
#include "diskio.h"
#include "fatfs_disk.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"

bool flashfs_is_mounted = false;

bool mount_fatfs_disk()
{
    int err = flash_fs_mount();
    if (err)
        return false;

    flashfs_is_mounted = true;
    return true;
}

bool fatfs_is_mounted() { return flashfs_is_mounted; }

void create_fatfs_disk()
{
    flash_fs_create();
    flashfs_is_mounted = true;

    // now create a fatfs on the flash_fs filesystem :-)

    FATFS fs;           /* Filesystem object */
    FIL fil;            /* File object */
    FRESULT res;        /* API result code */
    BYTE work[FF_MAX_SS]; /* Work area (larger is better for processing time) */

    /* Format the default drive with default parameters */
    printf("making fatfs\n");
    res = f_mkfs("flash:", 0, work, sizeof work);
    f_mount(&fs, "flash:", 0);
    f_setlabel("MZ800PICO");
    res = f_open(&fil, "WELCOME.TXT", FA_CREATE_NEW | FA_WRITE);
    f_puts("MZ800Pico\r\nDrag MZF files in here!\r\n", &fil);
    f_close(&fil);
    res = f_open(&fil, "mzpico.ini", FA_CREATE_NEW | FA_WRITE);
    // Default configuration: every device section present (curated common
    // options commented out); README.md is the full option reference.
    // Section order matters - it is device registration order, which
    // decides read priority on shared ports (e.g. 0xf8).
    f_puts("; MZPico default configuration. Full option reference: README.md\r\n"
           "; Looked up on sd:/mzpico.ini first, then flash:/mzpico.ini - keep a copy\r\n"
           "; on the SD card so the machine still boots if the flash volume fails.\r\n"
           "; Sections for devices this board cannot support are skipped at boot.\r\n"
           "\r\n"
           "[menu]\r\n"
           "key_b=Basic|@basic\r\n"
           "key_e=Explorer|@explorer\r\n"
           ";key_1=My game|sd:/games/game.mzf\r\n"
           "\r\n"
           "; SRAM memory card (boot device, port 0xf8)\r\n"
           "[sramdisk]\r\n"
           ";image=sd:/program.mzf\r\n"
           "\r\n"
           "; MZPico PicoRD RAM disk. File-backed by default: costs almost no\r\n"
           "; RAM (the default device set would not fit the Pico W heap with a\r\n"
           "; RAM-backed 64KB disk) and survives power-off. Remove the image=\r\n"
           "; line for a volatile RAM-backed disk.\r\n"
           "[pico_rd]\r\n"
           "image=flash:/pico_rd.img\r\n"
           "size=65536\r\n"
           "\r\n"
           "; MZPico management device - required by the menu and explorer\r\n"
           "[pico_mgr]\r\n"
           "\r\n"
           "; WD1793 floppy controller, drives 1-4\r\n"
           "[fdc]\r\n"
           ";image_disk1=sd:/disk1.dsk\r\n"
           ";image_disk2=sd:/floppy_dir\r\n"
           ";fs_disk2=basic\r\n"
           ";write_protected=true\r\n"
           "\r\n"
           "; MZ-1F11 Quick Disk\r\n"
           "[qd]\r\n"
           ";image=sd:/image.mzq\r\n"
           ";write_protected=true\r\n"
           "\r\n"
           "; ---- Sound: Deluxe board only (skipped on Frugal) ----\r\n"
           "\r\n"
           "; SN76489 PSG\r\n"
           "[psg]\r\n"
           ";volume=20\r\n"
           "\r\n"
           "; MZ-800 beeper (8253)\r\n"
           "[ctc]\r\n"
           ";volume=20\r\n"
           "\r\n"
           "; ---- Optional (not enabled by default) ----\r\n"
           "\r\n"
           "; MZ-1R18-style paged RAM disk (Deluxe board only - skipped on Frugal)\r\n"
           ";[ramdisk]\r\n"
           ";image=sd:/ramdisk.img\r\n"
           ";size=131072\r\n"
           "\r\n"
           "; WiFi cloud storage (Pico W builds only)\r\n"
           ";[cloud]\r\n"
           ";wifi_ssid=MyNetwork\r\n"
           ";wifi_password=secret\r\n"
           "\r\n", &fil);
    f_close(&fil);
    f_mount(0, "", 0);
}

uint32_t fatfs_disk_read(uint8_t* buff, uint32_t sector, uint32_t count)
{	
//	printf("fatfs_disk_read sector=%d, count=%d\n", sector, count);
    if (!flashfs_is_mounted) return RES_ERROR;
    if (sector >= flash_fs_num_fat_sectors())
			return RES_PARERR;

    /* copy data to buffer */
    for (int i=0; i<count; i++)
        flash_fs_read_FAT_sector(sector + i, buff + (i*SECTOR_SIZE));
    return RES_OK;
}

uint32_t fatfs_disk_write(const uint8_t* buff, uint32_t sector, uint32_t count)
{
// 	printf("fatfs_disk_write sector=%d, count=%d\n", sector, count);
    if (!flashfs_is_mounted) return RES_ERROR;
    if (sector >= flash_fs_num_fat_sectors())
        return RES_PARERR;

    /* copy data to buffer */
    for (int i=0; i<count; i++) {
        if (!flash_fs_write_FAT_sector(sector + i, buff + (i*SECTOR_SIZE)))
            return RES_ERROR; // volume full or out of range; old data intact
        // verify
        if (!flash_fs_verify_FAT_sector(sector + i, buff + (i*SECTOR_SIZE))) {
            printf("VERIFY ERROR!");
            return RES_ERROR;
        }
    }
    return RES_OK;
}

void fatfs_disk_sync()
{
    flash_fs_sync();
}
