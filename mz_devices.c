#include "common.h"
#include "mz_devices.h"

MZDevice *mz_devices[MAX_MZ_DEVICES];
uint8_t mz_device_count = 0;
MZDevice *mz_device_read_port_map[256] = {0};
MZDevice *mz_device_write_port_map[256] = {0};
read_fn_t read_port_map[256] = {0};
write_fn_t write_port_map[256] = {0};


int mz_device_register(MZDevice *dev) {
  int i;
  uint8_t port;

  if (mz_device_count >= MAX_MZ_DEVICES)
    return E_MAX_DEVICES;

  for (i = 0; i < dev->read_port_count; i++) {
    port = dev->read[i].port;
    //if (mz_device_read_port_map[port])
    //  return E_PORT_ALLOCATED;
    mz_device_read_port_map[port] = dev;
    read_port_map[port] = dev->read[i].fn;
  }
  for (i = 0; i < dev->write_port_count; i++) {
    port = dev->write[i].port;
    //if (mz_device_write_port_map[port])
    //  return E_PORT_ALLOCATED;
    mz_device_write_port_map[port] = dev;
    write_port_map[port] = dev->write[i].fn;
  }
  mz_devices[mz_device_count++] = dev;
}
