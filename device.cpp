#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"
#include "hardware/vreg.h"

#include "common.hpp"
#include "bus.hpp"
#include "file.hpp"
#include "bus_io.pio.h"
#include "device.hpp"
#include "config.hpp"
#include "file_source.hpp"
#include "cached_source.hpp"
#include "qd_dir_source.hpp"

#include "mz_devices.hpp"
#include "fdc.hpp"

#include "ff.h"
#include "fatfs_disk.h"
#include "iniparser.h"
#include "embedded_mzf.hpp"

#define SYSCLOCK 180000

// ---- PIO SM indices ----
#define SM_RESET 0
#define SM_READ  1
#define SM_WRITE 2

FDCDevice *fdc;
QDDevice *qd;

// Control pins
static const uint control_pins[] = { IORQ_PIN, RD_PIN, WR_PIN };
static const uint control_pins_count = sizeof(control_pins) / sizeof(control_pins[0]);

// ---- Globals ----
static PIO  pio = pio1;

void blink(uint8_t cnt) {
    for (int i = 0; i < cnt; ++i) {
        gpio_put(25, true);
        sleep_ms(200);
        gpio_put(25, false);
        sleep_ms(200);
    }
}

RAM_FUNC static void reset_handler(void) {
    pio_interrupt_clear(pio, 0);
    MZDeviceManager::flushAll();
    watchdog_reboot(0, 0, 0);
}

RAM_FUNC static void listen_loop(void) {
    uint8_t addr = 0;
    uint8_t data = 0;
    uint32_t raw_bus;

    while (true) {
        if (!pio_sm_is_rx_fifo_empty(pio, SM_READ)) {
            addr = pio_sm_get(pio, SM_READ) >> 24;
            MZDevice* dev = MZDeviceManager::getReadDevice(addr);
            auto fn  = MZDeviceManager::getReadFunction(addr);

            if (fn && dev) {
                if (dev->needsExwait()) set_exwait();

                fn(dev, addr, &data, 0);

                acquire_data_bus_for_writing();
                write_data_bus(data);

                if (dev->needsExwait()) release_exwait();
                if (dev->isInterrupt()) set_interrupt();
            }
        }
        else if (!pio_sm_is_rx_fifo_empty(pio, SM_WRITE)) {
            addr = pio_sm_get(pio, SM_WRITE) >> 24;

            MZDevice* dev = MZDeviceManager::getWriteDevice(addr);
            auto fn  = MZDeviceManager::getWriteFunction(addr);

            if (fn && dev) {
                if (dev->needsExwait()) set_exwait();
                data = read_data_bus();
                fn(dev, addr, data, 0);
                if (dev->needsExwait()) release_exwait();
                if (dev->isInterrupt()) set_interrupt();
            }
        }
    }
}

static void init_gpio(void) {
    #if (ADDR_BUS_BASE != DATA_BUS_BASE)
        for (int i = 0; i < ADDR_BUS_COUNT; ++i) {
            gpio_init(ADDR_BUS_BASE + i);
            gpio_set_dir(ADDR_BUS_BASE + i, GPIO_IN);
        }
    #endif

    for (int i = 0; i < DATA_BUS_COUNT; ++i) {
        gpio_init(DATA_BUS_BASE + i);
        gpio_set_dir(DATA_BUS_BASE + i, GPIO_IN);
        gpio_set_slew_rate(DATA_BUS_BASE + i, GPIO_SLEW_RATE_FAST);
    }

    for (size_t i = 0; i < control_pins_count; ++i) {
        gpio_init(control_pins[i]);
        gpio_set_dir(control_pins[i], GPIO_IN);
        gpio_pull_up(control_pins[i]);
    }

    #ifdef BOARD_DELUXE
        for (size_t i = 0; i < 4; ++i) {
            gpio_init(EN0_PIN + i);
            gpio_set_dir(EN0_PIN + i, GPIO_OUT);
            gpio_set_slew_rate(EN0_PIN + i, GPIO_SLEW_RATE_FAST);
            gpio_set_function(EN0_PIN + i, GPIO_FUNC_PIO1);
        }
    #endif

    gpio_init(INT_PIN);
    gpio_init(EXWAIT_PIN);
    gpio_init(RESET_PIN);
    gpio_set_dir(INT_PIN, GPIO_IN);
    gpio_set_dir(EXWAIT_PIN, GPIO_IN);
    gpio_set_dir(RESET_PIN, GPIO_IN);
    gpio_set_pulls(INT_PIN, false, false);
    gpio_set_pulls(EXWAIT_PIN, false, false);
    gpio_set_slew_rate(EXWAIT_PIN, GPIO_SLEW_RATE_FAST);

}

