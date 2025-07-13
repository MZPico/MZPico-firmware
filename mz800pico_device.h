#ifndef __MZ800PICO_DEVICE_H__
#define __MZ800PICO_DEVICE_H__

#include "pico/stdlib.h"

// --- Pin Definitions ---
#define ADDR_BUS_BASE    0
#define ADDR_BUS_COUNT   8

#define DATA_BUS_BASE    8
#define DATA_BUS_COUNT   8

#define IORQ_PIN         16
#define RD_PIN           17
#define WR_PIN           18

#define IO_REPO_COMMAND_ADDR  0x40
#define IO_REPO_DATA_ADDR  0x41

#define REPO_CMD_LIST_DIR   0x01
#define REPO_CMD_MOUNT      0x03
#define REPO_CMD_UPLOAD     0x04
#define REPO_CMD_LIST_DEV   0x05
#define REPO_CMD_CHDEV      0x06
#define REPO_CMD_LIST_WF    0x07
#define REPO_CMD_CONN_WF    0x08
#define REPO_CMD_LIST_REPOS 0x09
#define REPO_CMD_CHREPO     0x0a

#define DATA_BUS_MASK    (((1u << DATA_BUS_COUNT) - 1) << DATA_BUS_BASE)

// --- Macros ---
#define READ_PINS(pins, base, count) (((pins) >> (base)) & ((1u << (count)) - 1))

void device_main(void);
void blink(uint8_t i);

#endif
