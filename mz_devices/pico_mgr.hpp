#pragma once

#include <stdint.h>
#include <string.h>     // memcpy used in templates
#include "mz_devices.hpp"
#include "common.hpp"
#include "bus.hpp"

constexpr uint8_t PICO_MGR_READ_PORT_COUNT = 4;
constexpr uint8_t PICO_MGR_WRITE_PORT_COUNT = 5;
constexpr uint8_t PICO_MGR_COMMAND_PORT_INDEX = 0;
constexpr uint8_t PICO_MGR_DATA_PORT_INDEX = 1;
constexpr uint8_t PICO_MGR_ADDR0_PORT_INDEX = 2;
constexpr uint8_t PICO_MGR_ADDR1_PORT_INDEX = 3;
constexpr uint8_t PICO_MGR_RESET_PORT_INDEX = 4;

constexpr uint8_t PICO_MGR_DEFAULT_BASE_PORT = 0x40;
constexpr bool PICO_MGR_EXWAIT = true;
constexpr const char PICO_MGR_ID[] = "pico_mgr";

using PackFn = void (*)(const void* record, uint8_t* dst);            // record -> packed bytes
using UnpackFn = void (*)(const uint8_t* src, void* recordOut);         // packed bytes -> record


class PicoMgr final : public MZDevice {
public:
    PicoMgr();
    int  init() override;
    inline void setContent(uint16_t recordSize, PackFn pack, UnpackFn unpack) { recordSize_ = recordSize; pack_ = pack; unpack_ = unpack; };
    void setString(const char *str);
    void addRaw(const uint8_t *dt, uint16_t sz);
    uint16_t getRaw(uint8_t *dest);
    uint8_t *allocateRaw(uint16_t sz);
    int  isInterrupt() override { return 0; }
    bool needsExwait() const override { return PICO_MGR_EXWAIT; }
    uint8_t getDefaultBasePort() const override { return PICO_MGR_DEFAULT_BASE_PORT; }
    static std::string getDevType() { return PICO_MGR_ID; }
    int readConfig(dictionary *ini) override;
    int flush() override { return 0; }


    static int writeControl(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int readControl(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int readData(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int writeAddr0(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int writeAddr1(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    static int readAddr0(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int readAddr1(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    static int writeReset(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);

    bool addRecord(const void* record);
    bool getRecord(uint16_t index, void* outRecord) const;
    inline uint16_t getNumberOfRecords() const { return getLength() / recordSize_; }
    inline void resetContent() { setLength(0); }

private:
    // Buffer and mappings
    uint8_t data[PICO_MGR_BUFF_SIZE];
    uint8_t  response_command;
    uint16_t idx;

    inline uint16_t getLength() const {
        return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
    }

    inline bool setLength(uint16_t len) {
        if (len > payloadCapacity()) {
            return false; // reject invalid length
        }
        data[0] = static_cast<uint8_t>(len & 0xFF);
        data[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
        return true;
    }

    inline uint16_t payloadCapacity() const {
        return PICO_MGR_BUFF_SIZE - 2; // 2 bytes reserved for length
    }

    inline uint16_t remainingCapacity() const {
        uint16_t length = getLength();
        if (length > payloadCapacity()) {
            return 0; // prevent underflow
        }
        return payloadCapacity() - length;
    }
    
    uint16_t recordSize_;
    PackFn   pack_;
    UnpackFn unpack_;

};
