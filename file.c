#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

#include "file.h"
#include "device.h"
#include "ff.h"
#include "fdc.h"
#include "qd.h"
#include "fatfs_disk.h"
#include "basic.h"

#define FLASH_ID "flash"
#define SD_ID "sd"

/*
 * Alignment & serialization notes (RP2040 / ARM Cortex-M0+):
 * - Do NOT access multi-byte fields via unaligned pointers.
 * - Use a naturally-aligned struct for in-memory operations.
 * - Serialize into the outbound byte buffer with a packed "wire" struct or memcpy of fields.
 */

typedef struct {
  char     is_dir;                             // 1 byte
  char     filename[MAX_FILENAME_LENGTH];      // fixed-size name (must be NUL-terminated by producer)
  uint32_t size;                               // file size
} DIR_ENTRY;                                   // naturally aligned in RAM

#pragma pack(push, 1)
typedef struct {
  char     is_dir;
  char     filename[MAX_FILENAME_LENGTH];
  uint32_t size;
} DIR_ENTRY_WIRE;                              // packed on the wire
#pragma pack(pop)

uint8_t device_count;
DEV_ENTRY devices[MAX_DEVICES];
FATFS fatfs_flash;
FATFS fatfs_sd;


/* ---------- Helpers ---------- */

static inline void write_u16_le(uint8_t *dst, uint16_t v) {
  // Safe for any alignment
  memcpy(dst, &v, sizeof(v));
}

static inline void serialize_dir_entry(uint8_t *dst, const DIR_ENTRY *src) {
  // Serialize one DIR_ENTRY into packed wire format without unaligned accesses
  // Layout: is_dir (1), filename[MAX_FILENAME_LENGTH], size (LE u32)
  DIR_ENTRY_WIRE w;
  w.is_dir = src->is_dir;

  // Ensure filename is NUL-terminated and fits
  // Copy up to MAX_FILENAME_LENGTH-1 and force NUL
  memset(w.filename, 0, sizeof(w.filename));
  strncpy(w.filename, src->filename, sizeof(w.filename) - 1);

  // Copy size (endianness same as host; we memcpy into packed field)
  w.size = src->size;

  memcpy(dst, &w, sizeof(DIR_ENTRY_WIRE));
}

static inline void sanitize_filename(char *dst, size_t dst_len, const char *src) {
  if (!dst_len) return;
  if (src == NULL) { dst[0] = '\0'; return; }
  // Copy safely and always NUL-terminate
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = '\0';
}

/* ---------- Device mounting ---------- */

int mount_devices(void) {
  mount_fatfs_disk();
  device_count = 0;
  if (f_mount(&fatfs_sd, SD_ID":", 1) == FR_OK) {
    strcpy(devices[device_count++].name, SD_ID);
  }
  if (f_mount(&fatfs_flash, FLASH_ID":", 1) != FR_OK)
    return 1;
  strcpy(devices[device_count++].name, FLASH_ID);
  return 0;
}

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

int entry_compare(const void* p1, const void* p2) {
  const DIR_ENTRY* e1 = (const DIR_ENTRY*)p1;
  const DIR_ENTRY* e2 = (const DIR_ENTRY*)p2;
  if (e1->is_dir && !e2->is_dir) return -1;
  else if (!e1->is_dir && e2->is_dir) return 1;
  else return strcasecmp(e1->filename, e2->filename);
}

/*
 * read_directory
 * Output buffer format:
 *   [0..1]  : uint16_t little-endian byte size of payload that follows
 *   [2..]   : payload
 *
 * Payload on success:
 *   num_dir_entries * sizeof(DIR_ENTRY_WIRE) bytes, sequence of packed entries.
 *
 * Payload on failure:
 *   NUL-terminated error string.
 */
