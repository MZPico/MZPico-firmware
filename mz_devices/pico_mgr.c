#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "file.h"
#include "pico_mgr.h"

int pico_mgr_init(void *v_self) {
  PicoMgr *self = (PicoMgr *)v_self;
  self->idx = 0;
  //self->internal_idx = 2;
  self->response_command = 0;

  return 0;
}

int pico_mgr_write_control(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoMgr *self = (PicoMgr *)v_self;

  char path[256];
  int ret;
  uint16_t len;

  switch (dt) {
    case REPO_CMD_LIST_DIR:
      memcpy(&len, self->data, 2);
      if (len >= sizeof(path)) return -1;
      memcpy(path, &self->data[2], len);
      path[len] = 0;
      self->idx = 0;
      ret = read_directory(path, self->data);
      if (!ret)
        self->response_command = 0x03;
      else
        self->response_command = 0x04;
      break;
    case REPO_CMD_MOUNT:
      memcpy(&len, self->data, 2);
      if (len >= sizeof(path)) return -1;
      memcpy(path, &self->data[2], len);
      path[len] = 0;
      self->idx = 0;
      mount_file(path, self->data);
      self->response_command = 0x03;
      break;
    case REPO_CMD_LIST_DEV:
      self->idx = 0;
      get_device_list(self->data);
      self->response_command = 0x03;
  };
  return 0;
}

RAM_FUNC int pico_mgr_read_control(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  PicoMgr *self = (PicoMgr *)v_self;

  *dt = self->response_command;

  return 0;
}

RAM_FUNC int pico_mgr_write_data(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoMgr *self = (PicoMgr *)v_self;

  self->data[self->idx++] = dt;
//  if (self->idx >= self->size)
  if (self->idx >= PICO_MGR_BUFF_SIZE)
    self->idx = 0;
  return 0;
}

RAM_FUNC int pico_mgr_read_addr0(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  PicoMgr *self = (PicoMgr *)v_self;

  return self->idx & 0x00ff;
}

RAM_FUNC int pico_mgr_read_addr1(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  PicoMgr *self = (PicoMgr *)v_self;

  return (self->idx & 0xff00) >> 8;
}

RAM_FUNC int pico_mgr_write_addr0(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoMgr *self = (PicoMgr *)v_self;

  self->idx = (self->idx & 0xff00) | (uint16_t)dt;
  return 0;
}

RAM_FUNC int pico_mgr_write_addr1(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoMgr *self = (PicoMgr *)v_self;

  self->idx = (self->idx & 0x00ff) | ((uint16_t)dt << 8);
  return 0;
}


RAM_FUNC int pico_mgr_read_data(void *v_self, uint8_t port, uint8_t *dt, uint8_t high_addr) {
  PicoMgr *self = (PicoMgr *)v_self;

  *dt = self->data[self->idx++];
//  if (self->idx >= self->size)
  if (self->idx >= PICO_MGR_BUFF_SIZE)
    self->idx = 0;
  return 0;
}

RAM_FUNC int pico_mgr_write_reset(void *v_self, uint8_t port, uint8_t dt, uint8_t high_addr) {
  PicoMgr *self = (PicoMgr *)v_self;

  self->idx = 0;
  return 0;
}
/*
uint16_t add_and_serialize_record(void *v_self, void *record) {
  PicoMgr *self = (PicoMgr *)v_self;
  uint8_t packed_record[64];

  self->pack_struct(packed_record, records);
  if (self->internal_idx + self->record_size >= PICO_MGR_BUFF_SIZE) return 0; // overflow
  memcpy(self->data + self->internal_idx, packed_record, self->record_size);
  self->internal_idx += self->record_size;
  write_u16_le(self->data, read_u16_le(self->data) + self->record_size);
  return self->record_size;
}

uint16_t get_and_deserialize_next_record(void *v_self, void *record) {
  PicoMgr *self = (PicoMgr *)v_self;
  uint8_t unpacked_record[64];

  if (self->get_data_size(self)
  self->unpack_struct(self->data + self->internal_idx, unpacked_record);
}


void pico_cmd_set_data_size(void *v_self, uint16_t sz) {
  PicoMgr *self = (PicoMgr *)v_self;

  write_u16_le(self->data, sz);
}

uint16_t pico_cmd_get_data_size(void *v_self) {
  PicoMgr *self = (PicoMgr *)v_self;

  return read_u16_le(self->data);
}

uint16_t pico_cmd_get_record_count(void *v_self) {
  PicoMgr *self = (PicoMgr *)v_self;

  return read_u16_le(self->data) / self->record_size;
}

*/

PicoMgr *pico_mgr_new(uint8_t port_base) {
  PicoMgr *mgr = calloc(1, sizeof *mgr);

  if (!mgr) return NULL;

  mgr->read[PICO_MGR_COMMAND_PORT_INDEX].port = mgr->write[PICO_MGR_COMMAND_PORT_INDEX].port = port_base + PICO_MGR_COMMAND_PORT_INDEX;
  mgr->read[PICO_MGR_COMMAND_PORT_INDEX].fn = pico_mgr_read_control;
  mgr->write[PICO_MGR_COMMAND_PORT_INDEX].fn = pico_mgr_write_control;

  mgr->read[PICO_MGR_DATA_PORT_INDEX].port = mgr->write[PICO_MGR_DATA_PORT_INDEX].port = port_base + PICO_MGR_DATA_PORT_INDEX;
  mgr->read[PICO_MGR_DATA_PORT_INDEX].fn = pico_mgr_read_data;
  mgr->write[PICO_MGR_DATA_PORT_INDEX].fn = pico_mgr_write_data;

  mgr->read[PICO_MGR_ADDR0_PORT_INDEX].port = mgr->write[PICO_MGR_ADDR0_PORT_INDEX].port = port_base + PICO_MGR_ADDR0_PORT_INDEX;
  mgr->read[PICO_MGR_ADDR0_PORT_INDEX].fn = pico_mgr_read_addr0;
  mgr->write[PICO_MGR_ADDR0_PORT_INDEX].fn = pico_mgr_write_addr0;

  mgr->read[PICO_MGR_ADDR1_PORT_INDEX].port = mgr->write[PICO_MGR_ADDR1_PORT_INDEX].port = port_base + PICO_MGR_ADDR1_PORT_INDEX;
  mgr->read[PICO_MGR_ADDR1_PORT_INDEX].fn = pico_mgr_read_addr1;
  mgr->write[PICO_MGR_ADDR1_PORT_INDEX].fn = pico_mgr_write_addr1;

  mgr->write[PICO_MGR_RESET_PORT_INDEX].port = port_base + PICO_MGR_RESET_PORT_INDEX;
  mgr->write[PICO_MGR_RESET_PORT_INDEX].fn = pico_mgr_write_reset;

  mgr->iface.read = mgr->read;
  mgr->iface.read_port_count = PICO_MGR_READ_PORT_COUNT;

  mgr->iface.write = mgr->write;
  mgr->iface.write_port_count = PICO_MGR_WRITE_PORT_COUNT;

  mgr->iface.init = pico_mgr_init;
  mgr->iface.is_interrupt = NULL;
  mgr->iface.needs_exwait = 1;
  /*mgr->set_data_size = pico_cmd_set_data_size;*/
  return mgr;
}
