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
    uint8_t getDefaultBasePort() const override { return RAMDISK_DEFAULT_BASE_PORT; }
    int readConfig(dictionary *ini) override;
    int flush() override;
    int setDriveContent(const std::string &content, bool in_ram);
    static std::string getDevType() { return RAMDISK_ID; }

    RAM_FUNC static int readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int writePageAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeAddress(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);

private:
    uint8_t* data;
    bool readOnly;
    uint32_t size;
    int loadMzf(const uint8_t* src, size_t src_size, bool in_ram);
    std::unique_ptr<ByteSource> bs;
};
