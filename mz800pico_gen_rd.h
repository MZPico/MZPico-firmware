#ifndef __MZ800PICO_GEN_RD_H__
#define __MZ800PICO_GEN_RD_H__

#include <stdint.h>

#include "mz800pico_common.h"

typedef struct {
  uint8_t *data;
  uint16_t size;
  uint16_t read_idx;
  uint16_t write_idx;
  uint8_t rw_separate;
  uint8_t port_reset;
  uint8_t port_read;
  uint8_t port_write;
} GEN_RD;

#define GEN_RD_RW_SEPARATE 1
#define GEN_RD_RW_COMMON 0

#define GEN_RAMDISK(name, sz, rw, p_reset, p_read, p_write)   \
        uint8_t name##_data[sz];    \
        GEN_RD name = {             \
          .data = name##_data,      \
          .size = sz,               \
          .read_idx = 0,            \
          .write_idx = 0,           \
          .rw_separate = rw,        \
          .port_reset = p_reset,    \
          .port_read = p_read,      \
          .port_write = p_write     \
        };

ALWAYS_INLINE void gen_rd_reset(GEN_RD *rd) {
  rd->read_idx = 0;
  rd->write_idx = 0;
}

ALWAYS_INLINE void gen_rd_reset_read(GEN_RD *rd) {
  rd->read_idx = 0;
  if (rd->rw_separate == GEN_RD_RW_COMMON)
    rd->write_idx = 0;
}

ALWAYS_INLINE void gen_rd_reset_write(GEN_RD *rd) {
  rd->write_idx = 0;
  if (rd->rw_separate == GEN_RD_RW_COMMON)
    rd->read_idx = 0;
}

ALWAYS_INLINE void gen_rd_write(GEN_RD *rd, uint8_t byte) {
  rd->data[rd->write_idx++] = byte;
  if (rd->write_idx >= rd->size)
    rd->write_idx = 0;
  if (rd->rw_separate == GEN_RD_RW_COMMON)
    rd->read_idx = rd->write_idx;
}

ALWAYS_INLINE uint8_t gen_rd_read(GEN_RD *rd) {
  uint8_t val;

  val = rd->data[rd->read_idx++];
  if (rd->read_idx >= rd->size)
    rd->read_idx = 0;
  if (rd->rw_separate == GEN_RD_RW_COMMON)
    rd->write_idx = rd->read_idx;
  return val;
}

#endif
