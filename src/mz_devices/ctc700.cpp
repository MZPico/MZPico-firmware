#include "common.hpp"

#ifdef BOARD_DELUXE

#include "ctc700.hpp"
#include "mem_snoop.hpp"
#include "hardware/dma.h"

REGISTER_MZ_DEVICE(CTC700Device)

// Diagnostic ports live in MZPico's private 0x40 block (pico_mgr 0x40-0x44,
// pico_rd 0x45-0x4B), clear of anything the machine itself decodes
static constexpr uint8_t PORT_DIAG_CURSOR = 0x4E;
static constexpr uint8_t PORT_DIAG_EVENTS = 0x4F;

// Write-listener port order (positional match with writeMappings)
static constexpr uint8_t PORT_BANK_E1 = 0xE1;  // DRAM over D000-FFFF: peripherals unmapped
static constexpr uint8_t PORT_BANK_E3 = 0xE3;  // peripherals + upper ROM mapped back
static constexpr uint8_t PORT_BANK_E4 = 0xE4;  // power-on map: peripherals mapped, lock cleared
static constexpr uint8_t PORT_BANK_E5 = 0xE5;  // prohibit upper area
static constexpr uint8_t PORT_BANK_E6 = 0xE6;  // return (unlock) upper area
static constexpr uint8_t PORT_GDG_DMD = 0xCE;  // display mode; bit 3 = MZ-700 mode

CTC700Device::CTC700Device() {
    readMappings[0].fn = CTC700Device::readDiagCursor;    // DE
    readMappings[1].fn = CTC700Device::readDiagEvents;    // DF
    writeMappings[0].fn = CTC700Device::writeBankUnmap;   // E1
    writeMappings[1].fn = CTC700Device::writeBankMap;     // E3
    writeMappings[2].fn = CTC700Device::writeBankMap;     // E4 (also unlocks, same effect for us)
    writeMappings[3].fn = CTC700Device::writeBankLock;    // E5
    writeMappings[4].fn = CTC700Device::writeBankUnlock;  // E6
    writeMappings[5].fn = CTC700Device::writeDmd;         // CE

    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);

    // The machine powers up in the MZ-700 map with peripherals mapped;
    // an MZ-800-mode DMD write disables the snoop when it arrives
    mz700Mode = true;
    periMapped = true;
    periLocked = false;
    forceActive = false;

    gateOpen = false;
    counterRunning = false;
    squareWave = false;
    reloadValue = 0;
    counter = 0;
    outputHigh = true;
    cycleResid = 0;
    loadMode = LoadMode::None;
    waitingMsb = false;
    latchedLsb = 0;

    cursorValid = false;
    cursor = 0;

    snoopDmaCh = -1;
    e0Events = 0;

    volume = 100;
    pan = 50;
    pan256 = (uint16_t)((pan * 256) / 100);
}

CTC700Device::~CTC700Device() {
    i2s_audio_unregister_source(this);
}

int CTC700Device::init() {
    snoopDmaCh = mem_snoop_channel();
    return i2s_audio_register_source(this);
}

std::vector<uint8_t> CTC700Device::getReadPorts() const {
    return {PORT_DIAG_CURSOR, PORT_DIAG_EVENTS};
}

std::vector<uint8_t> CTC700Device::getWritePorts() const {
    return {PORT_BANK_E1, PORT_BANK_E3, PORT_BANK_E4,
            PORT_BANK_E5, PORT_BANK_E6, PORT_GDG_DMD};
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

    // Diagnostic: render regardless of mode/bank state, so the capture
    // chain can be exercised from MZ-800 mode with plain POKEs to E004+
    forceActive = iniparser_getboolean(ini, (getDevID() + ":force").c_str(), false);

    return 0;
}

// ---- core1: bank/mode tracking (I/O writes) ----

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

RAM_FUNC int CTC700Device::writeDmd(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    static_cast<CTC700Device*>(self)->mz700Mode = (dt & 0x08) != 0;
    return 0;
}

// ---- core1: diagnostic reads ----

