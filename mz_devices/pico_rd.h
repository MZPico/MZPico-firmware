#ifndef __PICO_RD_H__
#define __PICO_RD_H__

#include <stdint.h>
#include "mz_devices.h"
#include "common.h"

#define PICO_RD_READ_PORT_COUNT 5
#define PICO_RD_WRITE_PORT_COUNT 7

#define PICO_RD_CONTROL_PORT_INDEX 0
#define PICO_RD_DATA_PORT_INDEX 1
#define PICO_RD_ADDR2_PORT_INDEX 2
#define PICO_RD_ADDR1_PORT_INDEX 3
#define PICO_RD_ADDR0_PORT_INDEX 4
#define PICO_RD_ADDRS_PORT_INDEX 5
#define PICO_RD_ADDRI_PORT_INDEX 6


typedef struct {
  MZDevice iface;
  ReadPortMapping read[PICO_RD_READ_PORT_COUNT];
  WritePortMapping write[PICO_RD_WRITE_PORT_COUNT];
  uint8_t *data;
  uint32_t size;
  uint32_t idx;
  uint8_t addr_idx;
} PicoRD;

PicoRD *pico_rd_new(uint8_t base_port, uint8_t *data, uint32_t size);

#endif
