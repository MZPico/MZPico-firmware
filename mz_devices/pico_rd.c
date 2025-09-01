#include <stdio.h>
#include <stdlib.h>
#include "pico_rd.h"

RAM_FUNC int pico_rd_init(void *v_self) {
  PicoRD *self = (PicoRD *)v_self;

  self->idx = 0;
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_write_control(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  self->idx = 0;
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_read_control(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  self->idx = 0;
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_write_data(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  self->data[self->idx++] = dt;
  if (self->idx >= self->size)
    self->idx = 0;
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_read_data(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  *dt = self->data[self->idx++];
  if (self->idx >= self->size)
    self->idx = 0;
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_write_addr2(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  self->idx = (self->idx & 0xff00ffff) | ((uint32_t)dt << 16);
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_read_addr2(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  *dt = (self->idx & 0x00ff0000) >> 16;
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_write_addr1(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  self->idx = (self->idx & 0xffff00ff) | ((uint32_t)dt << 8);
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_read_addr1(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  *dt = (self->idx & 0x0000ff00) >> 8;
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_write_addr0(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  self->idx = (self->idx & 0xffffff00) | ((uint32_t)dt);
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_read_addr0(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  *dt = (self->idx & 0x000000ff);
  self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_write_addrs(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  self->idx = (self->idx & ~(0xff << (self->addr_idx * 8))) | ((uint32_t)dt << (self->addr_idx * 8));
  self->addr_idx++;
  if (self->addr_idx > 2)
    self->addr_idx = 0;
  return 0;
}

RAM_FUNC int pico_rd_write_addri(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoRD *self = (PicoRD *)v_self;

  self->idx += (uint32_t)dt;
  if (self->idx >= self->size)
    self->idx = (self->idx - self->size);
  return 0;
}


PicoRD *pico_rd_new(uint8_t base_port, uint8_t *data, uint32_t size) {
  PicoRD *rd = calloc(1, sizeof *rd);

  rd->read[PICO_RD_CONTROL_PORT_INDEX].port = rd->write[PICO_RD_CONTROL_PORT_INDEX].port = base_port + PICO_RD_CONTROL_PORT_INDEX;
  rd->read[PICO_RD_CONTROL_PORT_INDEX].fn = pico_rd_read_control;
  rd->write[PICO_RD_CONTROL_PORT_INDEX].fn = pico_rd_write_control;

  rd->read[PICO_RD_DATA_PORT_INDEX].port = rd->write[PICO_RD_DATA_PORT_INDEX].port = base_port + PICO_RD_DATA_PORT_INDEX;
  rd->read[PICO_RD_DATA_PORT_INDEX].fn = pico_rd_read_data;
  rd->write[PICO_RD_DATA_PORT_INDEX].fn = pico_rd_write_data;

  rd->read[PICO_RD_ADDR2_PORT_INDEX].port = rd->write[PICO_RD_ADDR2_PORT_INDEX].port = base_port + PICO_RD_ADDR2_PORT_INDEX;
  rd->read[PICO_RD_ADDR2_PORT_INDEX].fn = pico_rd_read_addr2;
  rd->write[PICO_RD_ADDR2_PORT_INDEX].fn = pico_rd_write_addr2;

  rd->read[PICO_RD_ADDR1_PORT_INDEX].port = rd->write[PICO_RD_ADDR1_PORT_INDEX].port = base_port + PICO_RD_ADDR1_PORT_INDEX;
  rd->read[PICO_RD_ADDR1_PORT_INDEX].fn = pico_rd_read_addr1;
  rd->write[PICO_RD_ADDR1_PORT_INDEX].fn = pico_rd_write_addr1;

  rd->read[PICO_RD_ADDR0_PORT_INDEX].port = rd->write[PICO_RD_ADDR0_PORT_INDEX].port = base_port + PICO_RD_ADDR0_PORT_INDEX;
  rd->read[PICO_RD_ADDR0_PORT_INDEX].fn = pico_rd_read_addr0;
  rd->write[PICO_RD_ADDR0_PORT_INDEX].fn = pico_rd_write_addr0;

  rd->write[PICO_RD_ADDRS_PORT_INDEX].port = base_port + PICO_RD_ADDRS_PORT_INDEX;
  rd->write[PICO_RD_ADDRS_PORT_INDEX].fn = pico_rd_write_addrs;

  rd->write[PICO_RD_ADDRI_PORT_INDEX].port = base_port + PICO_RD_ADDRI_PORT_INDEX;
  rd->write[PICO_RD_ADDRI_PORT_INDEX].fn = pico_rd_write_addri;

  rd->iface.read = rd->read;
  rd->iface.read_port_count = PICO_RD_READ_PORT_COUNT;

  rd->iface.write = rd->write;
  rd->iface.write_port_count = PICO_RD_WRITE_PORT_COUNT;

  rd->iface.init = pico_rd_init;
  rd->iface.is_interrupt = NULL;
  rd->iface.needs_exwait = 0;

  rd->data = data;
  rd->size = size;
  return rd;
}
