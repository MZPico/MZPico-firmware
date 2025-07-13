#include <string.h>
#include <stdlib.h>

#include "mz800pico_file.h"
#include "mz800pico_device.h"
#include "ff.h"
#include "fatfs_disk.h"


typedef struct __attribute__((__packed__)) {
	char isDir;
	char filename[32];
  uint32_t size;
} DIR_ENTRY;
        

char *get_filename_ext(char *filename) {
    char *dot = strrchr(filename, '.');
    if(!dot || dot == filename) return "";
    return dot + 1;
}

int is_valid_file(char *filename) {
	char *ext = get_filename_ext(filename);
	if (strcasecmp(ext, "MZF") == 0 || strcasecmp(ext, "M12") == 0)
		return 1;
	return 0;
}

int entry_compare(const void* p1, const void* p2)
{
	DIR_ENTRY* e1 = (DIR_ENTRY*)p1;
	DIR_ENTRY* e2 = (DIR_ENTRY*)p2;
	if (e1->isDir && !e2->isDir) return -1;
	else if (!e1->isDir && e2->isDir) return 1;
	else return strcasecmp(e1->filename, e2->filename);
}

FILINFO fno;

int read_directory(GEN_RD *dest_rd, char *path) {
	int ret = 0;
	uint16_t *num_dir_entries = (uint16_t *)&dest_rd->data[0];
  uint8_t *buff = dest_rd->data+2;
	DIR_ENTRY *dst = (DIR_ENTRY *)buff;

  if (!fatfs_is_mounted())
     mount_fatfs_disk();

	FATFS FatFs;
  *num_dir_entries = 0;
	if (f_mount(&FatFs, "", 1) == FR_OK) {
		DIR dir;
		if (f_opendir(&dir, path) == FR_OK) {
      if (strcmp(path, "/") != 0) {
        strcpy(dst->filename, "..");
        dst->isDir = 1;
        dst++;
        (*num_dir_entries)++;
      }
			while (*num_dir_entries < MAX_DIR_FILES) {
				if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
					break;
				if (fno.fattrib & (AM_HID | AM_SYS))
					continue;
				dst->isDir = fno.fattrib & AM_DIR ? 1 : 0;
				if (!dst->isDir)
					if (!is_valid_file(fno.fname)) continue;
				// copy file record to first ram block
				// long file name
				strncpy(dst->filename, fno.fname, 31);
				dst->filename[31] = 0;
        dst->size = fno.fsize;
        dst++;
				(*num_dir_entries)++;
			}
			f_closedir(&dir);
		}
		else {
			strcpy(buff, "Can't read directory");
    }
		f_mount(0, "", 1);
		qsort((DIR_ENTRY *)buff, *num_dir_entries, sizeof(DIR_ENTRY), entry_compare);
		ret = 1;
	}
	else {
		strcpy(buff, "Can't read flash memory");
  }
	return ret;
}

uint16_t count_ones(const uint8_t *array, size_t length) {
  uint16_t count = 0;
  for (size_t i = 0; i < length; i++) {
    uint8_t byte = array[i];
    while (byte) {
      byte &= (byte - 1);
      count++;
    }
  }
  return count;
}

int mount_file(GEN_RD *dir_rd, GEN_RD *mount_rd, uint16_t index) {
  FATFS FatFs;
  UINT br;

  if (f_mount(&FatFs, "", 1) != FR_OK)
		return 0;

  FIL fil;
	if (f_open(&fil, ((DIR_ENTRY *)&dir_rd->data[2])[index].filename, FA_READ) != FR_OK)
		goto cleanup;

  f_lseek(&fil, 18);
  f_read(&fil, mount_rd->data, 6, &br); // read addresses
  f_lseek(&fil, 128);
  f_read(&fil, &mount_rd->data[9], *(uint16_t *)mount_rd->data, &br); // read body
  *(uint16_t *)&mount_rd->data[6] = count_ones((uint8_t *)&mount_rd->data[9], *(uint16_t *)mount_rd->data); // count body checksum
  mount_rd->data[8] = count_ones(mount_rd->data, 8) & 0xff;

cleanup:
  f_mount(0, "", 1);
  return 0;
}
