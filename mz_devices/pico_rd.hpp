#pragma once

#include <cstdint>
#include "mz_devices.hpp"

constexpr uint8_t PICO_RD_READ_PORT_COUNT = 5;
constexpr uint8_t PICO_RD_WRITE_PORT_COUNT = 7;

constexpr uint8_t PICO_RD_CONTROL_PORT_INDEX = 0;
constexpr uint8_t PICO_RD_DATA_PORT_INDEX = 1;
constexpr uint8_t PICO_RD_ADDR2_PORT_INDEX = 2;
constexpr uint8_t PICO_RD_ADDR1_PORT_INDEX = 3;
constexpr uint8_t PICO_RD_ADDR0_PORT_INDEX = 4;
constexpr uint8_t PICO_RD_ADDRS_PORT_INDEX = 5;
constexpr uint8_t PICO_RD_ADDRI_PORT_INDEX = 6;

constexpr uint8_t PICO_RD_DEFAULT_BASE_PORT = 0x45;
constexpr const char PICO_RD_ID[] = "pico_rd";
constexpr bool PICO_RD_EXWAIT = true;
constexpr uint32_t PICO_RD_DEFAULT_SIZE = 65536;

class PicoRD final : public MZDevice {
public:
    PicoRD();

    int init() override;
    int isInterrupt() override { return 0; }
    bool needsExwait() const override { return PICO_RD_EXWAIT; }
    uint8_t getDefaultBasePort() const override { return PICO_RD_DEFAULT_BASE_PORT; }
    static std::string getDevType() { return PICO_RD_ID; }
    int readConfig(dictionary *ini) override;
    void setDriveContent(std::string content, bool in_ram);

    static int writeControl(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int readControl(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int writeAddr2(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int readAddr2(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int writeAddr1(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int readAddr1(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int writeAddr0(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int readAddr0(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int writeAddrs(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int writeAddri(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);

private:
    uint8_t* data;
    uint32_t size;
    uint8_t addr_idx;
    bool readOnly;
    std::unique_ptr<ByteSource> bs;
};

