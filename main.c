#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "mz800pico_trap.pio.h"

// --- Pin Definitions ---
#define ADDR_BUS_BASE    0
#define ADDR_BUS_COUNT   8

#define DATA_BUS_BASE    8
#define DATA_BUS_COUNT   8

#define IORQ_PIN         16
#define RD_PIN           17
#define WR_PIN           18

// Control pins array
static const uint control_pins[] = { IORQ_PIN, RD_PIN, WR_PIN };
static const uint control_pins_count = sizeof(control_pins) / sizeof(control_pins[0]);

// --- Constants ---
#define IO_RESET_ADDR    0xE9
#define IO_READ_ADDR     0xEA
#define IO_WRITE_ADDR    0xEB

#define RD_SIZE          65536u
#define DATA_BUS_MASK    (((1u << DATA_BUS_COUNT) - 1) << DATA_BUS_BASE)

// --- Macros ---
#define READ_PINS(pins, base, count) (((pins) >> (base)) & ((1u << (count)) - 1))

// --- Globals ---
PIO pio = pio0;
uint sm = 0;

static uint8_t rd[RD_SIZE] = {
    // --- Header (9 bytes) ---
    0x15, 0x00,       // Body size: 21 bytes
    0x00, 0x12,       // Load address: 0x1200
    0x00, 0x12,       // Entry point: 0x1200
    0x9A, 0x00,       // Body CRC: 154 ones
    0x2B,             // Header CRC: 43 ones in header[0–7]

    // --- Program Body (21 bytes) ---
    0x11, 0x05, 0x12,             // LD DE, message
    0xCD, 0x15, 0x00,             // CALL 0x0015
    0xC3, 0x04, 0x12,             // JP hang
    0x48, 0x45, 0x4C, 0x4C, 0x4F, // "HELLO"
    0x20,                         // space
    0x57, 0x4F, 0x52, 0x4C, 0x44, // "WORLD"
    0x0D                          // CR terminator
};

static uint32_t rd_pnt = 0;

// --- Inline Functions ---
static inline void rd_pnt_increment(void) {
    rd_pnt++;
    if (rd_pnt >= RD_SIZE) {
        rd_pnt = 0;
    }
}

static inline uint8_t read_address_bus(void) {
    return READ_PINS(gpio_get_all(), ADDR_BUS_BASE, ADDR_BUS_COUNT);
}

static inline uint8_t read_data_bus(void) {
    return READ_PINS(gpio_get_all(), DATA_BUS_BASE, DATA_BUS_COUNT);
}

static inline void read_address_data_bus(uint8_t *addr_out, uint8_t *data_out) {
    uint32_t pins = gpio_get_all();
    *addr_out = READ_PINS(pins, ADDR_BUS_BASE, ADDR_BUS_COUNT);
    *data_out = READ_PINS(pins, DATA_BUS_BASE, DATA_BUS_COUNT);
}

static inline void write_data_bus(uint8_t value) {
    gpio_put_masked(DATA_BUS_MASK, (uint32_t)value << DATA_BUS_BASE);
}

static inline void acquire_data_bus_for_writing(void) {
    gpio_set_dir_masked(DATA_BUS_MASK, DATA_BUS_MASK);  // Set data pins as output
}

static inline void release_data_bus(void) {
    gpio_set_dir_masked(DATA_BUS_MASK, 0);              // Set data pins as input (Hi-Z)
}

static inline uint8_t rd_read(void) {
    uint8_t val = rd[rd_pnt];
    rd_pnt_increment();
    return val;
}

static inline void rd_write(uint8_t value) {
    rd[rd_pnt] = value;
    rd_pnt_increment();
}

// --- IRQ Handler ---
void pio0_irq0_handler(void) {
    if (pio_interrupt_get(pio, 0)) {
        pio_interrupt_clear(pio, 0);

        uint8_t addr = read_address_bus();
        if (addr == IO_RESET_ADDR) {
            rd_pnt = 0;
        } else if (addr == IO_READ_ADDR) {
            acquire_data_bus_for_writing();
            write_data_bus(rd_read());
        }
    }

    if (pio_interrupt_get(pio, 1)) {
        pio_interrupt_clear(pio, 1);

        uint8_t addr, data;
        read_address_data_bus(&addr, &data);
        if (addr == IO_WRITE_ADDR) {
            sleep_us(1);  // Wait for valid data
            rd_write(data);
        }
    }

    if (pio_interrupt_get(pio, 2)) {
        pio_interrupt_clear(pio, 2);
        release_data_bus();
    }
}

// --- GPIO Init ---
void init_gpio(void) {
    for (int i = 0; i < ADDR_BUS_COUNT; ++i) {
        gpio_init(ADDR_BUS_BASE + i);
        gpio_set_dir(ADDR_BUS_BASE + i, GPIO_IN);
    }

    for (int i = 0; i < DATA_BUS_COUNT; ++i) {
        gpio_init(DATA_BUS_BASE + i);
        gpio_set_dir(DATA_BUS_BASE + i, GPIO_IN);
    }

    //for (size_t i = 0; i < control_pins_count; ++i) {
    for (size_t i = 0; i < 3; ++i) {
        gpio_init(control_pins[i]);
        gpio_set_dir(control_pins[i], GPIO_IN);
        gpio_pull_up(control_pins[i]);
    }
}

// --- Main Entry ---
int main(void) {
    stdio_init_all();
    init_gpio();

    mz800pico_trap_init(pio, sm, RD_PIN, WR_PIN);

    irq_set_exclusive_handler(PIO0_IRQ_0, pio0_irq0_handler);
    irq_set_enabled(PIO0_IRQ_0, true);

    pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
    pio_set_irq0_source_enabled(pio, pis_interrupt1, true);
    pio_set_irq0_source_enabled(pio, pis_interrupt2, true);

    printf("mz800pico trap ready.\n");

    while (1) {
        tight_loop_contents();
    }
}

