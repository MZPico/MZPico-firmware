#ifndef __FILE_HPP__
#define __FILE_HPP__

#include <stdint.h>
#include "pico_mgr.hpp"

#define MAX_DIR_FILES 930
#define MAX_FILENAME_LENGTH 32
#define MAX_DEV_NAME_LENGTH 8
#define MAX_DEVICES 2

typedef struct {
  char name[MAX_DEV_NAME_LENGTH];
} DEV_ENTRY;

int read_directory(const char *path, PicoMgr *mgr);
int mount_file(const char *path, PicoMgr *mgr);
int get_device_list(PicoMgr *mgr);
int mount_devices(void);

extern DEV_ENTRY devices[MAX_DEVICES];
extern uint8_t device_count;

#endif
