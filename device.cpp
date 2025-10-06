// device.cpp  — C++ port of device.c, adapted to MZDeviceManager + FDCDevice
#include <cstdio>
#include <cstring>

// Pico SDK / hardware (C++ compatible)
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"
#include "hardware/vreg.h"

// Your project headers (unchanged)
#include "common.hpp"
#include "bus.hpp"
#include "file.hpp"
#include "trap_reset.pio.h"
#include "device.hpp"
#include "config.hpp"
#include "file_source.hpp"
#include "cached_source.hpp"
#include "qd_dir_source.hpp"

// New C++ device framework + the FDC device
#include "mz_devices.hpp"
#include "fdc.hpp"      // C++ port of FDC

#include "ff.h"
#include "fatfs_disk.h"
#include "iniparser.h"
#include "embedded_mzf.hpp"

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
static PIO  pio = pio0;

// Optional small status LED helper
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
    watchdog_reboot(0, 0, 0);
}

RAM_FUNC static void listen_loop(void) {
    uint8_t addr = 0;
    uint8_t data = 0;

    while (!(sio_hw->gpio_in & (1u << IORQ_PIN)));
    while (true) {
        while (sio_hw->gpio_in & (1u << IORQ_PIN));

        const uint32_t pins = sio_hw->gpio_in;

        if (!(pins & (1u << RD_PIN))) {
            // IN: CPU reads from port
            //set_exwait();
            addr = read_address_bus();
            MZDevice* dev = MZDeviceManager::getReadDevice(addr);
            auto fn  = MZDeviceManager::getReadFunction(addr);

            if (fn && dev) {
                //set_exwait();
                if (dev->needsExwait()) set_exwait();

                fn(dev, addr, &data, 0);

                acquire_data_bus_for_writing();
                write_data_bus(data);

                if (dev->needsExwait()) release_exwait();
                if (dev->isInterrupt()) set_interrupt();
                //release_exwait();
            }
            //release_exwait();
        }
        else if (!(pins & (1u << WR_PIN))) {
            //set_exwait();
            read_address_data_bus(&addr, &data);

            MZDevice* dev = MZDeviceManager::getWriteDevice(addr);
            auto fn  = MZDeviceManager::getWriteFunction(addr);

            if (fn && dev) {
                //set_exwait();
                if (dev->needsExwait()) set_exwait();

                fn(dev, addr, data, 0);

                if (dev->needsExwait()) release_exwait();
                if (dev->isInterrupt()) set_interrupt();
                //release_exwait();
            }
            //release_exwait();
        }

        // Wait for IORQ to go HIGH again (cycle end) and tri-state data bus
        while (!(sio_hw->gpio_in & (1u << IORQ_PIN)));
        release_data_bus();
    }
}

static void init_gpio(void) {
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
    /*
    gpio_put(25, true);
    sleep_ms(20);
    gpio_put(25, false);
    sleep_ms(20);
    */
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

// Entry point used by your startup (unchanged name)
void device_main(void) {
    set_sys_clock_khz(200000, true);
    init_gpio();
    mount_devices();
/*
    FIL fx;

    uint8_t b[512];
    uint8_t bt;
    uint32_t rd;
    unsigned int rd1;
    blink(2);
    std::unique_ptr<ByteSource> bs;
    FileSource *fs;
    f_open(&fx, "flash:/file.mzq", FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
    ByteSourceFactory::from_file("flash:/QDisk-5Z001_74.mzq", 0, 128, bs);
    fs = static_cast<FileSource *>(bs.get());

    uint32_t sz = fs->getSize();
    for (int i=0; i<sz; i++) {
        bs->getByte(bt);
        f_write(&fx, &bt, 1, &rd1);
    };
    f_close(&fx);
    return;
*/
    set_exwait();
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

                config[keyName] = value;
            }

            delete[] keys;
            picoConfig[sectionName] = config;
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

    release_exwait();
    trap_reset_init(pio, SM_RESET);
    //trap_read_init (pio, SM_READ,  ADDR_BUS_BASE, RD_PIN);
    //trap_write_init(pio, SM_WRITE, ADDR_BUS_BASE, WR_PIN);

    irq_set_exclusive_handler(PIO0_IRQ_0, reset_handler);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_set_irq0_source_enabled(pio, pis_interrupt0, true);

    // Run I/O server
    listen_loop();
}
