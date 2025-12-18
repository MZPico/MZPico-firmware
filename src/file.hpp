#ifndef __FILE_HPP__
#define __FILE_HPP__

#include <stdint.h>
#include "pico_mgr.hpp"

#define MAX_DIR_FILES 930
#define MAX_FILENAME_LENGTH 32
#define MAX_DEV_NAME_LENGTH 8
#define MAX_DEVICES 3

typedef struct {
  char name[MAX_DEV_NAME_LENGTH];
} DEV_ENTRY;

int read_directory(const char *path, PicoMgr *mgr);
int mount_file(const char *path, PicoMgr *mgr);
int get_device_list(PicoMgr *mgr);
int mount_devices(void);

#ifdef USE_PICO_W
// Cloud (WiFi) helpers
int cloud_read_directory(const char *path, PicoMgr *mgr);
int cloud_mount_file(const char *path, PicoMgr *mgr);
// Launch asynchronous WiFi initialization on core1.
int cloud_launch_async(void);
// Add cloud device to device list once WiFi connected.
void cloud_add_device(std::string new_device);
#endif

extern DEV_ENTRY devices[MAX_DEVICES];
extern uint8_t device_count;

#endif
