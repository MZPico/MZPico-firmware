#include "mz_devices.hpp"
#include "device.hpp"

// Default implementation: create consecutive ports from basePort
std::pair<std::vector<uint8_t>, std::vector<uint8_t>> MZDevice::applyBasePort(uint8_t basePort) const {
    std::vector<uint8_t> readPorts;
    std::vector<uint8_t> writePorts;
    
    auto defaultReads = getReadPorts();
    auto defaultWrites = getWritePorts();
    
    for (size_t i = 0; i < defaultReads.size(); ++i) {
        readPorts.push_back(basePort + i);
    }
    for (size_t i = 0; i < defaultWrites.size(); ++i) {
        writePorts.push_back(basePort + i);
    }
    
    return {readPorts, writePorts};
}

void MZDevice::initializePortMappings(const std::vector<uint8_t>& readPorts,
                                      const std::vector<uint8_t>& writePorts) {
    // Initialize read port mappings
    readPortCount = readPorts.size();
    if (readPortCount > MAX_DEVICE_PORTS) readPortCount = MAX_DEVICE_PORTS;
    for (uint8_t i = 0; i < readPortCount; ++i) {
        readMappings[i].port = readPorts[i];
    }
    
    // Initialize write port mappings
    writePortCount = writePorts.size();
    if (writePortCount > MAX_DEVICE_PORTS) writePortCount = MAX_DEVICE_PORTS;
    for (uint8_t i = 0; i < writePortCount; ++i) {
        writeMappings[i].port = writePorts[i];
    }
}

bool MZDeviceManager::isRegistered(std::string devID) {
    for (uint8_t i=0; i<deviceCount; i++) {
        if (devices[i]->getDevID() == devID)
            return true;
    }
    return false;
}

void MZDeviceManager::listenPorts(MZDevice* dev) {
    // Register read listeners
    for (uint8_t i = 0; i < dev->getReadCount(); i++) {
        uint8_t port = dev->getReadMappings()[i].port;
        auto &L = readListeners[port];
        if (L.count < MAX_DEVICES_PER_PORT) {
            L.devs[L.count] = dev;
            L.fns[L.count] = dev->getReadMappings()[i].fn;
            L.count++;
            if (dev->needsExwait()) L.needsExwaitAny = true;
        }
    }
    // Register write listeners
    for (uint8_t i = 0; i < dev->getWriteCount(); i++) {
        uint8_t port = dev->getWriteMappings()[i].port;
        auto &L = writeListeners[port];
        if (L.count < MAX_DEVICES_PER_PORT) {
            L.devs[L.count] = dev;
            L.fns[L.count] = dev->getWriteMappings()[i].fn;
            L.count++;
            if (dev->needsExwait()) L.needsExwaitAny = true;
        }
    }
}

void MZDeviceManager::unListenPorts(MZDevice* dev) {
    // Remove from read listeners
    for (uint8_t i = 0; i < dev->getReadCount(); i++) {
        uint8_t port = dev->getReadMappings()[i].port;
        auto &L = readListeners[port];
        for (uint8_t j = 0; j < L.count; ++j) {
            if (L.devs[j] == dev) {
                // Compact arrays
                for (uint8_t k = j + 1; k < L.count; ++k) {
                    L.devs[k-1] = L.devs[k];
                    L.fns[k-1]  = L.fns[k];
                }
                L.devs[L.count-1] = nullptr;
                L.fns[L.count-1]  = nullptr;
                L.count--;
                break;
            }
        }
        recomputeExwait(port);
    }
    // Remove from write listeners
    for (uint8_t i = 0; i < dev->getWriteCount(); i++) {
        uint8_t port = dev->getWriteMappings()[i].port;
        auto &L = writeListeners[port];
        for (uint8_t j = 0; j < L.count; ++j) {
            if (L.devs[j] == dev) {
                for (uint8_t k = j + 1; k < L.count; ++k) {
                    L.devs[k-1] = L.devs[k];
                    L.fns[k-1]  = L.fns[k];
                }
                L.devs[L.count-1] = nullptr;
                L.fns[L.count-1]  = nullptr;
                L.count--;
                break;
            }
        }
        recomputeExwait(port);
    }
}

void MZDeviceManager::recomputeExwait(uint8_t port) {
    bool any = false;
    auto &RL = readListeners[port];
    for (uint8_t i = 0; i < RL.count; ++i) {
        if (RL.devs[i] && RL.devs[i]->needsExwait()) { any = true; break; }
    }
    RL.needsExwaitAny = any;

    any = false;
    auto &WL = writeListeners[port];
    for (uint8_t i = 0; i < WL.count; ++i) {
        if (WL.devs[i] && WL.devs[i]->needsExwait()) { any = true; break; }
    }
    WL.needsExwaitAny = any;
}

