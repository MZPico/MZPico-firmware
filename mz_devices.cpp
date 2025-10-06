#include "mz_devices.hpp"
#include "device.hpp"

void MZDevice::setPorts() {
    for (int i = 0; i < readPortCount; i++)
        readMappings[i].port  = basePort + i;
    for (int i = 0; i < writePortCount; i++)
        writeMappings[i].port  = basePort + i;
}

bool MZDeviceManager::isRegistered(std::string devID) {
    for (uint8_t i=0; i<deviceCount; i++) {
        if (devices[i]->getDevID() == devID)
            return true;
    }
    return false;
}

void MZDeviceManager::listenPorts(MZDevice* dev) {
    for (uint8_t i = 0; i < dev->getReadCount(); i++) {
        uint8_t port = dev->getReadMappings()[i].port;
        readPortMap[port] = dev;
        readFunctions[port] = dev->getReadMappings()[i].fn;
    }

    for (uint8_t i = 0; i < dev->getWriteCount(); i++) {
        uint8_t port = dev->getWriteMappings()[i].port;
        writePortMap[port] = dev;
        writeFunctions[port] = dev->getWriteMappings()[i].fn;
    }
}

void MZDeviceManager::unListenPorts(MZDevice* dev) {
    for (uint8_t i = 0; i < dev->getReadCount(); i++) {
        uint8_t port = dev->getReadMappings()[i].port;
        readPortMap[port] = NULL;
        readFunctions[port] = NULL;
    }

    for (uint8_t i = 0; i < dev->getWriteCount(); i++) {
        uint8_t port = dev->getWriteMappings()[i].port;
        writePortMap[port] = NULL;
        writeFunctions[port] = NULL;
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

    MZDevice* dev = (it->second)();
    if (isRegistered(id))
        return nullptr;

    dev->setDevID(id);
    //if (dev->isEnabled()) {
    //    listenPorts(dev);
    //}

    devices[deviceCount++] = dev;
    return dev;
}

int MZDeviceManager::enableDevice(MZDevice* dev) {
    if (!dev)
        return 1;
    if (!isRegistered(dev->getDevID()))
        return E_DEVICE_NOT_REGISTERED;
    dev->Enable();
    listenPorts(dev);

    return 0;
}

int MZDeviceManager::disableDevice(MZDevice* dev) {
    if (!dev)
        return 1;
    if (!isRegistered(dev->getDevID()))
        return E_DEVICE_NOT_REGISTERED;
    dev->Disable();
    unListenPorts(dev);

    return 0;
}

int MZDeviceManager::setBasePort(MZDevice* dev, uint8_t basePort) {
    if (!dev)
        return 1;
    if (!basePort)
        return 1;
    if (!isRegistered(dev->getDevID()))
        return E_DEVICE_NOT_REGISTERED;
    if (dev->isEnabled())
        unListenPorts(dev);
    dev->setBasePort(basePort);
    dev->setPorts();
    if (dev->isEnabled())
        listenPorts(dev);
    return 0;
}

