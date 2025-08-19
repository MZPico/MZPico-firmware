#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "file.h"
#include "device.h"
#include "ff.h"
#include "fdc.h"
#include "qd.h"
#include "fatfs_disk.h"
#include "basic.h"


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
	if (strcasecmp(ext, "MZF") == 0 || strcasecmp(ext, "M12") == 0 ||
       strcasecmp(ext, "DSK") == 0 || strcasecmp(ext, "MZQ") == 0)
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

int read_directory(char *path, GEN_RD *buffer) {
	int ret = 0;
	uint16_t *dir_list_size = (uint16_t *)buffer->data;
	uint16_t num_dir_entries;
  uint8_t *buff = buffer->data+2;
	DIR_ENTRY *dst = (DIR_ENTRY *)buff;

  num_dir_entries = 0;
	DIR dir;
  if (f_opendir(&dir, path) == FR_OK) {
    if (strcmp(path, "/") != 0) {
      strcpy(dst->filename, "..");
      dst->isDir = 1;
      dst++;
      num_dir_entries++;
    }
		while (num_dir_entries < MAX_DIR_FILES) {
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
			num_dir_entries++;
		}
		f_closedir(&dir);
 		qsort((DIR_ENTRY *)buff, num_dir_entries, sizeof(DIR_ENTRY), entry_compare);
    *(dir_list_size) = num_dir_entries * sizeof(DIR_ENTRY);
 	} else {
		strcpy(buff, "Can't read directory");
    *(dir_list_size) = strlen(buff) + 1;
    ret = 1;
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

void get_uppercase_extension(const char* filename, char* extension) {
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        extension[0] = '\0'; // No extension
        return;
    }

    dot++; // Skip the dot
    while (*dot) {
        *extension++ = toupper((unsigned char)*dot);
        dot++;
    }
    *extension = '\0'; // Null-terminate the string
}



int mount_file(char *path, GEN_RD *buffer) {
  UINT br;
  uint16_t len;
  uint8_t *buff = buffer->data+2;
	uint16_t *dir_list_size = (uint16_t *)buffer->data;
  char extension[16];
  FIL fil;

  get_uppercase_extension(path, extension);
  if (!strcmp(extension, "MZF") || !strcmp(extension, "M12")) {
    if (f_open(&fil, path, FA_READ) != FR_OK) {
      sprintf(buff, "Can't read file %s", path);
      *(dir_list_size) = strlen(buff) + 1;
      return 2;
    }
    f_read(&fil, buff, 128, &br); // read addresses
    memcpy(&len, buff+18, 2);
    f_read(&fil, buff+128, len, &br); // read body
    len += 2 + 128;
    memcpy(buffer->data, &len, 2);
  } else if (!strcmp(extension, "DSK"))
    fdc_set_drive_content(0, path);
  else if (!strcmp(extension, "MZQ"))
    qd_set_drive_content(path);
  else if (!strcmp(path, "#basic")) {
    memcpy(buffer->data, basic+18, 2);
    memcpy(buffer->data+2, basic, sizeof(basic));
  }
  return 0;
}
