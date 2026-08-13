#include "ctc.hpp"
#include "common.hpp"
#include "pico/time.h"

REGISTER_MZ_DEVICE(CTCDevice)

CTCDevice::CTCDevice() {
    for (int i = 0; i < 8; i++) {
        writeMappings[i].fn = nullptr;
    }

    writeMappings[2].fn = CTCDevice::writePort;   // D2: 8255 port C data
    writeMappings[3].fn = CTCDevice::writePort;   // D3: 8255 control (BSR/mode)
    writeMappings[4].fn = CTCDevice::writePort;   // D4: 8253 counter 0
    writeMappings[7].fn = CTCDevice::writePort;   // D7: 8253 control

    // Initialize port mappings with defaults
    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);

    ioqHead = 0;
    ioqTail = 0;

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

// core1: enqueue the write with a 1MHz-timer timestamp; decoding and the
// tone model run on core0 at the write's scheduled sample position
RAM_FUNC int CTCDevice::writePort(MZDevice* self, uint8_t port, uint8_t dt, uint8_t) {
    auto* dev = static_cast<CTCDevice*>(self);
    uint32_t h = dev->ioqHead;
    if ((h - dev->ioqTail) < IOQ_SIZE) {
        uint32_t i = h & (IOQ_SIZE - 1);
        dev->ioq[i].ts = time_us_32();
        dev->ioq[i].port = port;
        dev->ioq[i].data = dt;
        dev->ioqHead = h + 1;
    }
    return 0;
}

void CTCDevice::applyWrite(uint8_t port, uint8_t dt) {
    switch (port & 0x07) {
        case 2:   // 0xD2: direct 8255 port C write - PC0 is the sound gate
            tone.setGate((dt & 0x01) != 0);
            break;
        case 3:   // 0xD3: 8255 control. Mode-set (bit 7) resets port C ->
                  // gate closes; BSR touches the gate only when it selects
                  // PC0 (tape motor / interrupt mask BSRs must not)
            if (dt & 0x80) {
                tone.setGate(false);
            } else if (((dt >> 1) & 0x07) == 0) {
                tone.setGate((dt & 0x01) != 0);
            }
            break;
        case 4:   // 0xD4: 8253 counter 0 data
            tone.countWrite(dt);
            break;
        case 7:   // 0xD7: 8253 control word
            tone.ctrlWrite(dt);
            break;
        default:
            break;
    }
}

void CTCDevice::processWrites() {
    uint32_t head = ioqHead;   // snapshot; core1 keeps appending
    bool have = head != ioqTail;
    uint32_t newestTs = have ? ioq[(head - 1) & (IOQ_SIZE - 1)].ts : 0;
    timeline.beginBuffer(newestTs, have);

    while (ioqTail != head) {
        uint32_t i = ioqTail & (IOQ_SIZE - 1);
        uint32_t ts = ioq[i].ts;
        // Leave events beyond this buffer's window queued for the next one
        if (!timeline.accepts(ts)) break;
        timeline.push(ts, ioq[i].port, ioq[i].data);
        ioqTail++;
    }
}

void CTCDevice::renderSample(int16_t& left, int16_t& right) {
    timeline.applyDue([this](uint8_t port, uint8_t data) {
        applyWrite(port, data);
    });

    int32_t amplitude = (CTC_BASE_AMPLITUDE * volume) / 100;
    int32_t sample = tone.render(amplitude);

    left = static_cast<int16_t>((sample * (int32_t)(256 - pan256)) >> 8);
    right = static_cast<int16_t>((sample * (int32_t)pan256) >> 8);
}
