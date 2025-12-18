#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

#include "file.hpp"
#include "device.hpp"
#include "ff.h"
#include "fdc.hpp"
#include "qd.hpp"
#include "fatfs_disk.h"
#include "embedded_mzf.hpp"
#include "pico_mgr.hpp"

#define FLASH_ID "flash"
#define SD_ID "sd"
#ifdef USE_PICO_W
#define CLOUD_ID "cloud"
#endif

uint8_t device_count;
DEV_ENTRY devices[MAX_DEVICES];
FATFS fatfs_flash;
FATFS fatfs_sd;

typedef struct {
  char     is_dir;
  char     filename[MAX_FILENAME_LENGTH];
  uint32_t size;
} DIR_ENTRY;
#define DIR_ENTRY_SIZE (sizeof(char) + MAX_FILENAME_LENGTH + sizeof(uint32_t))

void pack_DIR_ENTRY(const void* recPtr, uint8_t* dst) {
    const DIR_ENTRY* r = (const DIR_ENTRY*)recPtr;
    dst[0] = r->is_dir;
    memcpy(dst + 1, r->filename, MAX_FILENAME_LENGTH);
    write_u32_le(dst + 1 + MAX_FILENAME_LENGTH, r->size);
}
void unpack_DIR_ENTRY(const uint8_t* src, void* outPtr) {
    DIR_ENTRY* r = (DIR_ENTRY*)outPtr;
    r->is_dir = src[0];
    memcpy(r->filename, src + 1, MAX_FILENAME_LENGTH);
    r->size = read_u32_le(src + 1 + MAX_FILENAME_LENGTH);
}

static inline void sanitize_filename(char *dst, size_t dst_len, const char *src) {
  if (!dst_len) return;
  if (src == NULL) { dst[0] = '\0'; return; }
  // Copy safely and always NUL-terminate
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = '\0';
}

int mount_devices(void) {
  mount_fatfs_disk();
  device_count = 0;
  if (f_mount(&fatfs_sd, SD_ID":", 1) == FR_OK) {
    strcpy(devices[device_count++].name, SD_ID);
  }
  if (f_mount(&fatfs_flash, FLASH_ID":", 1) != FR_OK)
    return 1;
  {
    strcpy(devices[device_count++].name, FLASH_ID);
  }
  return 0;
}

char *get_filename_ext(char *filename) {
  char *dot = strrchr(filename, '.');
  if(!dot || dot == filename) return NULL;
  return dot + 1;
}

int is_valid_file(char *filename) {
  char *ext = get_filename_ext(filename);
  if (!ext)
    return 0;
  if (strcasecmp(ext, "MZF") == 0 || strcasecmp(ext, "M12") == 0 ||
      strcasecmp(ext, "DSK") == 0 || strcasecmp(ext, "MZQ") == 0)
    return 1;
  return 0;
}

int entry_compare(const void* p1, const void* p2) {
  const DIR_ENTRY* e1 = (const DIR_ENTRY*)p1;
  const DIR_ENTRY* e2 = (const DIR_ENTRY*)p2;
  if (e1->is_dir && !e2->is_dir) return -1;
  else if (!e1->is_dir && e2->is_dir) return 1;
  else return strcasecmp(e1->filename, e2->filename);
}

