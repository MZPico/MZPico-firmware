#pragma once

#include <cstdint>
#include "mz_devices.hpp"
#include "common.hpp"
#include "pico_rd_addr.hpp"
#include "cow_disk.hpp"

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
    std::vector<uint8_t> getReadPorts() const override;
    std::vector<uint8_t> getWritePorts() const override;
    static std::string getDevType() { return PICO_RD_ID; }
    int readConfig(dictionary *ini) override;
    int flush() override;
    void setDriveContent(std::string content, bool in_ram);

    RAM_FUNC static int writeControl(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int readControl(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writeAddr2(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int readAddr2(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writeAddr1(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int readAddr1(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writeAddr0(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int readAddr0(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writeAddrs(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeAddri(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);

    // Contents persist, like real RAM (mode=cow: the pool survives every
    // Z80 reset); the position returns to 0
    void softReset() override;

private:
    uint8_t* data;
    uint32_t size;
    bool readOnly;
    PicoRdAddr a_;             // the position register (pico_rd_addr.hpp)
    std::unique_ptr<ByteSource> bs;
    CowDisk* cow_ = nullptr;   // mode=cow: the card's flash image (XIP) + SRAM pool
};

