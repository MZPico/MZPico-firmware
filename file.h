#ifndef __FILE_H__
#define __FILE_H__

#include <stdint.h>

#define MAX_DIR_FILES 930
#define MAX_FILENAME_LENGTH 32
#define MAX_DEV_NAME_LENGTH 8
#define MAX_DEVICES 2

typedef struct __attribute__((__packed__)) {
  char name[MAX_DEV_NAME_LENGTH];
} DEV_ENTRY;

int read_directory(char *path, uint8_t *buffer);
int mount_file(char *path, uint8_t *buffer);
int get_device_list(uint8_t *buffer);
int mount_devices(void);

extern DEV_ENTRY devices[MAX_DEVICES];
extern uint8_t device_count;

#endif
