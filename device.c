#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"

#include "device.h"
#include "common.h"
#include "gen_rd.h"
#include "file.h"
#include "trap_read.pio.h"
#include "trap_write.pio.h"
#include "trap_reset.pio.h"
#include "device_fw.h"

// Control pins array
const uint control_pins[] = { IORQ_PIN, RD_PIN, WR_PIN };
const uint control_pins_count = sizeof(control_pins) / sizeof(control_pins[0]);

/*
typedef struct {
  uint8_t read_pnt;
  uint8_t write_pnt;
  uint8_t *content;
} RAMDISK;
*/

// --- Globals ---
PIO pio = pio0;
uint sm = 0;

GEN_RAMDISK(sram, 16384, 0xf8, 0xf9, 0xfa)
GEN_RAMDISK(comm_buffer, 0xD000 - 0x1200 + 128 + 2 + 4, 0x42, 0x41, 0x41)
PICO_RAMDISK(pico_rd, 0x18000, 0x43, 0x44, 0x44, 0x45, 0x46, 0x47, 0x48)

GEN_RD *ramdisks[] = {
  &comm_buffer,
  &sram,
};

#define RAMDISKS_SIZE (sizeof(ramdisks) / sizeof(ramdisks[0]))

volatile uint8_t request_command = 0;
volatile uint8_t response_command = 0;

void blink(uint8_t cnt) {
  for (int i=0; i<cnt; i++) {
    gpio_put(25, true);
    sleep_ms(200);
    gpio_put(25, false);
    sleep_ms(200);
  }
}


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

// --- IRQ Handler ---
RAM_FUNC void pio0_irq0_handler(void) {
  uint8_t i;
  uint8_t addr;
  uint8_t data;
  uint32_t raw;
  GEN_RD *rd;
  if (pio_interrupt_get(pio, 0)) {
    pio_interrupt_clear(pio, 0);

    addr = read_address_bus();
    if (addr == IO_REPO_COMMAND_ADDR) {
      acquire_data_bus_for_writing();
      write_data_bus(response_command);
    } else {
      for (i=0; i<RAMDISKS_SIZE; i++) {
        rd = ramdisks[i];
        if (addr == rd->port_reset) {
          gen_rd_reset(rd);
          break;
        } else if (addr == rd->port_read) {
          acquire_data_bus_for_writing();
          write_data_bus(gen_rd_read(rd));
          break;
        }
      }
      if (addr == pico_rd.port_read) {
        acquire_data_bus_for_writing();
        write_data_bus(pico_rd_read(&pico_rd));
      } else if (addr == pico_rd.port_control) {
        pico_rd_reset(&pico_rd);
      }
    }
  }

  if (pio_interrupt_get(pio, 1)) {
    pio_interrupt_clear(pio, 1);

    read_address_data_bus(&addr, &data);
    if (addr == IO_REPO_COMMAND_ADDR) {
      if ((response_command != 0x01) && (response_command != 0x02)) {
        request_command = data;
        response_command = 0x01;
      }
    } else {
      for (i=0; i<RAMDISKS_SIZE; i++) {
        rd = ramdisks[i];
        if (addr == rd->port_write) {
          gen_rd_write(rd, data);
          break;
        }
      }
      if (addr == pico_rd.port_write) {
        pico_rd_write(&pico_rd, data);
      } else if (addr == pico_rd.port_addr3) {
        pico_rd_set_addr3(&pico_rd, data);
      } else if (addr == pico_rd.port_addr2) {
        pico_rd_set_addr2(&pico_rd, data);
      } else if (addr == pico_rd.port_addr1) {
        pico_rd_set_addr1(&pico_rd, data);
      } else if (addr == pico_rd.port_addr_serial) {
        pico_rd_set_addr_serial(&pico_rd, data);
      }
    }
  }

  if (pio_interrupt_get(pio, 2)) {
    pio_interrupt_clear(pio, 2);
    release_data_bus();
  }

  if (pio_interrupt_get(pio, 3)) {
    pio_interrupt_clear(pio, 3);
    watchdog_reboot(0, 0, 0);
  }

}

RAM_FUNC void handle_command(){
  char path[256];
  int ret;
  uint16_t len;

  if (!request_command)
    return;

  response_command = 0x02;
  switch (request_command) {
    case REPO_CMD_LIST_DIR:
      memcpy(&len, comm_buffer.data, 2);
      memcpy(path, comm_buffer.data + 2, len);
      path[len] = 0;
      gen_rd_reset(&comm_buffer);
      ret = read_directory(path, &comm_buffer);
      if (!ret)
        response_command = 0x03;
      else
        response_command = 0x04;
      break;
    case REPO_CMD_MOUNT:
      memcpy(&len, comm_buffer.data, 2);
      memcpy(path, &comm_buffer.data[2], len);
      path[len] = 0;
      gen_rd_reset(&comm_buffer);
      mount_file(path, &comm_buffer);
      response_command = 0x03;
      break;
  };
}

// --- GPIO Init ---
RAM_FUNC void init_gpio(void) {
    for (int i = 0; i < ADDR_BUS_COUNT; ++i) {
        gpio_init(ADDR_BUS_BASE + i);
        gpio_set_dir(ADDR_BUS_BASE + i, GPIO_IN);
    }

    for (int i = 0; i < DATA_BUS_COUNT; ++i) {
        gpio_init(DATA_BUS_BASE + i);
        gpio_set_dir(DATA_BUS_BASE + i, GPIO_IN);
    }

    for (size_t i = 0; i < control_pins_count; ++i) {
        gpio_init(control_pins[i]);
        gpio_set_dir(control_pins[i], GPIO_IN);
        gpio_pull_up(control_pins[i]);
    }
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, true);
    sleep_ms(20);
    gpio_put(25, false);
    sleep_ms(20);
}

RAM_FUNC void device_main(void) {
    set_sys_clock_khz(250000, true);

    init_gpio();
    memcpy(sram.data, firmware, sizeof(firmware));
    trap_read_init(pio, 0, ADDR_BUS_BASE, RD_PIN);
    trap_write_init(pio, 1, ADDR_BUS_BASE, WR_PIN);
    trap_reset_init(pio, 2);

    irq_set_exclusive_handler(PIO0_IRQ_0, pio0_irq0_handler);
    irq_set_enabled(PIO0_IRQ_0, true);

    pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
    pio_set_irq0_source_enabled(pio, pis_interrupt1, true);
    pio_set_irq0_source_enabled(pio, pis_interrupt2, true);
    pio_set_irq0_source_enabled(pio, pis_interrupt3, true);

    while (1) {
        if ((request_command != 0) && (response_command == 0x01))
          handle_command();
        tight_loop_contents();
    }
}

