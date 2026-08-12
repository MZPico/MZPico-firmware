#include "ctc.hpp"
#include "common.hpp"

REGISTER_MZ_DEVICE(CTCDevice)

CTCDevice::CTCDevice() {
    for (int i = 0; i < 8; i++) {
        writeMappings[i].fn = nullptr;
    }

    writeMappings[2].fn = CTCDevice::writePortC;      // D2: 8255 port C data
    writeMappings[3].fn = CTCDevice::writePortCtrl;   // D3: 8255 control (BSR/mode)
    writeMappings[4].fn = CTCDevice::writeCounter0;   // D4: 8253 counter 0
    writeMappings[7].fn = CTCDevice::writeCounterCtrl; // D7: 8253 control

    // Initialize port mappings with defaults
    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);

    gateOpen = false;
    counterRunning = false;
    squareWave = false;
    reloadValue = 0;
    counter = 0;
    outputHigh = true;
    cycleResid = 0;
    volume = 100;
    pan = 50;
    pan256 = (uint16_t)((pan * 256) / 100);

    loadMode = LoadMode::None;
    waitingMsb = false;
    latchedLsb = 0;
}

CTCDevice::~CTCDevice() {
    i2s_audio_unregister_source(this);
}

int CTCDevice::init() {
    return i2s_audio_register_source(this);
}

std::vector<uint8_t> CTCDevice::getReadPorts() const {
    return {};
}

std::vector<uint8_t> CTCDevice::getWritePorts() const {
    std::vector<uint8_t> ports;
    for (uint8_t i = 0; i < 8; ++i) {
        ports.push_back(CTC_PORT_BASE + i);
    }
    return ports;
}

int CTCDevice::readConfig(dictionary *ini) {
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

    return 0;
}

// 0xD2: direct 8255 port C write - PC0 is the sound gate
RAM_FUNC int CTCDevice::writePortC(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* dev = static_cast<CTCDevice*>(self);
    dev->gateOpen = (dt & 0x01) != 0;
    return 0;
}

// 0xD3: 8255 control register. Bit 7 = 1 is a mode-set word (resets all
// port C outputs on a real 8255 - gate closes). Bit 7 = 0 is a Bit
// Set/Reset command: bits 3-1 select the PC bit, bit 0 is the value; only
// PC0 is the sound gate - BSR commands for PC1-PC7 (tape motor, interrupt
// mask, ...) must not touch it.
RAM_FUNC int CTCDevice::writePortCtrl(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* dev = static_cast<CTCDevice*>(self);
    if (dt & 0x80) {
        dev->gateOpen = false;
    } else if (((dt >> 1) & 0x07) == 0) {
        dev->gateOpen = (dt & 0x01) != 0;
    }
    return 0;
}

// 0xD7: 8253 control word. A control word for counter 0 halts the counter
// (output parks high) until a full count is loaded - the ROM relies on this
// as its silencing idiom ("control word + open gate", no count). A latch
// command (RL = 00) only latches the count for reading; the programmed
// access mode persists.
RAM_FUNC int CTCDevice::writeCounterCtrl(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* dev = static_cast<CTCDevice*>(self);
    uint8_t counter_select = (dt >> 6) & 0x03;
    if (counter_select != 0) {
        return 0; // only counter #0 supported
    }

    uint8_t rl = (dt >> 4) & 0x03;
    if (rl == 0) {
        return 0; // counter latch for reading; no state change for writes
    }

    switch (rl) {
        case 1: dev->loadMode = LoadMode::LSB; break;
        case 2: dev->loadMode = LoadMode::MSB; break;
        case 3: dev->loadMode = LoadMode::LSB_MSB; break;
    }
    dev->waitingMsb = false;

    // Modes (M2 M1 M0 in bits 3-1): x11 = mode 3 square wave; everything
    // else (mode 0 one-shot used as the 10ms MUSIC timebase, etc.) makes
    // no audible tone.
    dev->squareWave = ((dt >> 1) & 0x03) == 0x03;
    dev->counterRunning = false;
    dev->outputHigh = true;
    return 0;
}

RAM_FUNC int CTCDevice::writeCounter0(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* dev = static_cast<CTCDevice*>(self);

    switch (dev->loadMode) {
        case LoadMode::LSB: {
            uint16_t value = (static_cast<uint16_t>(dev->reloadValue) & 0xFF00) | dt;
            dev->applyReload(value);
            break;
        }
        case LoadMode::MSB: {
            uint16_t value = (static_cast<uint16_t>(dt) << 8) | (static_cast<uint16_t>(dev->reloadValue) & 0x00FF);
            dev->applyReload(value);
            break;
        }
        case LoadMode::LSB_MSB: {
            if (!dev->waitingMsb) {
                dev->latchedLsb = dt;
                dev->waitingMsb = true;
            } else {
                uint16_t value = static_cast<uint16_t>(dev->latchedLsb) | (static_cast<uint16_t>(dt) << 8);
                dev->waitingMsb = false;
                dev->applyReload(value);
            }
            break;
        }
        default:
            break;
    }

    return 0;
}

void CTCDevice::applyReload(uint16_t value) {
    uint32_t effective = value;
    if (effective == 0) {
        effective = 0x10000;
    }
    reloadValue = effective;
    // Mode 3: output high for ceil(N/2) counts, low for floor(N/2)
    counter = (effective + 1) >> 1;
    outputHigh = true;
    counterRunning = true;
}

void CTCDevice::renderSample(int16_t& left, int16_t& right) {
    if (!gateOpen || !counterRunning || !squareWave || reloadValue < 2) {
        left = 0;
        right = 0;
        return;
    }

    cycleResid += CTC_INPUT_CLOCK;
    uint32_t cyclesToRun = cycleResid / AUDIO_SAMPLE_RATE;
    cycleResid %= AUDIO_SAMPLE_RATE;

    // Mode 3 square wave: frequency = clock / N, i.e. the output toggles
    // every half-period (N/2 counts), not every N counts
    uint32_t reload = reloadValue;
    if (counter == 0 || counter > reload) {
        // reload raced in from core1 mid-render; resynchronize
        counter = (reload + 1) >> 1;
    }
    for (uint32_t i = 0; i < cyclesToRun; i++) {
        if (--counter == 0) {
            outputHigh = !outputHigh;
            counter = outputHigh ? (reload + 1) >> 1 : reload >> 1;
        }
    }

    int32_t amplitude = (CTC_BASE_AMPLITUDE * volume) / 100;
    int32_t sample = outputHigh ? amplitude : -amplitude;

    left = static_cast<int16_t>((sample * (int32_t)(256 - pan256)) >> 8);
    right = static_cast<int16_t>((sample * (int32_t)pan256) >> 8);
}
