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

#define FLASH_ID "flash"
#define SD_ID "sd"
#ifdef USE_PICO_W
#define CLOUD_ID "cloud"
#endif

uint8_t device_count;
DEV_ENTRY devices[MAX_DEVICES];
FATFS fatfs_flash;
FATFS fatfs_sd;

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
