#include "ctc.hpp"
#include "common.hpp"

REGISTER_MZ_DEVICE(CTCDevice)

CTCDevice::CTCDevice() {
    readPortCount = 0;
    writePortCount = 8; // D0..D7

    for (int i = 0; i < writePortCount; i++) {
        writeMappings[i].fn = nullptr;
    }

    writeMappings[2].fn = CTCDevice::writePortC;      // D2
    writeMappings[3].fn = CTCDevice::writePortCtrl;   // D3
    writeMappings[4].fn = CTCDevice::writeCounter0;   // D4
    writeMappings[7].fn = CTCDevice::writeCounterCtrl; // D7

    gateEnabled = false;
    pcAllowsOutput = true;
    reloadValue = 0;
    counter = 0;
    outputHigh = false;
    cycleResid = 0;
    volume = 100;

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

int CTCDevice::readConfig(dictionary *ini) {
    if (!ini) return -1;

    int vol = iniparser_getint(ini, (getDevID() + ":volume").c_str(), 100);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    volume = static_cast<uint8_t>(vol);

    return 0;
}

RAM_FUNC int CTCDevice::writePortC(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* dev = static_cast<CTCDevice*>(self);
    dev->pcAllowsOutput = (dt & 0x01) != 0; // active low prohibits output
    return 0;
}

RAM_FUNC int CTCDevice::writePortCtrl(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* dev = static_cast<CTCDevice*>(self);
    dev->gateEnabled = (dt & 0x01) != 0;
    return 0;
}

RAM_FUNC int CTCDevice::writeCounterCtrl(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    auto* dev = static_cast<CTCDevice*>(self);
    uint8_t counter_select = (dt >> 6) & 0x03;
    if (counter_select != 0) {
        return 0; // only counter #0 supported
    }

    uint8_t rl = (dt >> 4) & 0x03;
    switch (rl) {
        case 1: dev->loadMode = LoadMode::LSB; break;
        case 2: dev->loadMode = LoadMode::MSB; break;
        case 3: dev->loadMode = LoadMode::LSB_MSB; break;
        default: dev->loadMode = LoadMode::None; break;
    }

    dev->waitingMsb = false;
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
    counter = effective;
    outputHigh = true;
}

void CTCDevice::renderSample(int16_t& left, int16_t& right) {
    if (!gateEnabled || !pcAllowsOutput || reloadValue == 0) {
        left = 0;
        right = 0;
        return;
    }

    cycleResid += CTC_INPUT_CLOCK;
    uint32_t cyclesToRun = cycleResid / AUDIO_SAMPLE_RATE;
    cycleResid %= AUDIO_SAMPLE_RATE;

    for (uint32_t i = 0; i < cyclesToRun; i++) {
        if (counter == 0) {
            counter = reloadValue;
        }
        counter--;
        if (counter == 0) {
            outputHigh = !outputHigh;
        }
    }

    int16_t amplitude = static_cast<int16_t>((CTC_BASE_AMPLITUDE * volume) / 100);
    int16_t sample = outputHigh ? amplitude : -amplitude;

    left = sample;
    right = sample;
}