int read_directory(char *path, uint8_t *buffer) {
  FILINFO fno;
  int ret = 0;
  uint16_t num_dir_entries = 0;
  size_t path_ln = strlen(path);
  DIR dir;

  // We'll build entries in a local, aligned array, then serialize into buffer.
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

    // Serialize into buffer
    uint8_t *payload = buffer + 2;
    for (uint16_t i = 0; i < num_dir_entries; i++) {
      serialize_dir_entry(payload + i * sizeof(DIR_ENTRY_WIRE), &entries[i]);
    }

    uint16_t payload_size = (uint16_t)(num_dir_entries * sizeof(DIR_ENTRY_WIRE));
    write_u16_le(buffer, payload_size);
  } else {
    const char *err_msg = "Can't read directory";
    size_t msg_len = strlen(err_msg) + 1; // include NUL
    memcpy(buffer + 2, err_msg, msg_len);
    write_u16_le(buffer, (uint16_t)msg_len);
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

/*
 * mount_file
 * On success for MZF/M12:
 *   buffer[0..1] = total size (LE) of following payload
 *   buffer[2..]  = first 128 bytes header + body of length 'len' (len is at offset 18 in header)
 * On error:
 *   buffer[0..1] = size of error string
 *   buffer[2..]  = NUL-terminated error string
 */
int mount_file(char *path, uint8_t *buffer) {
  UINT br;
  uint16_t len;
  uint8_t *payload = buffer + 2;
  char extension[16];
  FIL fil;

  get_uppercase_extension(path, extension);
  if (!strcmp(extension, "MZF") || !strcmp(extension, "M12")) {
    if (f_open(&fil, path, FA_READ) != FR_OK) {
      // Keep error message short to be conservative about buffer size
      const int max_err = 200;
      int n = snprintf((char*)payload, (size_t)max_err, "Can't read file %s", path);
      if (n < 0) n = 0;
      uint16_t sz = (uint16_t)(n + 1);
      write_u16_le(buffer, sz);
      return 2;
    }

    // Read first 128 bytes (header)
    if (f_read(&fil, payload, 128, &br) != FR_OK || br != 128) {
      const char *err = "File read error (header)";
      size_t err_len = strlen(err) + 1;
      memcpy(payload, err, err_len);
      write_u16_le(buffer, (uint16_t)err_len);
      f_close(&fil);
      return 2;
    }

    // 'len' is stored at payload[18..19] (LE). Use memcpy to avoid unaligned access.
    memcpy(&len, payload + 18, sizeof(len));

    // Read body of length 'len' into payload+128
    if (f_read(&fil, payload + 128, len, &br) != FR_OK || br != len) {
      const char *err = "File read error (body)";
      size_t err_len = strlen(err) + 1;
      memcpy(payload, err, err_len);
      write_u16_le(buffer, (uint16_t)err_len);
      f_close(&fil);
      return 2;
    }
    f_close(&fil);

    // Total payload = header (128) + body (len)
    uint16_t total = (uint16_t)(128 + len);
    // But original code added 2? It did: len += 2 + 128; memcpy(buffer, &len, 2);
    // That indicates the consumer expects size to include the 2 length bytes as well.
    // Preserve protocol: size written at buffer[0..1] equals 2 + 128 + len.
    uint16_t reported = (uint16_t)(2 + total);
    write_u16_le(buffer, reported);

  } else if (!strcmp(extension, "DSK")) {
    fdc->set_drive_content(fdc, 0, path);
    // No payload size change for this branch in original code; keep behavior.
  } else if (!strcmp(extension, "MZQ")) {
    qd->set_drive_content(qd, path);
    // No payload size change here either.
  } else if (!strcmp(path, "#basic")) {
    // Copy size and data from basic[] safely
    uint16_t sz;
    memcpy(&sz, basic + 18, sizeof(sz));      // size at offset 18..19
    write_u16_le(buffer, sz);
    memcpy(buffer + 2, basic, sizeof(basic)); // payload
  }
  return 0;
}

/*
 * get_device_list
 * Writes a list of DEV_ENTRY items into buffer + 2, spaced by sizeof(DEV_ENTRY),
 * mirroring the original implementation. The size prefix is device_count * sizeof(DEV_ENTRY).
 */
int get_device_list(uint8_t *buffer) {
  uint8_t i;
  uint16_t sz;

  for (i = 0; i < device_count; i++) {
    // Keep behavior: write the device name at the start of each DEV_ENTRY slot.
    // The remainder of the slot (if any) remains unchanged.
    strcpy((char *)(buffer + 2 + sizeof(DEV_ENTRY) * i), devices[i].name);
  }
  sz = (uint16_t)(device_count * sizeof(DEV_ENTRY));
  write_u16_le(buffer, sz);
  return 0;
}
