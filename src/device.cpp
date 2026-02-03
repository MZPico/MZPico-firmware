#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
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

#include "i2s_audio.pio.h"

#include "ff.h"
#include "fatfs_disk.h"
#include "iniparser.h"
#include "embedded_mzf.hpp"
#ifdef USE_PICO_W
#include "cloud_fs.hpp"
#include "pico/cyw43_arch.h"
#endif

#include "hardware/regs/resets.h"
#include "hardware/structs/resets.h"
#include "hardware/resets.h"

#define SYSCLOCK 180000

// ---- PIO SM indices ----
#define SM_RESET 0
#define SM_READ  1
#define SM_WRITE 2

FDCDevice *fdc;
QDDevice *qd;
SN76489Device *sn76489 = nullptr;
volatile bool sn76489_ready = false;  // Core1 signals when sn76489 is ready

// Control pins
static const uint control_pins[] = { IORQ_PIN, RD_PIN, WR_PIN };
static const uint control_pins_count = sizeof(control_pins) / sizeof(control_pins[0]);

volatile bool shutting_down = false;

// ---- Globals ----
static PIO  pio = pio1;

void blink(uint8_t cnt) {
#ifdef USE_PICO_W
    // On Pico W, GPIO25 is used internally by the CYW43 SPI; avoid direct access.
    // Optionally could use cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, value) after init.
    (void)cnt; // no-op to prevent WiFi conflicts
#else
    for (int i = 0; i < cnt; ++i) {
        gpio_put(25, true);
        sleep_ms(200);
        gpio_put(25, false);
        sleep_ms(200);
    }
#endif
}

RAM_FUNC static void reset_handler(void) {
    pio_interrupt_clear(pio, 0);
    shutting_down = true;
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
            auto fn  = MZDeviceManager::getReadFunction(addr);

            if (fn) {
                MZDevice* dev = MZDeviceManager::getReadDevice(addr);

                if (MZDeviceManager::portNeedsExwait(addr)) set_exwait();

                fn(dev, addr, &data, 0);

                acquire_data_bus_for_writing();
                write_data_bus(data);

                if (dev->isInterrupt()) set_interrupt();
                if (MZDeviceManager::portNeedsExwait(addr)) release_exwait();
                while (!(sio_hw->gpio_in & (1u << IORQ_PIN)));
                release_data_bus();
            }
        }
        else if (!pio_sm_is_rx_fifo_empty(pio, SM_WRITE)) {
            addr = pio_sm_get(pio, SM_WRITE) >> 24;

            auto fn  = MZDeviceManager::getWriteFunction(addr);

            if (fn) {
                MZDevice* dev = MZDeviceManager::getWriteDevice(addr);

                if (MZDeviceManager::portNeedsExwait(addr)) set_exwait();
                data = read_data_bus();
                fn(dev, addr, data, 0);
                if (dev->isInterrupt()) set_interrupt();
                if (MZDeviceManager::portNeedsExwait(addr)) release_exwait();
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


void device_main1(void) {
    // Ensure this core can be safely locked out during flash operations
    multicore_lockout_victim_init();
    mount_devices();
    // Clean up temporary cloud files: purge flash:/tmp directory on startup

    dictionary *ini = nullptr;
    FILINFO fno;
    if (f_stat("sd:/mzpico.ini", &fno) == FR_OK) {
        ini = iniparser_load("sd:/mzpico.ini");
    }
    if (!ini) {
        ini = iniparser_load("flash:/mzpico.ini");
    }
    if (!ini) {
        halt();
    }
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
            
            // PSG (SN76489) only available on DELUXE board
            #ifndef BOARD_DELUXE
            if (devName == "psg") {
                continue;
            }
            #endif
            
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
            else if (devName == "psg")
                sn76489 = (SN76489Device *)dev;
        }
    }
    
    // Signal core0 that device initialization is complete and sn76489 pointer is ready
    // Use memory barrier to ensure all writes are visible to core0
    __asm volatile("" ::: "memory");
    #ifdef BOARD_DELUXE
    sn76489_ready = true;
    #endif


    #ifdef USE_PICO_W
    // Read WiFi credentials from cloud section
    const char *ssid = iniparser_getstring(ini, "cloud:wifi_ssid", "");
    const char *pass = iniparser_getstring(ini, "cloud:wifi_password", "");
    if (ssid && ssid[0] && pass && pass[0]) {
        CloudWifiConfig wifi_cfg{ssid, pass, CYW43_AUTH_WPA2_AES_PSK, 5};
        cloud_wifi_set_config(wifi_cfg);
    }
    #endif

    iniparser_freedict(ini);

    listen_loop();
}

void device_main() {
    set_sys_clock_khz(SYSCLOCK, true);
    init_gpio();


    // Initialize lockout on this core before launching the other core
    multicore_lockout_victim_init();

    multicore_launch_core1(device_main1);

    #ifdef BOARD_DELUXE
        bus_read_deluxe_init(pio, SM_READ,  ADDR_BUS_BASE, RD_PIN);
        bus_write_deluxe_init(pio, SM_WRITE, ADDR_BUS_BASE, WR_PIN);
    #else
        bus_read_frugal_init(pio, SM_READ,  ADDR_BUS_BASE, RD_PIN);
        bus_write_frugal_init(pio, SM_WRITE, ADDR_BUS_BASE, WR_PIN);
    #endif


    bus_reset_init(pio0, SM_RESET);
    irq_set_exclusive_handler(PIO0_IRQ_0, reset_handler);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_set_irq0_source_enabled(pio0, pis_interrupt0, true);

    // workaround for unstability after cold boot
    if (!watchdog_caused_reboot()) {
        busy_wait_ms(100);
        watchdog_reboot(0, 0, 0);
    }

    // Wait for core1 to finish device initialization and set sn76489 pointer
    // Use memory barrier to ensure we see the latest value
    #ifdef BOARD_DELUXE
    while (!sn76489_ready) {
        tight_loop_contents();
    }
    __asm volatile("" ::: "memory");
    #endif
    
    // Initialize SN76489 audio hardware on core0 (if device was registered on core1)
    // Audio only works on DELUXE board where GPIOs 12,13,14 are not used for data bus
    #ifdef BOARD_DELUXE
    if (sn76489 && sn76489->isEnabled()) {
        int result = sn76489->initAudioOnCore0();
        if (result != 0) {
            printf("Warning: Failed to initialize SN76489 audio on core0 (error %d)\n", result);
        }
    }
    #endif

#ifdef USE_PICO_W
    cloud_init();
#else
    // Without WiFi, core0 just processes SN76489 writes in tight loop
    // Note: sn76489 pointer is guaranteed to be valid after sn76489_ready flag
    while(1) {
        if (sn76489) {
            sn76489->processWritesFromMainLoop();
        }
        tight_loop_contents();
    }
#endif
}
