#pragma once

#include <stdint.h>
#include "byte_source.hpp"
#include "mz_devices.hpp"
#include "common.hpp"

constexpr uint16_t SRAM_DEFAULT_SIZE = 32768;
constexpr uint8_t SRAM_DEFAULT_BASE_PORT = 0xf8;
constexpr const char SRAM_ID[] = "sramdisk";
constexpr bool SRAM_EXWAIT = true;

class SRamDisk final : public MZDevice {
public:
    SRamDisk();
    int init() override;
    int isInterrupt() override { return 0; };
    bool needsExwait() const override { return SRAM_EXWAIT; }
    uint8_t getDefaultBasePort() const override { return SRAM_DEFAULT_BASE_PORT; }
    int readConfig(dictionary *ini) override;
    int setDriveContent(const std::string &content, bool in_ram);
    static std::string getDevType() { return SRAM_ID; }

    RAM_FUNC static int readPort(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writePort(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int resetPort(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);

private:
    uint8_t* data;
    int firstByte;
    bool allowBoot;
    bool readOnly;
    uint16_t size;
    int loadMzf(const uint8_t* src, size_t src_size, bool in_ram);
    std::unique_ptr<ByteSource> bs;
};
