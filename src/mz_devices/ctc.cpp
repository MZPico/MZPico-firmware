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

    volume = 100;
    pan = 50;
    pan256 = (uint16_t)((pan * 256) / 100);
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
    static_cast<CTCDevice*>(self)->tone.setGate((dt & 0x01) != 0);
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
        dev->tone.setGate(false);
    } else if (((dt >> 1) & 0x07) == 0) {
        dev->tone.setGate((dt & 0x01) != 0);
    }
    return 0;
}

// 0xD7: 8253 control word
RAM_FUNC int CTCDevice::writeCounterCtrl(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    static_cast<CTCDevice*>(self)->tone.ctrlWrite(dt);
    return 0;
}

// 0xD4: 8253 counter 0 data
RAM_FUNC int CTCDevice::writeCounter0(MZDevice* self, uint8_t, uint8_t dt, uint8_t) {
    static_cast<CTCDevice*>(self)->tone.countWrite(dt);
    return 0;
}

void CTCDevice::renderSample(int16_t& left, int16_t& right) {
    int32_t amplitude = (CTC_BASE_AMPLITUDE * volume) / 100;
    int32_t sample = tone.render(amplitude);

    left = static_cast<int16_t>((sample * (int32_t)(256 - pan256)) >> 8);
    right = static_cast<int16_t>((sample * (int32_t)pan256) >> 8);
}
