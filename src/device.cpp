#include <cstdio>
#include <cstring>
#include <vector>
#include <sstream>

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

#include "i2s_audio.hpp"

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
volatile bool audio_sources_ready = false;  // Core1 signals when audio sources are ready

// Control pins
static const uint control_pins[] = { IORQ_PIN, RD_PIN, WR_PIN };
static const uint control_pins_count = sizeof(control_pins) / sizeof(control_pins[0]);

volatile bool shutting_down = false;

// ---- Globals ----
static PIO  pio = pio1;

// Program load offsets, needed to restart the bus SMs from their entry
// points when flushing pre-boot FIFO backlog (see listen_loop)
static uint bus_read_prog_offset;
static uint bus_write_prog_offset;

#ifdef BOARD_DELUXE
// Gate-control instructions core1 injects into SM_READ via pio_sm_exec():
// the read program parks the gates at BLOCK_ALL after capturing, so the
// data transceiver is only turned toward the Z80 bus when data is
// actually about to be driven (see bus_io.pio).
static const uint16_t gate_write_data_instr =
    pio_encode_nop() | pio_encode_sideset_opt(4, 0x3);   // GATES_WRITE_DATA
static const uint16_t gate_low_addr_instr =
    pio_encode_nop() | pio_encode_sideset_opt(4, 0xE);   // GATES_READ_LOW_ADDR