int read_directory(const char *path, PicoMgr *mgr) {
  // Cloud prefix handling (virtual filesystem served over WiFi/HTTPS)
#ifdef USE_PICO_W
  if (strncmp(path, "cloud:/", 7) == 0 || strncmp(path, "cloud:", 6) == 0) {
    return cloud_read_directory(path, mgr);
  }
#endif
  FILINFO fno;
  int ret = 0;
  uint16_t num_dir_entries = 0;
  size_t path_ln = strlen(path);
  DIR dir;
  DIR_ENTRY entries[MAX_DIR_FILES];

  if (f_opendir(&dir, path) == FR_OK) {
    // If not root, add ".."
    if (path_ln >= 2 && path[path_ln-2] != ':' && path[path_ln-1] != '/') {
      entries[num_dir_entries].is_dir = 1;
      sanitize_filename(entries[num_dir_entries].filename, sizeof(entries[num_dir_entries].filename), "..");
      entries[num_dir_entries].size = 0;
      num_dir_entries++;
    }

    while (num_dir_entries < MAX_DIR_FILES) {
      if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
        break;
      if (fno.fattrib & (AM_HID | AM_SYS))
        continue;

      uint8_t is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
      if (!is_dir && !is_valid_file(fno.fname))
        continue;

      entries[num_dir_entries].is_dir = is_dir;
      sanitize_filename(entries[num_dir_entries].filename, sizeof(entries[num_dir_entries].filename), fno.fname);
      entries[num_dir_entries].size = fno.fsize;
      num_dir_entries++;
    }

    f_closedir(&dir);

    // Sort aligned array
    qsort(entries, num_dir_entries, sizeof(DIR_ENTRY), entry_compare);

    mgr->setContent(DIR_ENTRY_SIZE, pack_DIR_ENTRY, unpack_DIR_ENTRY);
    for (uint16_t i = 0; i < num_dir_entries; i++) {
      mgr->addRecord(&entries[i]);
    }
  } else {
    mgr->setString("Can't read directory");
    ret = 1;
  }
  return ret;
}

uint16_t count_ones(const uint8_t *array, size_t length) {
  uint16_t count = 0;
  for (size_t i = 0; i < length; i++) {
    uint8_t byte = array[i];
    while (byte) {
      byte &= (uint8_t)(byte - 1);
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
    *extension++ = (char)toupper((unsigned char)*dot);
    dot++;
  }
  *extension = '\0'; // Null-terminate the string
}

int mount_file(const char *path, PicoMgr *mgr) {
  // Cloud file fetch handling
#ifdef USE_PICO_W
  if (strncmp(path, "cloud:/", 7) == 0 || strncmp(path, "cloud:", 6) == 0) {
    return cloud_mount_file(path, mgr);
  }
#endif
  UINT br;
  uint16_t len;
  char extension[16];
  uint8_t *payload;
  FIL fil;

  get_uppercase_extension(path, extension);
  if (!strcmp(extension, "MZF") || !strcmp(extension, "M12")) {
    if (f_open(&fil, path, FA_READ) != FR_OK) {
      payload = mgr->allocateRaw(200);
      snprintf((char *)payload, 200, "Can't read file %s", path);
      return 2;
    }

    payload = mgr->allocateRaw(128); 
    if (f_read(&fil, payload, 128, &br) != FR_OK || br != 128) {
      mgr->setString("File read error (header)");
      f_close(&fil);
      return 2;
    }

    memcpy(&len, payload + 18, sizeof(len));

    payload = mgr->allocateRaw(len);
    if (f_read(&fil, payload, len, &br) != FR_OK || br != len) {
      mgr->setString("File read error (body)");
      f_close(&fil);
      return 2;
    }
    f_close(&fil);

  } else if (!strcmp(extension, "DSK")) {
    if (!fdc->setDriveContent(0, path)) {
      mgr->setString("File read error");
      return 2;
    }
  } else if (!strcmp(extension, "MZQ")) {
    qd->setDriveContent(path);
  } else if (!strcmp(path, "@menu")) {
    mgr->addRaw(mzf_menu, sizeof(mzf_menu));
  } else if (!strcmp(path, "@explorer")) {
    mgr->addRaw(mzf_explorer, sizeof(mzf_explorer));
  } else if (!strcmp(path, "@basic")) {
    mgr->addRaw(mzf_basic, sizeof(mzf_basic));
  }
  return 0;
}

int get_device_list(PicoMgr *mgr) {
  for (uint8_t i = 0; i < device_count; i++) {
    mgr->addRaw((uint8_t *)devices[i].name, MAX_DEV_NAME_LENGTH);
  }
  return 0;
}

#ifdef USE_PICO_W
void cloud_add_device(std::string new_device) {
  // Prevent duplicate insertion
  for (uint8_t i = 0; i < device_count; ++i) {
    if (strncmp(devices[i].name, new_device.c_str(), MAX_DEV_NAME_LENGTH) == 0)
      return;
  }
  if (device_count < MAX_DEVICES) {
    strncpy(devices[device_count].name, new_device.c_str(), MAX_DEV_NAME_LENGTH);
    devices[device_count].name[MAX_DEV_NAME_LENGTH - 1] = '\0';
    device_count++;
  }
}
#endif
