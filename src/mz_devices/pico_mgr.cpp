#include "pico_mgr.hpp"
#include "file.hpp"
#include "bus.hpp"
#include "config.hpp"
#include "cloud_fs.hpp"
#include <string.h>

REGISTER_MZ_DEVICE(PicoMgr)

PicoMgr::PicoMgr()
    : response_command(0), idx(0), recordSize_(0), pack_(nullptr), unpack_(nullptr)
{
    readMappings[PICO_MGR_COMMAND_PORT_INDEX].fn = PicoMgr::readControl;
    writeMappings[PICO_MGR_COMMAND_PORT_INDEX].fn = PicoMgr::writeControl;
    readMappings[PICO_MGR_DATA_PORT_INDEX].fn = PicoMgr::readData;
    writeMappings[PICO_MGR_DATA_PORT_INDEX].fn = PicoMgr::writeData;
    readMappings[PICO_MGR_ADDR0_PORT_INDEX].fn = PicoMgr::readAddr0;
    writeMappings[PICO_MGR_ADDR0_PORT_INDEX].fn = PicoMgr::writeAddr0;
    readMappings[PICO_MGR_ADDR1_PORT_INDEX].fn = PicoMgr::readAddr1;
    writeMappings[PICO_MGR_ADDR1_PORT_INDEX].fn = PicoMgr::writeAddr1;
    writeMappings[PICO_MGR_RESET_PORT_INDEX].fn = PicoMgr::writeReset;

    readPortCount = PICO_MGR_READ_PORT_COUNT;
    writePortCount = PICO_MGR_WRITE_PORT_COUNT;
    setLength(0);
}

int PicoMgr::init() {
    idx = 0;
    response_command = 0;
    setLength(0);
    return 0;
}

int PicoMgr::readConfig(dictionary *ini) {
    return 0;
}

void PicoMgr::setString(const char *str) {
    if (!str) return;
    size_t ln = strlen(str);
    if (ln + 1 > payloadCapacity()) return;
    memcpy(data + 2, str, ln + 1);
    setLength(ln + 1);
    return;
}

void PicoMgr::addRaw(const uint8_t *dt, uint16_t sz) {
    if (!dt) return;
    if (sz > remainingCapacity()) return;
    memcpy(data + getLength() + 2, dt, sz);
    setLength(getLength() + sz);
    return;
}

uint16_t PicoMgr::getRaw(uint8_t *dest) {
    if (!dest) return 0;
    memcpy(dest, data+2, getLength());
    return getLength();
}

uint8_t *PicoMgr::allocateRaw(uint16_t sz) {
    if (sz > remainingCapacity()) return NULL;
    uint8_t *start = data + getLength() + 2;
    setLength(getLength() + sz);
    return start;
}

bool PicoMgr::addRecord(const void* record) {
    if (!pack_) return false;
    uint16_t length = getLength();
    if (length + recordSize_ > payloadCapacity()) return false;

    uint8_t* dst = data + 2 + length;
    pack_(record, dst);
    setLength(length + recordSize_);
    return true;
}

bool PicoMgr::getRecord(uint16_t index, void* outRecord) const {
    if (!unpack_) return false;
    uint16_t length = getLength();
    if (recordSize_ == 0) return false;
    uint16_t count = length / recordSize_;
    if (index >= count) return false;

    const uint8_t* src = data + 2 + index * recordSize_;
    unpack_(src, outRecord);
    return true;
}

int PicoMgr::writeControl(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* mgr = static_cast<PicoMgr*>(self);
    char path[256];
    int ret = 0;
    uint16_t len = mgr->getLength();

    auto setResponse = [&](int result) {
        mgr->response_command = result ? 0x04 : 0x03;
    };

    switch (dt) {
        case REPO_CMD_LIST_DIR:
        case REPO_CMD_MOUNT: {
            if (len == 0) return -1;
            std::string path(reinterpret_cast<char*>(mgr->data + 2), len-1);
            mgr->idx = 0;
            mgr->resetContent();
            if (dt == REPO_CMD_LIST_DIR)
                ret = read_directory(path.c_str(), mgr);
            else
                ret = mount_file(path.c_str(), mgr);
            setResponse(ret);
            break;
        }
        case REPO_CMD_LIST_DEV:
            mgr->idx = 0;
            mgr->resetContent();
            ret = get_device_list(mgr);
            setResponse(ret);
            break;
        case REPO_CMD_GET_CONFIG: {
            mgr->idx = 0;
            if (len == 0) return -1;
            std::string sectionName(reinterpret_cast<char*>(mgr->data + 2), len-1);
            mgr->resetContent();
            ret = getConfig(sectionName, mgr);
            setResponse(ret);
            break;
        }
        case REPO_CMD_GET_WIFI_STATUS: {
            mgr->idx = 0;
            mgr->resetContent();
            uint8_t status = static_cast<uint8_t>(cloud_wifi_state());
            mgr->addRaw(&status, 1);
            setResponse(0);
            break;
        }
        default:
            return -1;
    }
    return 0;
}

int PicoMgr::readControl(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* mgr = static_cast<PicoMgr*>(self);
    *dt = mgr->response_command;
    return 0;
}

int PicoMgr::writeData(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* mgr = static_cast<PicoMgr*>(self);
    mgr->data[mgr->idx++] = dt;
    if (mgr->idx >= PICO_MGR_BUFF_SIZE) mgr->idx = 0;
    return 0;
}

int PicoMgr::readData(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* mgr = static_cast<PicoMgr*>(self);
    *dt = mgr->data[mgr->idx++];
    if (mgr->idx >= PICO_MGR_BUFF_SIZE) mgr->idx = 0;
    return 0;
}

int PicoMgr::writeAddr0(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* mgr = static_cast<PicoMgr*>(self);
    mgr->idx = (mgr->idx & 0xFF00) | dt;
    return 0;
}

int PicoMgr::writeAddr1(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* mgr = static_cast<PicoMgr*>(self);
    mgr->idx = (mgr->idx & 0x00FF) | (dt << 8);
    return 0;
}

int PicoMgr::readAddr0(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* mgr = static_cast<PicoMgr*>(self);
    *dt = mgr->idx & 0xFF;
    return 0;
}

int PicoMgr::readAddr1(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* mgr = static_cast<PicoMgr*>(self);
    *dt = (mgr->idx >> 8) & 0xFF;
    return 0;
}

int PicoMgr::writeReset(MZDevice* self, uint8_t, uint8_t, uint8_t) {
    auto* mgr = static_cast<PicoMgr*>(self);
    mgr->idx = 0;
    return 0;
}
