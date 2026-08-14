#pragma once

#include <stdint.h>
#include <string>
#include <functional>
#include <memory>
#include <map>
#include <new>
#include <vector>

#include "common.hpp"
#include "iniparser.h"

#define REGISTER_MZ_DEVICE(CLASS) \
    MZDevice* create_##CLASS() { return new (std::nothrow) CLASS(); } \
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
constexpr uint8_t MAX_DEVICE_PORTS = 16;
constexpr uint8_t MAX_DEVICES_PER_PORT = 2;

constexpr uint8_t E_PORT_ALLOCATED = 255;
constexpr uint8_t E_PORT_NOT_AVAILABLE = 254;
constexpr uint8_t E_MAX_DEVICES = 253;
constexpr uint8_t E_DEVICE_ALREADY_REGISTERED = 252;
constexpr uint8_t E_DEVICE_NOT_REGISTERED = 251;
// readConfig: the device's RAM buffers don't fit - boot skips the device
// and continues instead of halting (a missing device is diagnosable from
// the running machine; a halted boot is not)
constexpr int E_DEVICE_NO_MEMORY = 250;

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
    // Each device declares its port configuration
    virtual std::vector<uint8_t> getReadPorts() const = 0;
    virtual std::vector<uint8_t> getWritePorts() const = 0;
    // Optionally override to customize how base_port remapping works
    virtual std::pair<std::vector<uint8_t>, std::vector<uint8_t>> applyBasePort(uint8_t basePort) const;
    virtual int readConfig(dictionary *ini) = 0;
    virtual int flush() = 0;

    const ReadPortMapping* getReadMappings() const { return readMappings; }
    const WritePortMapping* getWriteMappings() const { return writeMappings; }

    uint8_t getReadCount() const { return readPortCount; }
    uint8_t getWriteCount() const { return writePortCount; }

    std::string getDevID() const { return devID; }
    void setDevID(const std::string& id) { devID = id; }
    bool isEnabled() const { return enabled; }
    void Enable() { enabled = true; }
    void Disable() { enabled = false; }
    // Helper to initialize port mappings from provided port lists
    void initializePortMappings(const std::vector<uint8_t>& readPorts,
                                const std::vector<uint8_t>& writePorts);

protected:
    ReadPortMapping readMappings[MAX_DEVICE_PORTS];
    WritePortMapping writeMappings[MAX_DEVICE_PORTS];
    uint8_t readPortCount = 0;
    uint8_t writePortCount = 0;
    std::string devID;
    bool enabled = true;
};

class MZDeviceManager {
public:
    using Creator = std::function<MZDevice*()>;

    static void registerClass(const std::string& name, Creator creator) {
        getMap()[name] = std::move(creator);
    }

    static MZDevice* createDevice(const std::string& devType, const std::string& id);
    static void flushAll();
    static int disableDevice(MZDevice* dev);
    static int enableDevice(MZDevice* dev);
    // Configure a device with explicit port lists
    static int setPortsList(MZDevice* dev,
                            const std::vector<uint8_t>& readPorts,
                            const std::vector<uint8_t>& writePorts);

    // Aggregated, multi-listener helpers for fast dispatch from listen_loop
    static inline bool portNeedsExwait(uint8_t port) { return readListeners[port].needsExwaitAny || writeListeners[port].needsExwaitAny; }
    static inline bool hasReadListeners(uint8_t port) { return readListeners[port].count > 0; }
    static inline bool hasWriteListeners(uint8_t port) { return writeListeners[port].count > 0; }
    // Perform aggregated read: selects a device to provide data, and returns whether any device wants interrupt
    static bool handleRead(uint8_t port, uint8_t* dt, uint8_t high_addr);
    // Perform aggregated write: broadcasts to all listeners, and returns whether any device wants interrupt
    static bool handleWrite(uint8_t port, uint8_t dt, uint8_t high_addr);

    // Flat fast-path dispatch tables for listen_loop. Ports with a single
    // listener dispatch directly (v0.2.0-equivalent hot path: minimal loads
    // before EXWAIT assertion, which is timing-critical against the Z80's
    // /WAIT sampling); ports with multiple listeners point at a thunk into
    // the aggregated handleRead/handleWrite. Rebuilt on any listener change.
    static inline int (*flatReadFn[MAX_PORTS])(MZDevice*, uint8_t, uint8_t*, uint8_t) = {nullptr};
    static inline MZDevice* flatReadDev[MAX_PORTS] = {nullptr};
    static inline int (*flatWriteFn[MAX_PORTS])(MZDevice*, uint8_t, uint8_t, uint8_t) = {nullptr};
    static inline MZDevice* flatWriteDev[MAX_PORTS] = {nullptr};
    static inline bool flatExwait[MAX_PORTS] = {false};
    static void buildFlatTables();

private:
    static std::map<std::string, Creator>& getMap() { static std::map<std::string, Creator> creators; return creators; }
    static inline MZDevice* devices[MAX_MZ_DEVICES] = {nullptr};
    static inline uint8_t deviceCount = 0;

    struct PortReadListeners {
        uint8_t count;
        MZDevice* devs[MAX_DEVICES_PER_PORT];
        int (*fns[MAX_DEVICES_PER_PORT])(MZDevice*, uint8_t, uint8_t*, uint8_t);
        bool needsExwaitAny;
    };

    struct PortWriteListeners {
        uint8_t count;
        MZDevice* devs[MAX_DEVICES_PER_PORT];
        int (*fns[MAX_DEVICES_PER_PORT])(MZDevice*, uint8_t, uint8_t, uint8_t);
        bool needsExwaitAny;
    };

    static inline PortReadListeners readListeners[MAX_PORTS];
    static inline PortWriteListeners writeListeners[MAX_PORTS];
    static inline bool isRegistered(std::string devString);
    static void listenPorts(MZDevice *dev);
    static void unListenPorts(MZDevice *dev);
    static void recomputeExwait(uint8_t port);
};
