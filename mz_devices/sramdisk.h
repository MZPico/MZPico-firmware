#ifndef __SRAMDISK_H__
#define __SRAMDISK_H__

#include <stdint.h>
#include "mz_devices.h"
#include "common.h"

#define SRAMDISK_READ_PORT_COUNT 2
#define SRAMDISK_WRITE_PORT_COUNT 1

#define SRAMDISK_RESET_PORT_INDEX 0
#define SRAMDISK_READ_PORT_INDEX 1
#define SRAMDISK_WRITE_PORT_INDEX 0


typedef struct {
  MZDevice iface;
  ReadPortMapping read[SRAMDISK_READ_PORT_COUNT];
  WritePortMapping write[SRAMDISK_WRITE_PORT_COUNT];
  uint8_t *data;
  uint16_t size;
  uint16_t idx;
} SRamDisk;

SRamDisk *sramdisk_new(uint8_t reset_port, uint8_t read_port, uint8_t write_port, uint8_t *data, uint16_t size);

int sramdisk_write(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr);
int sramdisk_reset(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr);
int sramdisk_read(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr);

#endif
