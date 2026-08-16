#pragma once

#include <stdint.h>
#include "byte_source.hpp"
#include "mz_devices.hpp"
#include "common.hpp"

constexpr uint32_t RAMDISK_DEFAULT_SIZE = 65536;
constexpr uint8_t RAMDISK_DEFAULT_BASE_PORT = 0xe9;
constexpr const char RAMDISK_ID[] = "ramdisk";
constexpr bool RAMDISK_EXWAIT = true;

class RamDisk final : public MZDevice {
public:
    RamDisk();
    int init() override;
    int isInterrupt() override { return 0; };
    bool needsExwait() const override { return RAMDISK_EXWAIT; }
    std::vector<uint8_t> getReadPorts() const override;
    std::vector<uint8_t> getWritePorts() const override;
    std::pair<std::vector<uint8_t>, std::vector<uint8_t>> applyBasePort(uint8_t basePort) const override;
    int readConfig(dictionary *ini) override;
    int flush() override;
    static std::string getDevType() { return RAMDISK_ID; }

    RAM_FUNC static int readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int resetCounter(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writePageAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int readHighAddr(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);

    void softReset() override { pos_ = 0; } // contents persist, like real RAM

    // Deluxe only: 16-bit intra-page positioning (what real MZ-1R18
    // software uses) needs the Deluxe write capture - on Frugal the
    // device would silently corrupt random-access writes, so it is
    // skipped entirely. NB: the frugal bus-validation instruments
    // (rdtest/rdrtest/rdstress) target these ports; a frugal bus
    // campaign needs a diagnostic build with this returning true.
    bool supportedOnBoard() const override {
        #ifdef BOARD_DELUXE
        return true;
        #else
        return false;
        #endif
    }

private:
    uint8_t* data;
    bool readOnly;
    uint32_t size;
    uint32_t pos_;
    std::unique_ptr<ByteSource> bs;
};
