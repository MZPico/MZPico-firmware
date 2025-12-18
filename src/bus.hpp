#ifndef __BUS_H_
#define __BUS_H_

#include "hardware/structs/sio.h"

#include "common.hpp"

#define ADDR_BUS_BASE    0
#define ADDR_BUS_COUNT   8

#ifdef BOARD_DELUXE
  #define DATA_BUS_BASE    0
#else
  #define DATA_BUS_BASE    8
#endif

#define DATA_BUS_COUNT   8

#define IORQ_PIN         20
#define RD_PIN           21
#define WR_PIN           22
#define RESET_PIN        19
#define INT_PIN          18
#define EXWAIT_PIN       16
#define M1_PIN           17

#ifdef BOARD_DELUXE
  #define EN0_PIN          8
  #define EN1_PIN          9
  #define EN2_PIN          10
  #define RD0_PIN          11
#endif

#define IO_REPO_BASE_ADDR  0x40

#define REPO_CMD_LIST_DIR   0x01
#define REPO_CMD_MOUNT      0x03
#define REPO_CMD_UPLOAD     0x04
#define REPO_CMD_LIST_DEV   0x05
#define REPO_CMD_LIST_WF    0x07
#define REPO_CMD_CONN_WF    0x08
#define REPO_CMD_LIST_REPOS 0x09
#define REPO_CMD_CHREPO     0x0a
#define REPO_CMD_GET_CONFIG     0x0b
#define REPO_CMD_GET_WIFI_STATUS 0x0c

#define PICO_MGR_BUFF_SIZE (0xd000 - 0x1200 + 128 + 2 + 4)

#define DATA_BUS_MASK    (((1u << DATA_BUS_COUNT) - 1) << DATA_BUS_BASE)
#define READ_PINS(pins, base, count) (((pins) >> (base)) & ((1u << (count)) - 1))

ALWAYS_INLINE uint8_t read_address_bus(void) {
  return (sio_hw->gpio_in >> ADDR_BUS_BASE) & 0xFF;
}

ALWAYS_INLINE uint8_t read_data_bus(void) {
  return (sio_hw->gpio_in >> DATA_BUS_BASE) & 0xFF;
}

ALWAYS_INLINE void read_address_data_bus(uint8_t *addr_out, uint8_t *data_out) {
  *addr_out = (sio_hw->gpio_in >> ADDR_BUS_BASE) & 0xFF;
  *data_out = (sio_hw->gpio_in >> DATA_BUS_BASE) & 0xFF;
}

ALWAYS_INLINE void write_data_bus(uint8_t value) {
  sio_hw->gpio_clr = DATA_BUS_MASK;
  sio_hw->gpio_set = ((uint32_t)value << DATA_BUS_BASE);
}

ALWAYS_INLINE void acquire_data_bus_for_writing(void) {
  sio_hw->gpio_oe_set = DATA_BUS_MASK;
}

ALWAYS_INLINE void release_data_bus(void) {
  sio_hw->gpio_oe_clr = DATA_BUS_MASK;
}

ALWAYS_INLINE void set_interrupt(void) {
  sio_hw->gpio_clr = 1u << INT_PIN;
  sio_hw->gpio_oe_set = 1u << INT_PIN;
}

ALWAYS_INLINE void release_interrupt(void) {
  sio_hw->gpio_oe_clr = 1u << INT_PIN;
}

ALWAYS_INLINE void set_exwait(void) {
  sio_hw->gpio_clr = 1u << EXWAIT_PIN;
  sio_hw->gpio_oe_set = 1u << EXWAIT_PIN;
}

ALWAYS_INLINE void release_exwait(void) {
  sio_hw->gpio_oe_clr = 1u << EXWAIT_PIN;
}

#endif
