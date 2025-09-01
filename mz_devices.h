#ifndef __MZ_DEVICES__
#define __MZ_DEVICES__

#include <stdint.h>
#include "common.h"

#define MAX_MZ_DEVICES 64
#define E_PORT_ALLOCATED 255
#define E_PORT_NOT_AVAILABLE 254
#define E_MAX_DEVICES 253

typedef int (*read_fn_t)(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr);
typedef int (*write_fn_t)(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr);

typedef struct {
  uint8_t port;
  read_fn_t fn;
} ReadPortMapping;

typedef struct {
  uint8_t port;
  write_fn_t fn;
} WritePortMapping;

typedef struct {
  ReadPortMapping *read;
  uint8_t read_port_count;
  WritePortMapping *write;
  uint8_t write_port_count;
  int (*init)(void *v_self);
  int (*is_interrupt)(void *v_self);
  uint8_t needs_exwait;
} MZDevice;

int mz_device_register(MZDevice *dev);

extern MZDevice *mz_device_read_port_map[256];
extern MZDevice *mz_device_write_port_map[256];
extern read_fn_t read_port_map[256];
extern write_fn_t write_port_map[256];

ALWAYS_INLINE read_fn_t get_mz_port_read(uint8_t port, MZDevice **dev) {
  *dev = mz_device_read_port_map[port];
  return read_port_map[port];
}

ALWAYS_INLINE write_fn_t get_mz_port_write(uint8_t port, MZDevice **dev) {
  *dev = mz_device_write_port_map[port];
  return write_port_map[port];
}

#endif