#endif

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
    uint8_t low_addr = 0;
    uint8_t high_addr = 0;
    uint8_t data = 0;
    uint32_t raw_bus;

    // The bus SMs have been capturing since long before the devices were
    // configured; a SM may even be stalled mid-capture on a full FIFO.
    // Discard the stale backlog and restart both SMs from their entry
    // points so no pre-boot transaction is dispatched to live devices.
    pio_sm_set_enabled(pio, SM_READ, false);
    pio_sm_set_enabled(pio, SM_WRITE, false);
    pio_sm_clear_fifos(pio, SM_READ);
    pio_sm_clear_fifos(pio, SM_WRITE);
    pio_sm_restart(pio, SM_READ);
    pio_sm_restart(pio, SM_WRITE);
    pio_sm_exec(pio, SM_READ, pio_encode_jmp(bus_read_prog_offset));
    pio_sm_exec(pio, SM_WRITE, pio_encode_jmp(bus_write_prog_offset));
    pio_sm_set_enabled(pio, SM_READ, true);
    pio_sm_set_enabled(pio, SM_WRITE, true);

    while (true) {
        if (!pio_sm_is_rx_fifo_empty(pio, SM_READ)) {
            low_addr = pio_sm_get(pio, SM_READ) >> 24;
            if (MZDeviceManager::hasReadListeners(low_addr)) {
                // Latch the decision: set and release must agree even if the
                // listener tables were ever mutated while the handler runs
                bool useExwait = MZDeviceManager::portNeedsExwait(low_addr);
                if (useExwait) set_exwait();

                #ifdef BOARD_DELUXE
                high_addr = pio_sm_get_blocking(pio, SM_READ) >> 24;
                #endif

                bool wantsInterrupt = MZDeviceManager::handleRead(low_addr, &data, high_addr);
                #ifdef BOARD_DELUXE
                pio_sm_exec(pio, SM_READ, gate_write_data_instr);
                #endif
                acquire_data_bus_for_writing();
                write_data_bus(data);

                if (wantsInterrupt) set_interrupt();
                if (useExwait) release_exwait();
                while (!(sio_hw->gpio_in & (1u << IORQ_PIN)));
                release_data_bus();
                #ifdef BOARD_DELUXE
                // Re-arm the idle gate: normally the SM wrap already did this,
                // but if the cycle ended before the exec above, the injected
                // gate state would otherwise persist into the next capture.
                pio_sm_exec(pio, SM_READ, gate_low_addr_instr);
                #endif
            }
            #ifdef BOARD_DELUXE
            else {
                // Drain the high address word even when no device listens on
                // this port, otherwise the RX FIFO desyncs.
                pio_sm_get_blocking(pio, SM_READ);
            }
            #endif
        }
        else if (!pio_sm_is_rx_fifo_empty(pio, SM_WRITE)) {
            low_addr = pio_sm_get(pio, SM_WRITE) >> 24;
            if (MZDeviceManager::hasWriteListeners(low_addr)) {
                bool useExwait = MZDeviceManager::portNeedsExwait(low_addr);
                if (useExwait) set_exwait();
                #ifdef BOARD_DELUXE
                // The PIO captures high address and data as further FIFO words,
                // so the data byte is valid even if this loop runs late.
                high_addr = pio_sm_get_blocking(pio, SM_WRITE) >> 24;
                data = pio_sm_get_blocking(pio, SM_WRITE) >> 24;
                #else
                data = read_data_bus();
                #endif
                bool wantsInterrupt = MZDeviceManager::handleWrite(low_addr, data, high_addr);
                if (wantsInterrupt) set_interrupt();
                if (useExwait) release_exwait();
            }
            #ifdef BOARD_DELUXE
            else {
                // Drain the high address and data words even when no device
                // listens on this port, otherwise the RX FIFO desyncs.
                pio_sm_get_blocking(pio, SM_WRITE);
                pio_sm_get_blocking(pio, SM_WRITE);
            }
            #endif
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

static std::vector<uint8_t> parsePortsList(const char* s) {
    std::vector<uint8_t> out;
    if (!s || !*s) return out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // Trim spaces
        size_t start = tok.find_first_not_of(" \t\n\r");
        size_t end   = tok.find_last_not_of(" \t\n\r");
        if (start == std::string::npos) continue;
        std::string val = tok.substr(start, end - start + 1);
        // Support hex like 0xD8 or plain decimal
        unsigned int num = 0;
        if (val.size() > 2 && (val[0] == '0') && (val[1] == 'x' || val[1] == 'X')) {
            std::stringstream hx;
            hx << std::hex << val;
            hx >> num;
        } else {
            num = static_cast<unsigned int>(std::strtoul(val.c_str(), nullptr, 0));
        }
        if (num <= 0xFF) out.push_back(static_cast<uint8_t>(num));
    }
    return out;
}

// Helper to create consecutive port list from base_port and count
static std::vector<uint8_t> createConsecutivePorts(uint8_t basePort, uint8_t count) {
    std::vector<uint8_t> ports;
    for (uint8_t i = 0; i < count; ++i) {
        ports.push_back(basePort + i);
    }
    return ports;
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
            if (devName == "psg" || devName == "ctc") {
                continue;
            }
            #endif
            
            MZDevice* dev = MZDeviceManager::createDevice(devName, sectionName);
            if (!dev) continue;
            bool enabled = (bool)iniparser_getboolean(ini, (sectionName + ":enabled").c_str(), true);
            if (!enabled)
                MZDeviceManager::disableDevice(dev);

            // Get explicit port configuration from INI
            const char* read_ports_str  = iniparser_getstring(ini, (sectionName + ":read_ports").c_str(), "");
            const char* write_ports_str = iniparser_getstring(ini, (sectionName + ":write_ports").c_str(), "");
            auto read_ports  = parsePortsList(read_ports_str);
            auto write_ports = parsePortsList(write_ports_str);

            // If explicit lists not provided, try base_port shorthand
            if (read_ports.empty() && write_ports.empty()) {
                const char* base_port_str = iniparser_getstring(ini, (sectionName + ":base_port").c_str(), "");
                if (base_port_str && *base_port_str) {
                    // base_port provided: let device decide how to apply it
                    uint8_t basePort = (uint8_t)std::strtoul(base_port_str, nullptr, 0);
                    auto ports = dev->applyBasePort(basePort);
                    read_ports = ports.first;
                    write_ports = ports.second;
                } else {
                    // No config provided: use device defaults
                    read_ports = dev->getReadPorts();
                    write_ports = dev->getWritePorts();
                }
            }

            // Configure device with resolved ports
            int ret = MZDeviceManager::setPortsList(dev, read_ports, write_ports);
            if (ret)
                halt();

            if (!enabled)
                continue;
            dev->init();
            ret = dev->readConfig(ini);
            if (ret)
                halt();
            if (devName == "fdc")
                fdc = (FDCDevice *)dev;
            else if (devName == "qd")
                qd = (QDDevice *)dev;
            else if (devName == "psg")
                (void)dev;
        }
    }
    
    // Signal core0 that device initialization is complete and audio sources are ready
    // Use memory barrier to ensure all writes are visible to core0
    __asm volatile("" ::: "memory");
    #ifdef BOARD_DELUXE
    audio_sources_ready = true;
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
        bus_read_prog_offset  = bus_read_deluxe_init(pio, SM_READ,  ADDR_BUS_BASE, RD_PIN);
        bus_write_prog_offset = bus_write_deluxe_init(pio, SM_WRITE, ADDR_BUS_BASE, WR_PIN);
    #else
        bus_read_prog_offset  = bus_read_frugal_init(pio, SM_READ,  ADDR_BUS_BASE, RD_PIN);
        bus_write_prog_offset = bus_write_frugal_init(pio, SM_WRITE, ADDR_BUS_BASE, WR_PIN);
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

    // Wait for core1 to finish device initialization and register audio sources
    // Use memory barrier to ensure we see the latest value
    #ifdef BOARD_DELUXE
    while (!audio_sources_ready) {
        tight_loop_contents();
    }
    __asm volatile("" ::: "memory");
    #endif
    
    // Initialize shared I2S audio hardware on core0 (if audio sources were registered)
    // Audio only works on DELUXE board where GPIOs 12,13,14 are not used for data bus
    #ifdef BOARD_DELUXE
    if (i2s_audio_has_sources()) {
        int result = i2s_audio_init_on_core0();
        if (result != 0) {
            printf("Warning: Failed to initialize I2S audio on core0 (error %d)\n", result);
        }
    }
    #endif

#ifdef USE_PICO_W
    cloud_init();
#else
    // Without WiFi, core0 just processes audio sources in tight loop
    while(1) {
        i2s_audio_poll();
        tight_loop_contents();
    }
#endif
}