RAM_FUNC int CTC700Device::readDiagCursor(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    auto* dev = static_cast<CTC700Device*>(self);
    if (dev->snoopDmaCh >= 0) {
        // Word index of the DMA producer, low 8 bits: advances on every
        // captured memory write, so two reads with any activity between
        // them must differ if the PIO->DMA chain is alive
        *dt = (uint8_t)(dma_hw->ch[dev->snoopDmaCh].write_addr >> 2);
    } else {
        *dt = 0xEE;
    }
    return 0;
}

RAM_FUNC int CTC700Device::readDiagEvents(MZDevice* self, uint8_t, uint8_t* dt, uint8_t) {
    *dt = (uint8_t)static_cast<CTC700Device*>(self)->e0Events;
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
        e0Events = e0Events + 1;
        if (!snoopActive()) continue;

        handlePeripheralWrite(low, (w >> 24) & 0xFF);
    }
}

void CTC700Device::handlePeripheralWrite(uint8_t low, uint8_t data) {
    switch (low) {
        case 0x04: writeCounter0(data); break;      // 8253 counter 0
        case 0x07: writeCounterCtrl(data); break;   // 8253 control word
        case 0x08: gateOpen = (data & 0x01) != 0; break;  // melody gate latch
        default: break;                              // counters 1/2: not sound
    }
}

// ---- 8253 counter 0 model (same semantics as the I/O-mode CTC device) ----

void CTC700Device::writeCounterCtrl(uint8_t dt) {
    if (((dt >> 6) & 0x03) != 0) {
        return; // counters 1/2
    }
    uint8_t rl = (dt >> 4) & 0x03;
    if (rl == 0) {
        return; // latch for reading; write mode persists
    }
    switch (rl) {
        case 1: loadMode = LoadMode::LSB; break;
        case 2: loadMode = LoadMode::MSB; break;
        case 3: loadMode = LoadMode::LSB_MSB; break;
    }
    waitingMsb = false;
    squareWave = ((dt >> 1) & 0x03) == 0x03;  // mode 3
    counterRunning = false;                    // halted until a count is loaded
    outputHigh = true;
}

void CTC700Device::writeCounter0(uint8_t dt) {
    switch (loadMode) {
        case LoadMode::LSB:
            applyReload((uint16_t)((reloadValue & 0xFF00) | dt));
            break;
        case LoadMode::MSB:
            applyReload((uint16_t)(((uint16_t)dt << 8) | (reloadValue & 0x00FF)));
            break;
        case LoadMode::LSB_MSB:
            if (!waitingMsb) {
                latchedLsb = dt;
                waitingMsb = true;
            } else {
                waitingMsb = false;
                applyReload((uint16_t)(((uint16_t)dt << 8) | latchedLsb));
            }
            break;
        default:
            break;
    }
}

void CTC700Device::applyReload(uint16_t value) {
    uint32_t effective = value;
    if (effective == 0) {
        effective = 0x10000;
    }
    reloadValue = effective;
    counter = (effective + 1) >> 1;
    outputHigh = true;
    counterRunning = true;
}

void CTC700Device::renderSample(int16_t& left, int16_t& right) {
    if (!gateOpen || !counterRunning || !squareWave || reloadValue < 2 || !snoopActive()) {
        left = 0;
        right = 0;
        return;
    }

    cycleResid += CTC700_INPUT_CLOCK;
    uint32_t cyclesToRun = cycleResid / AUDIO_SAMPLE_RATE;
    cycleResid %= AUDIO_SAMPLE_RATE;

    uint32_t reload = reloadValue;
    if (counter == 0 || counter > reload) {
        counter = (reload + 1) >> 1;
    }
    for (uint32_t i = 0; i < cyclesToRun; i++) {
        if (--counter == 0) {
            outputHigh = !outputHigh;
            counter = outputHigh ? (reload + 1) >> 1 : reload >> 1;
        }
    }

    int32_t amplitude = (CTC700_BASE_AMPLITUDE * volume) / 100;
    int32_t sample = outputHigh ? amplitude : -amplitude;

    left = static_cast<int16_t>((sample * (int32_t)(256 - pan256)) >> 8);
    right = static_cast<int16_t>((sample * (int32_t)pan256) >> 8);
}

#endif // BOARD_DELUXE
