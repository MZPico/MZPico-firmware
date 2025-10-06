#pragma once

#include <stdint.h>
#include <string>
#include <functional>
#include <memory>
#include <map>

#include "iniparser.h"

#define REGISTER_MZ_DEVICE(CLASS) \
    MZDevice* create_##CLASS() { return new CLASS(); } \
    namespace { \
        struct AutoRegister_##CLASS { \
            AutoRegister_##CLASS() { \
                MZDeviceManager::registerClass(CLASS::getDevType(), create_##CLASS); \
            } \
        }; \
        static AutoRegister_##CLASS _autoRegister_##CLASS; \
    }


constexpr uint8_t MAX_MZ_DEVICES = 64;
constexpr uint16_t MAX_PORTS = 256;
constexpr uint8_t MAX_DEVICE_PORTS = 8;

constexpr uint8_t E_PORT_ALLOCATED = 255;
constexpr uint8_t E_PORT_NOT_AVAILABLE = 254;
constexpr uint8_t E_MAX_DEVICES = 253;
constexpr uint8_t E_DEVICE_ALREADY_REGISTERED = 252;
constexpr uint8_t E_DEVICE_NOT_REGISTERED = 251;

class MZDevice {
public:
    struct ReadPortMapping {
        uint8_t port;
        int (*fn)(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    };

    struct WritePortMapping {
        uint8_t port;
        int (*fn)(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    };

    virtual int init() = 0;
    virtual int isInterrupt() = 0;
    virtual bool needsExwait() const = 0;
    virtual uint8_t getDefaultBasePort() const = 0;
    virtual int readConfig(dictionary *ini) = 0;

    const ReadPortMapping* getReadMappings() const { return readMappings; }
    const WritePortMapping* getWriteMappings() const { return writeMappings; }

    uint8_t getReadCount() const { return readPortCount; }
    uint8_t getWriteCount() const { return writePortCount; }

    std::string getDevID() const { return devID; }
    void setDevID(const std::string& id) { devID = id; }
    bool isEnabled() const { return enabled; }
    void Enable() { enabled = true; }
    void Disable() { enabled = false; }
    void setBasePort(uint8_t newBasePort) { basePort = newBasePort; }
    uint8_t getBasePort() { return basePort; }
    void setPorts();

protected:
    ReadPortMapping readMappings[MAX_DEVICE_PORTS];
    WritePortMapping writeMappings[MAX_DEVICE_PORTS];
    static inline uint8_t readPortCount = 0;
    static inline uint8_t writePortCount = 0;
    uint8_t basePort;
    std::string devID;
    static inline std::string devType="none";
    bool enabled = true;
};

class MZDeviceManager {
public:
    using Creator = std::function<MZDevice*()>;

    static void registerClass(const std::string& name, Creator creator) {
        getMap()[name] = std::move(creator);
    }

    static MZDevice* createDevice(const std::string& devType, const std::string& id);
    static int disableDevice(MZDevice* dev);
    static int enableDevice(MZDevice* dev);
    static int setBasePort(MZDevice* dev, uint8_t basePort);

    static inline MZDevice* getReadDevice(uint8_t port) {
        return readPortMap[port];
    }

    static inline MZDevice* getWriteDevice(uint8_t port) {
        return writePortMap[port];
    }

    static inline auto getReadFunction(uint8_t port) {
        return readFunctions[port];
    }

    static inline auto getWriteFunction(uint8_t port) {
        return writeFunctions[port];
    }

private:
    static std::map<std::string, Creator>& getMap() { static std::map<std::string, Creator> creators; return creators; }
    static inline MZDevice* devices[MAX_MZ_DEVICES] = {nullptr};
    static inline MZDevice* readPortMap[MAX_PORTS] = {nullptr};
    static inline MZDevice* writePortMap[MAX_PORTS] = {nullptr};
    static inline uint8_t deviceCount = 0;

    static inline int (*readFunctions[MAX_PORTS])(MZDevice*, uint8_t, uint8_t*, uint8_t) = {nullptr};
    static inline int (*writeFunctions[MAX_PORTS])(MZDevice*, uint8_t, uint8_t, uint8_t) = {nullptr};
    static inline bool isRegistered(std::string devString);
    static void listenPorts(MZDevice *dev);
    static void unListenPorts(MZDevice *dev);
};