std::string stripTrailingNumbers(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(0, end);
}

void halt(void) {
    while (true)
        tight_loop_contents();
}

void device_main(void) {
    set_sys_clock_khz(SYSCLOCK, true);
    init_gpio();
    mount_devices();

    dictionary *ini = iniparser_load("flash:/mzpico.ini");
    int sectionNumber = iniparser_getnsec(ini);

    for (int i=0; i < sectionNumber; i++) {
        std::string sectionName = iniparser_getsecname(ini, i);
        if (sectionName == "menu" || sectionName == "explorer") {
            SectionConfig config;

            // Get number of keys in this section
            int keyCount = iniparser_getsecnkeys(ini, sectionName.c_str());
            if (keyCount <= 0)
                continue;

            // Allocate array to hold key pointers
            const char **keys = new const char*[keyCount];

            // Fill keys[]; function returns number of keys found
            iniparser_getseckeys(ini, sectionName.c_str(), keys);

            for (int k = 0; k < keyCount; ++k) {
                std::string fullKey = keys[k] ? keys[k] : "";
                std::string keyName = fullKey;

                // Remove "section:" prefix
                std::string prefix = sectionName + ":";
                if (keyName.rfind(prefix, 0) == 0)
                    keyName = keyName.substr(prefix.length());

                const char *value_cstr = iniparser_getstring(ini, fullKey.c_str(), "");
                std::string value = value_cstr ? value_cstr : "";

                config.emplace_back(keyName, value);
            }

            delete[] keys;
            picoConfig.emplace_back(sectionName, std::move(config));
        } else { // devices
            std::string devName = stripTrailingNumbers(sectionName);
            MZDevice* dev = MZDeviceManager::createDevice(devName, sectionName);
            if (!dev) continue;
            bool enabled = (bool)iniparser_getboolean(ini, (sectionName + ":enabled").c_str(), true);
            if (!enabled)
                MZDeviceManager::disableDevice(dev);
            uint8_t basePort = (uint8_t)iniparser_getint(ini, (sectionName + ":base_port").c_str(), dev->getDefaultBasePort());
            MZDeviceManager::setBasePort(dev, basePort);
            if (!enabled)
                continue;
            dev->init();
            int ret = dev->readConfig(ini);
            if (ret)
                halt();
            if (devName == "fdc")
                fdc = (FDCDevice *)dev;
            else if (devName == "qd")
                qd = (QDDevice *)dev;
        }
    }

    iniparser_freedict(ini);

    #ifdef BOARD_DELUXE
        bus_read_deluxe_init(pio, SM_READ,  ADDR_BUS_BASE, RD_PIN);
        bus_write_deluxe_init(pio, SM_WRITE, ADDR_BUS_BASE, WR_PIN);
    #else
        bus_read_frugal_init(pio, SM_READ,  ADDR_BUS_BASE, RD_PIN);
        bus_write_frugal_init(pio, SM_WRITE, ADDR_BUS_BASE, WR_PIN);
    #endif



    bus_reset_init(pio, SM_RESET);
    irq_set_exclusive_handler(PIO1_IRQ_0, reset_handler);
    irq_set_enabled(PIO1_IRQ_0, true);
    pio_set_irq0_source_enabled(pio, pis_interrupt0, true);

    // workaround for unstability after cold boot
    if (!watchdog_caused_reboot()) {
        busy_wait_ms(30);
        watchdog_reboot(0, 0, 0);
    }

    listen_loop();
}
