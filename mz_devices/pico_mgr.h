#ifndef __PICO_MGR_H__
#define __PICO_MGR_H__

#include <stdint.h>
#include "mz_devices.h"
#include "bus.h"
#include "common.h"

#define PICO_MGR_READ_PORT_COUNT 4
#define PICO_MGR_WRITE_PORT_COUNT 5

#define PICO_MGR_COMMAND_PORT_INDEX 0
#define PICO_MGR_DATA_PORT_INDEX 1
#define PICO_MGR_ADDR0_PORT_INDEX 2
#define PICO_MGR_ADDR1_PORT_INDEX 3
#define PICO_MGR_RESET_PORT_INDEX 4

typedef struct {
  MZDevice iface;
  uint8_t data[PICO_MGR_BUFF_SIZE];
  ReadPortMapping read[PICO_MGR_READ_PORT_COUNT];
  WritePortMapping write[PICO_MGR_WRITE_PORT_COUNT];

  uint8_t response_command;
  uint16_t idx;
  /*
  uint16_t internal_idx;
  size_t (*pack_struct)(uint8_t *dst, const void *structure);
  void (*unpack_struct)(const uint8_t *src, void *structure);
  void (*set_data_size)(void *v_self, uint16_t size);
  uint16_t record_size;
  */
} PicoMgr;

PicoMgr *pico_mgr_new(uint8_t base_port);

#endif
