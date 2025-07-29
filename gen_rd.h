#ifndef __GEN_RD_H__
#define __GEN_RD_H__

#include <stdint.h>

#include "common.h"

typedef struct {
  uint8_t *data;
  uint16_t size;
  uint16_t idx;
  uint8_t port_reset;
  uint8_t port_read;
  uint8_t port_write;
} GEN_RD;

typedef struct {
  uint8_t *data;
  uint32_t size;
  uint32_t idx;
  uint32_t addr_idx;
  uint8_t port_control;
  uint8_t port_read;
  uint8_t port_write;
  uint8_t port_addr3;
  uint8_t port_addr2;
  uint8_t port_addr1;
  uint8_t port_addr_serial;
} PICO_RD;

#define GEN_RD_RW_SEPARATE 1
#define GEN_RD_RW_COMMON 0

#define GEN_RAMDISK(name, sz, p_reset, p_read, p_write)   \
        uint8_t name##_data[sz];    \
        GEN_RD name = {             \
          .data = name##_data,      \
          .size = sz,               \
          .idx = 0,                 \
          .port_reset = p_reset,    \
          .port_read = p_read,      \
          .port_write = p_write     \
        };

#define PICO_RAMDISK(name, sz, p_control, p_read, p_write, p_addr3, p_addr2, p_addr1, p_addr_serial)   \
        uint8_t name##_data[sz];    \
        PICO_RD name = {            \
          .data = name##_data,      \
          .size = sz,               \
          .idx = 0,                 \
          .addr_idx = 0,            \
          .port_control = p_control,\
          .port_read = p_read,      \
          .port_write = p_write,    \
          .port_addr3 = p_addr3,    \
          .port_addr2 = p_addr2,    \
          .port_addr1 = p_addr1,    \
          .port_addr_serial = p_addr_serial     \
        };

ALWAYS_INLINE void gen_rd_reset(GEN_RD *rd) {
  rd->idx = 0;
}

ALWAYS_INLINE void gen_rd_write(GEN_RD *rd, uint8_t byte) {
  rd->data[rd->idx++] = byte;
  if (rd->idx >= rd->size)
    rd->idx = 0;
}

ALWAYS_INLINE uint8_t gen_rd_read(GEN_RD *rd) {
  uint8_t val;

  val = rd->data[rd->idx++];
  if (rd->idx >= rd->size)
    rd->idx = 0;
  return val;
}

ALWAYS_INLINE void pico_rd_reset(PICO_RD *rd) {
  rd->idx = 0;
  rd->addr_idx = 0;
}

ALWAYS_INLINE void pico_rd_write(PICO_RD *rd, uint8_t byte) {
  rd->data[rd->idx++] = byte;
  if (rd->idx >= rd->size)
    rd->idx = 0;
  rd->addr_idx = 0;
}

ALWAYS_INLINE uint8_t pico_rd_read(PICO_RD *rd) {
  uint8_t val;

  val = rd->data[rd->idx++];
  if (rd->idx >= rd->size)
    rd->idx = 0;
  rd->addr_idx = 0;
  return val;
}


ALWAYS_INLINE void pico_rd_set_addr3(PICO_RD *rd, uint8_t byte) {
  rd->idx = (rd->idx & 0x0000ffff) | ((uint32_t)byte << 16);
}

ALWAYS_INLINE void pico_rd_set_addr2(PICO_RD *rd, uint8_t byte) {
  rd->idx = (rd->idx & 0xffff00ff) | ((uint32_t)byte << 8);
}

ALWAYS_INLINE void pico_rd_set_addr1(PICO_RD *rd, uint8_t byte) {
  rd->idx = (rd->idx & 0xffffff00) | ((uint32_t)byte);
}

ALWAYS_INLINE void pico_rd_set_addr_serial(PICO_RD *rd, uint8_t byte) {
  rd->idx = (rd->idx & ~(0xff << (rd->addr_idx * 8))) | ((uint32_t)byte << (rd->addr_idx * 8));
  rd->addr_idx++;
  if (rd->addr_idx > 2)
    rd->addr_idx = 0;
}

#endif
