#include <stdio.h>
#include <stdlib.h>
#include "sramdisk.h"

RAM_FUNC int sramdisk_init(void *v_self) {
  SRamDisk *self = (SRamDisk *)v_self;

  self->idx = 0;
  return 0;
}

RAM_FUNC int sramdisk_write(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  SRamDisk *self = (SRamDisk *)v_self;

  self->data[self->idx++] = dt;
  if (self->idx >= self->size)
    self->idx = 0;
  return 0;
}

RAM_FUNC int sramdisk_reset(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  SRamDisk *self = (SRamDisk *)v_self;

  self->idx = 0;
  return 0;
}

RAM_FUNC int sramdisk_read(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  SRamDisk *self = (SRamDisk *)v_self;

  *dt = self->data[self->idx++];
  if (self->idx >= self->size)
    self->idx = 0;
  return 0;
}

SRamDisk *sramdisk_new(uint8_t reset_port, uint8_t read_port, uint8_t write_port, uint8_t *data, uint16_t size) {
  SRamDisk *rd = calloc(1, sizeof *rd);

  rd->read[SRAMDISK_RESET_PORT_INDEX].port = reset_port;
  rd->read[SRAMDISK_RESET_PORT_INDEX].fn = sramdisk_reset;

  rd->read[SRAMDISK_READ_PORT_INDEX].port = read_port;
  rd->read[SRAMDISK_READ_PORT_INDEX].fn = sramdisk_read;

  rd->write[SRAMDISK_WRITE_PORT_INDEX].port = write_port;
  rd->write[SRAMDISK_WRITE_PORT_INDEX].fn = sramdisk_write;

  rd->iface.read = rd->read;
  rd->iface.read_port_count = SRAMDISK_READ_PORT_COUNT;

  rd->iface.write = rd->write;
  rd->iface.write_port_count = SRAMDISK_WRITE_PORT_COUNT;

  rd->iface.init = sramdisk_init;
  rd->iface.is_interrupt = NULL;
  rd->iface.needs_exwait = 0;

  rd->data = data;
  rd->size = size;
  
  return rd;
}
