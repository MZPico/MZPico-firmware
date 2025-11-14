/*
 * GPL notice preserved from original C implementation:
 *   (c) 2009 Michal Hucik <http://www.ordoz.com>
 *   (c) 2012 Bohumil Novacek <http://dzi.n.cz/8bit/>
 * Ported to C++ and integrated with MZDevice by Martin Matyas, 2025.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include "ff.h"
#include "mz_devices.hpp"
#include "bus.hpp"
#include "common.hpp"
#include "byte_source.hpp"

constexpr bool FDC_EXWAIT = true;
constexpr uint8_t FDC_DEFAULT_BASE_PORT = 0xd8;
constexpr const char FDC_ID[] = "fdc";

#define FDC_WRITE_PORTS 8
#define FDC_READ_PORTS 4
#define FDC_NUM_DRIVES 4
#define FILENAME_LENGTH 32

class FDCDevice final : public MZDevice {
public:
    explicit FDCDevice();
    ~FDCDevice();
    int init() override;
    int isInterrupt() override;
    RAM_FUNC bool needsExwait() const override { return FDC_EXWAIT; }
    uint8_t getDefaultBasePort() const override { return FDC_DEFAULT_BASE_PORT; }
    int readConfig(dictionary *ini) override;
    int flush() override;
    static ALWAYS_INLINE std::string getDevType() { return FDC_ID; }
    int setDriveContent(uint8_t drive_id, const char* file_path);

private:
    static int ReadThunk(MZDevice* dev, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int WriteThunk(MZDevice* dev, uint8_t port, uint8_t  dt, uint8_t high_addr);
    int32_t getTrackOffset(uint8_t drive_id, uint8_t track, uint8_t side);
    uint8_t seekToSector(uint8_t drive_id, uint8_t sector);
    uint8_t setTrack();
    int fdcRead(uint8_t port, uint8_t* dt, uint8_t high_addr);
    int fdcWrite(uint8_t port, uint8_t  dt, uint8_t high_addr);

private:
    struct FDDrive {
        std::unique_ptr<ByteSource> bs;
        uint8_t TRACK{0};
        uint8_t SECTOR{0};
        uint8_t SIDE{0};
        int32_t track_offset{0};
        int16_t sector_size{0};
    };
    uint8_t regSTATUS{0};
    uint8_t regDATA{0};
    uint8_t regTRACK{0};
    uint8_t regSECTOR{0};
    uint8_t SIDE{0};
    uint8_t buffer[0x100]{};
    uint16_t buffer_pos{0};
    uint8_t COMMAND{0};
    uint8_t MOTOR{0};
    uint8_t DENSITY{0};
    uint8_t EINT{0};
    uint16_t DATA_COUNTER{0};
    FDDrive drive[FDC_NUM_DRIVES]{};
    uint8_t MULTIBLOCK_RW{0};
    uint8_t STATUS_SCRIPT{0};
    uint8_t waitForInt{0};
    uint8_t write_track_stage{0};
    uint16_t write_track_counter{0};
    uint8_t reading_status_counter{0};
    int fd0disabled{-1};
};

