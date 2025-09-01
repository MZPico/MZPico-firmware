#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"
#include "hardware/vreg.h"

#include "common.h"
#include "bus.h"
#include "file.h"
#include "trap_read.pio.h"
#include "trap_write.pio.h"
#include "trap_reset.pio.h"
#include "device_fw.h"
#include "device.h"
#include "fdc.h"
#include "qd.h"
#include "sramdisk.h"
#include "pico_rd.h"
#include "pico_mgr.h"
#include "sramdisk.h"
#include "pico_rd.h"
#include "ff.h"
#include "fatfs_disk.h"
#include "mz_devices.h"

#define SM_RESET 0
#define SM_READ 1
#define SM_WRITE 2

// Control pins array
const uint control_pins[] = { IORQ_PIN, RD_PIN, WR_PIN };
const uint control_pins_count = sizeof(control_pins) / sizeof(control_pins[0]);


// --- Globals ---
PIO pio = pio0;
uint sm = 0;

//uint8_t pico_mgr_buff[PICO_MGR_BUFF_SIZE];
PicoMgr *pico_mgr;

FDC *fdc;
QD *qd;

#define BOOT_RD_SIZE 16384
uint8_t boot_rd_data[BOOT_RD_SIZE];
SRamDisk *boot_rd;

#define PICO_RD_SIZE 0x18000
uint8_t pico_rd_data[PICO_RD_SIZE];
PicoRD *pico_rd;

void blink(uint8_t cnt) {
  for (int i=0; i<cnt; i++) {
    gpio_put(25, true);
    sleep_ms(200);
    gpio_put(25, false);
    sleep_ms(200);
  }
}

RAM_FUNC void reset_handler(void) {
  pio_interrupt_clear(pio, 0);
  watchdog_reboot(0, 0, 0);
}
/*
RAM_FUNC void listen_loop(void) {
  uint8_t addr;
  uint8_t data;
  MZDevice *dev;
  read_fn_t read;
  write_fn_t write;
  uint32_t pins;

  while (1) {
    if (!pio_sm_is_rx_fifo_empty(pio, SM_READ)) {
      addr = (uint8_t)(pio_sm_get(pio, SM_READ) >>24);
      read = get_mz_port_read(addr, &dev);
      if (read) {
        if (dev->needs_exwait) set_exwait();
        read(dev, addr, &data, 0);
        acquire_data_bus_for_writing();
        write_data_bus(data);
        if (dev->needs_exwait) release_exwait();
        if (dev->is_interrupt && dev->is_interrupt(dev)) set_interrupt();
      };
      pio_sm_put(pio, SM_READ, IO_END);
    } else if (!pio_sm_is_rx_fifo_empty(pio, SM_WRITE)) {
      pins = pio_sm_get(pio, SM_WRITE);
      addr = (uint8_t)((pins >> 16) & 0xff);
      data = (uint8_t)(pins >> 24);
      write = get_mz_port_write(addr, &dev);
      if (write) {
        if (dev->needs_exwait) set_exwait();
        write(dev, addr, data, 0);
        if (dev->needs_exwait) release_exwait();
        if (dev->is_interrupt && dev->is_interrupt(dev)) set_interrupt();
      }
    }
  }
}
*/

RAM_FUNC void listen_loop(void) {
  uint8_t addr;
  uint8_t data;
  MZDevice *dev;
  read_fn_t read;
  write_fn_t write;
  uint32_t pins;

  while (1) {
    while (sio_hw->gpio_in & (1<<IORQ_PIN));
    pins = sio_hw->gpio_in;
    //if (!(sio_hw->gpio_in & (1<<RD_PIN))) {
    if (!(pins & (1<<RD_PIN))) {
      addr = read_address_bus();
      read = get_mz_port_read(addr, &dev);
      if (read) {
        if (dev->needs_exwait) set_exwait();
        read(dev, addr, &data, 0);
        acquire_data_bus_for_writing();
        write_data_bus(data);
        if (dev->needs_exwait) release_exwait();
        if (dev->is_interrupt && dev->is_interrupt(dev)) set_interrupt();
      }
    } else if (!(pins & (1<<WR_PIN))) {
      read_address_data_bus(&addr, &data);
      write = get_mz_port_write(addr, &dev);
      if (write) {
        if (dev->needs_exwait) set_exwait();
        write(dev, addr, data, 0);
        if (dev->needs_exwait) release_exwait();
        if (dev->is_interrupt && dev->is_interrupt(dev)) set_interrupt();
      }
    }
    while (!(sio_hw->gpio_in & (1<<IORQ_PIN)));
    release_data_bus();
  }
}

void init_gpio(void) {
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
    gpio_init(INT_PIN);
    gpio_init(EXWAIT_PIN);
    gpio_set_dir(INT_PIN, GPIO_IN);
    gpio_set_dir(EXWAIT_PIN, GPIO_IN);
    gpio_set_pulls(INT_PIN, false, false);
    gpio_set_pulls(EXWAIT_PIN, false, false);
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, true);
    sleep_ms(20);
    gpio_put(25, false);
    sleep_ms(20);
}


void device_main(void) {
    set_sys_clock_khz(260000, true);
    init_gpio();
    mount_devices();

    //pico_mgr = pico_mgr_new(IO_REPO_BASE_ADDR, pico_mgr_buff, PICO_MGR_BUFF_SIZE);
    pico_mgr = pico_mgr_new(IO_REPO_BASE_ADDR); if (!pico_mgr) return;
    fdc = fdc_new(0xd8); if (!fdc) return;
    qd = qd_new(0xf4); if (!qd) return;
    boot_rd = sramdisk_new(0xf8, 0xf9, 0xfa, boot_rd_data, BOOT_RD_SIZE); if (!boot_rd) return;
    pico_rd = pico_rd_new(0x45, pico_rd_data, PICO_RD_SIZE); if (!pico_rd) return;

    pico_mgr->iface.init(pico_mgr);
    qd->iface.init(qd);
    fdc->iface.init(fdc);
    boot_rd->iface.init(boot_rd);
    pico_rd->iface.init(pico_rd);

    mz_device_register(&pico_mgr->iface);
    mz_device_register(&qd->iface);
    mz_device_register(&fdc->iface);
    mz_device_register(&boot_rd->iface);
    mz_device_register(&pico_rd->iface);

    memcpy(boot_rd->data, firmware, sizeof(firmware));

    trap_reset_init(pio, SM_RESET);
    trap_read_init(pio, SM_READ, ADDR_BUS_BASE, RD_PIN);
    trap_write_init(pio, SM_WRITE, ADDR_BUS_BASE, WR_PIN);
    irq_set_exclusive_handler(PIO0_IRQ_0, reset_handler);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_set_irq0_source_enabled(pio, pis_interrupt0, true);

    listen_loop();
}
