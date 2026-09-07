#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <utility>
#include "common.hpp"
#include "iniparser.h"
#define REGISTER_MZ_DEVICE(CLASS)
constexpr uint8_t MAX_DEVICE_PORTS = 16;
constexpr int E_DEVICE_NO_MEMORY = 250;
class MZDevice {
public:
    struct ReadPortMapping { uint8_t port; int (*fn)(MZDevice*, uint8_t, uint8_t*, uint8_t); };
    struct WritePortMapping { uint8_t port; int (*fn)(MZDevice*, uint8_t, uint8_t, uint8_t); };
    virtual ~MZDevice() {}
    virtual int init() = 0;
    virtual int isInterrupt() = 0;
    virtual bool needsExwait() const = 0;
    virtual std::vector<uint8_t> getReadPorts() const = 0;
    virtual std::vector<uint8_t> getWritePorts() const = 0;
    virtual std::pair<std::vector<uint8_t>, std::vector<uint8_t>> applyBasePort(uint8_t) const { return {getReadPorts(), getWritePorts()}; }
    virtual int readConfig(dictionary*) = 0;
    virtual int flush() = 0;
    virtual void softReset() {}
    virtual bool supportedOnBoard() const { return true; }
    const ReadPortMapping* getReadMappings() const { return readMappings; }
    const WritePortMapping* getWriteMappings() const { return writeMappings; }
    uint8_t getReadCount() const { return readPortCount; }
    uint8_t getWriteCount() const { return writePortCount; }
    std::string getDevID() const { return devID; }
    void initializePortMappings(const std::vector<uint8_t>& r, const std::vector<uint8_t>& w) {
        readPortCount = r.size(); for (uint8_t i = 0; i < readPortCount; ++i) readMappings[i].port = r[i];
        writePortCount = w.size(); for (uint8_t i = 0; i < writePortCount; ++i) writeMappings[i].port = w[i];
    }
protected:
    ReadPortMapping readMappings[MAX_DEVICE_PORTS]{};
    WritePortMapping writeMappings[MAX_DEVICE_PORTS]{};
    uint8_t readPortCount = 0, writePortCount = 0;
    std::string devID = "unicard";
    bool enabled = true;
};