RAM_FUNC bool MZDeviceManager::handleRead(uint8_t port, uint8_t* dt, uint8_t high_addr) {
    auto &L = readListeners[port];
    if (L.count == 0) return false;

    bool wantsInterrupt = false;

    // Execute read on all registered devices to allow side effects.
    // The first device's returned value is used for the bus; others use a temporary.
    for (uint8_t i = 0; i < L.count; ++i) {
        if (!L.fns[i]) continue;
        if (i == 0) {
            L.fns[i](L.devs[i], port, dt, high_addr);
        } else {
            uint8_t tmp = 0;
            L.fns[i](L.devs[i], port, &tmp, high_addr);
        }
        if (L.devs[i] && L.devs[i]->isInterrupt()) wantsInterrupt = true;
    }

    return wantsInterrupt;
}

RAM_FUNC bool MZDeviceManager::handleWrite(uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto &L = writeListeners[port];
    if (L.count == 0) return false;

    bool wantsInterrupt = false;
    // Broadcast and aggregate interrupt in a single pass
    for (uint8_t i = 0; i < L.count; ++i) {
        if (L.fns[i]) {
            L.fns[i](L.devs[i], port, dt, high_addr);
        }
        if (L.devs[i] && L.devs[i]->isInterrupt()) wantsInterrupt = true;
    }
    return wantsInterrupt;
}

// Thunks for the rare multi-listener ports: route through the aggregated
// dispatch. Interrupt aggregation note: listen_loop calls isInterrupt() on
// the FIRST listener only for flat dispatch; today the only interrupt
// source (FDC) never shares a port, so this is equivalent.
RAM_FUNC static int multiReadThunk(MZDevice*, uint8_t port, uint8_t* dt, uint8_t high_addr) {
    MZDeviceManager::handleRead(port, dt, high_addr);
    return 0;
}

RAM_FUNC static int multiWriteThunk(MZDevice*, uint8_t port, uint8_t dt, uint8_t high_addr) {
    MZDeviceManager::handleWrite(port, dt, high_addr);
    return 0;
}

void MZDeviceManager::buildFlatTables() {
    for (uint16_t p = 0; p < MAX_PORTS; ++p) {
        auto &RL = readListeners[p];
        if (RL.count == 1) {
            // Single listener: direct dispatch (fn may be a null placeholder,
            // which keeps the port unhandled, matching v0.2.0 semantics)
            flatReadFn[p] = RL.fns[0];
            flatReadDev[p] = RL.devs[0];
        } else if (RL.count > 1) {
            flatReadFn[p] = multiReadThunk;
            flatReadDev[p] = RL.devs[0];
        } else {
            flatReadFn[p] = nullptr;
            flatReadDev[p] = nullptr;
        }

        auto &WL = writeListeners[p];
        if (WL.count == 1) {
            flatWriteFn[p] = WL.fns[0];
            flatWriteDev[p] = WL.devs[0];
        } else if (WL.count > 1) {
            flatWriteFn[p] = multiWriteThunk;
            flatWriteDev[p] = WL.devs[0];
        } else {
            flatWriteFn[p] = nullptr;
            flatWriteDev[p] = nullptr;
        }

        flatExwait[p] = RL.needsExwaitAny || WL.needsExwaitAny;
    }
}

MZDevice* MZDeviceManager::createDevice(const std::string& devType, const std::string& id) {
    auto& creators = getMap();
    auto it = creators.find(devType);
    if (it == creators.end())
    {
        return nullptr;
    }

    if (deviceCount >= MAX_MZ_DEVICES)
        return nullptr;

    if (isRegistered(id))
        return nullptr;

    MZDevice* dev = (it->second)();
    dev->setDevID(id);
    devices[deviceCount++] = dev;
    return dev;
}

void MZDeviceManager::flushAll() {
    for (uint8_t i=0; i<deviceCount; i++)
        devices[i]->flush();
}

int MZDeviceManager::enableDevice(MZDevice* dev) {
    if (!dev)
        return 1;
    if (!isRegistered(dev->getDevID()))
        return E_DEVICE_NOT_REGISTERED;
    dev->Enable();
    listenPorts(dev);
    buildFlatTables();

    return 0;
}

int MZDeviceManager::disableDevice(MZDevice* dev) {
    if (!dev)
        return 1;
    if (!isRegistered(dev->getDevID()))
        return E_DEVICE_NOT_REGISTERED;
    dev->Disable();
    unListenPorts(dev);
    buildFlatTables();

    return 0;
}

int MZDeviceManager::setPortsList(MZDevice* dev,
                                  const std::vector<uint8_t>& readPorts,
                                  const std::vector<uint8_t>& writePorts) {
    if (!dev)
        return 1;
    if (!isRegistered(dev->getDevID()))
        return E_DEVICE_NOT_REGISTERED;

    // Validate counts match device capability
    if (readPorts.size() != dev->getReadCount())
        return E_PORT_ALLOCATED;
    if (writePorts.size() != dev->getWriteCount())
        return E_PORT_ALLOCATED;

    if (dev->isEnabled())
        unListenPorts(dev);

    // Initialize port mappings with provided lists
    dev->initializePortMappings(readPorts, writePorts);

    if (dev->isEnabled())
        listenPorts(dev);
    buildFlatTables();

    return 0;
}

