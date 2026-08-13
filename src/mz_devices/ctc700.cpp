#include "common.hpp"

#ifdef BOARD_DELUXE

#include "ctc700.hpp"
#include "mem_snoop.hpp"

REGISTER_MZ_DEVICE(CTC700Device)

// Write-listener port order (positional match with writeMappings)
static constexpr uint8_t PORT_BANK_E1 = 0xE1;  // DRAM over D000-FFFF: peripherals unmapped
static constexpr uint8_t PORT_BANK_E3 = 0xE3;  // peripherals + upper ROM mapped back
static constexpr uint8_t PORT_BANK_E4 = 0xE4;  // power-on map: peripherals mapped, lock cleared
static constexpr uint8_t PORT_BANK_E5 = 0xE5;  // prohibit upper area
static constexpr uint8_t PORT_BANK_E6 = 0xE6;  // return (unlock) upper area

CTC700Device::CTC700Device() {
    writeMappings[0].fn = CTC700Device::writeBankUnmap;   // E1
    writeMappings[1].fn = CTC700Device::writeBankMap;     // E3
    writeMappings[2].fn = CTC700Device::writeBankMap;     // E4 (also unlocks, same effect for us)
    writeMappings[3].fn = CTC700Device::writeBankLock;    // E5
    writeMappings[4].fn = CTC700Device::writeBankUnlock;  // E6

    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);

    // The peripheral window is mapped in the power-on bank configuration
    periMapped = true;
    periLocked = false;
    forceActive = false;

    cursorValid = false;
    cursor = 0;

    volume = 100;
    pan = 50;
    pan256 = (uint16_t)((pan * 256) / 100);
}

CTC700Device::~CTC700Device() {
    i2s_audio_unregister_source(this);
}

int CTC700Device::init() {
    return i2s_audio_register_source(this);
}

std::vector<uint8_t> CTC700Device::getReadPorts() const {
    return {};
}

std::vector<uint8_t> CTC700Device::getWritePorts() const {
    return {PORT_BANK_E1, PORT_BANK_E3, PORT_BANK_E4,
            PORT_BANK_E5, PORT_BANK_E6};
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> CTC700Device::applyBasePort(uint8_t) const {
    return {getReadPorts(), getWritePorts()};
}

int CTC700Device::readConfig(dictionary *ini) {
    if (!ini) return -1;

    int vol = iniparser_getint(ini, (getDevID() + ":volume").c_str(), 100);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    volume = static_cast<uint8_t>(vol);

    int pan_value = iniparser_getint(ini, (getDevID() + ":pan").c_str(), 50);
    if (pan_value < 0) pan_value = 0;
    if (pan_value > 100) pan_value = 100;
    pan = static_cast<uint8_t>(pan_value);
    pan256 = (uint16_t)((pan * 256) / 100);

    // Diagnostic: render regardless of the bank state, so the capture
    // chain can be exercised with plain POKEs to E004+
    forceActive = iniparser_getboolean(ini, (getDevID() + ":force").c_str(), false);

    return 0;
}

// ---- core1: bank tracking (I/O writes) ----

RAM_FUNC int CTC700Device::writeBankUnmap(MZDevice* self, uint8_t, uint8_t, uint8_t) {
    static_cast<CTC700Device*>(self)->periMapped = false;
    return 0;
}

RAM_FUNC int CTC700Device::writeBankMap(MZDevice* self, uint8_t, uint8_t, uint8_t) {
    auto* dev = static_cast<CTC700Device*>(self);
    dev->periMapped = true;
    dev->periLocked = false;
    return 0;
}

RAM_FUNC int CTC700Device::writeBankLock(MZDevice* self, uint8_t, uint8_t, uint8_t) {
    static_cast<CTC700Device*>(self)->periLocked = true;
    return 0;
}

RAM_FUNC int CTC700Device::writeBankUnlock(MZDevice* self, uint8_t, uint8_t, uint8_t) {
    static_cast<CTC700Device*>(self)->periLocked = false;
    return 0;
}

// ---- core0: snoop ring consumer ----

void CTC700Device::processWrites() {
    mem_snoop_service();

    uint32_t now = mem_snoop_cursor();
    if (!cursorValid) {
        // First scan: skip whatever accumulated before we were ready
        cursor = now;
        cursorValid = true;
        return;
    }

    uint32_t pending = (now - cursor) & (MEM_SNOOP_RING_WORDS - 1);
    while (pending--) {
        uint32_t w = mem_snoop_read(cursor);
        cursor = (cursor + 1) & (MEM_SNOOP_RING_WORDS - 1);

        uint8_t high = (w >> 16) & 0xFF;
        if (high != 0xE0) continue;
        uint8_t low = (w >> 8) & 0xFF;
        if (low < 0x04 || low > 0x08) continue;
        if (!snoopActive()) continue;

        handlePeripheralWrite(low, (w >> 24) & 0xFF);
    }
}

void CTC700Device::handlePeripheralWrite(uint8_t low, uint8_t data) {
    switch (low) {
        case 0x04: tone.countWrite(data); break;             // 8253 counter 0
        case 0x07: tone.ctrlWrite(data); break;              // 8253 control word
        case 0x08: tone.setGate((data & 0x01) != 0); break;  // melody gate latch
        default: break;                                       // counters 1/2: not sound
    }
}

void CTC700Device::renderSample(int16_t& left, int16_t& right) {
    // Not gated on the bank state: unmapping the window only blocks WRITES
    // (handled in processWrites); a tone already playing keeps sounding on
    // real hardware
    int32_t amplitude = (CTC700_BASE_AMPLITUDE * volume) / 100;
    int32_t sample = tone.render(amplitude);

    left = static_cast<int16_t>((sample * (int32_t)(256 - pan256)) >> 8);
    right = static_cast<int16_t>((sample * (int32_t)pan256) >> 8);
}

#endif // BOARD_DELUXE
