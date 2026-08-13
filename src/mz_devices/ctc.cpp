#include "ctc.hpp"
#include "common.hpp"
#include "ctc_diag.hpp"
#include "pico/time.h"

REGISTER_MZ_DEVICE(CTCDevice)

CtcDiag g_ctc_diag = {};

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

    // Speaker resonance simulation amount, 0-100 (0 = flat/off)
    int spk = iniparser_getint(ini, (getDevID() + ":speaker").c_str(), 0);
    if (spk < 0) spk = 0;
    if (spk > 100) spk = 100;
    tone.speakerMix = (uint8_t)((spk * 255) / 100);

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
        case 2:   // 0xD2: direct 8255 port C write - PC0 is the audio mask
            tone.setAudioMask((dt & 0x01) != 0);
            break;
        case 3:   // 0xD3: 8255 control. Mode-set (bit 7) resets port C ->
                  // mask closes; BSR touches it only when it selects PC0
                  // (tape motor / interrupt mask BSRs must not)
            if (dt & 0x80) {
                tone.setAudioMask(false);
            } else if (((dt >> 1) & 0x07) == 0) {
                tone.setAudioMask((dt & 0x01) != 0);
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
    g_ctc_diag.ioToneState = (tone.audioMask ? 1 : 0) | (tone.outLevel ? 2 : 0) |
                             (tone.running ? 4 : 0) | (tone.gate0 ? 8 : 0) |
                             ((tone.mode & 7) << 4);
    g_ctc_diag.ioToneReload = tone.reloadValue;

    uint32_t head = ioqHead;   // snapshot; core1 keeps appending
    bool have = head != ioqTail;
    uint32_t newestTs = have ? ioq[(head - 1) & (IOQ_SIZE - 1)].ts : 0;
    timeline.beginBuffer(newestTs, have);

    while (ioqTail != head) {
        uint32_t i = ioqTail & (IOQ_SIZE - 1);
        uint32_t ts = ioq[i].ts;
        // Leave events beyond this buffer's window queued for the next one
        if (!timeline.accepts(ts)) break;

        g_ctc_diag.ioTotal++;
        g_ctc_diag.ioPort[ioq[i].port & 0x07]++;
        uint32_t lp = (g_ctc_diag.lastIoPos++ & 31) * 2;
        g_ctc_diag.lastIo[lp] = ioq[i].port;
        g_ctc_diag.lastIo[lp + 1] = ioq[i].data;

        if ((ioq[i].port & 0x07) == 2) {
            bool high = (ioq[i].data & 0x01) != 0;
            if (high && !g_ctc_diag.d2High) {
                uint32_t period = ts - g_ctc_diag.d2RiseTs;
                g_ctc_diag.d2Period[g_ctc_diag.d2PeriodPos++ & 7] =
                    (period > 0xFFFF) ? 0xFFFF : (uint16_t)period;
                g_ctc_diag.d2RiseTs = ts;
            } else if (!high && g_ctc_diag.d2High) {
                uint32_t width = ts - g_ctc_diag.d2RiseTs;
                g_ctc_diag.d2Width[g_ctc_diag.d2WidthPos++ & 7] =
                    (width > 0xFFFF) ? 0xFFFF : (uint16_t)width;
            }
            g_ctc_diag.d2High = high;
        }

        if (!timeline.push(ts, ioq[i].port, ioq[i].data)) {
            applyWrite(ioq[i].port, ioq[i].data);   // overflow: coarse > dropped
        }
        ioqTail++;
    }

    g_ctc_diag.ioPushed = timeline.pushedTotal;
    g_ctc_diag.ioNegClamped = timeline.negClamped;
    g_ctc_diag.ioBacklogUs = timeline.lastBacklogUs;
    for (int i = 0; i < 8; i++) g_ctc_diag.ioLastRel[i] = timeline.lastRel[i];
}

void CTCDevice::renderSample(int16_t& left, int16_t& right) {
    // Apply due writes at their cycle offset WITHIN the sample: pulse-train
    // engines make pulses narrower than one sample, which must contribute
    // their area rather than collapse to the final state
    uint32_t cycles = tone.beginSample();
    timeline.applyDue(cycles, [this](uint8_t port, uint8_t data, uint32_t cyc) {
        tone.advanceTo(cyc);
        applyWrite(port, data);
    });

    int32_t amplitude = (CTC_BASE_AMPLITUDE * volume) / 100;
    int32_t sample = tone.finishSample(amplitude);

    left = static_cast<int16_t>((sample * (int32_t)(256 - pan256)) >> 8);
    right = static_cast<int16_t>((sample * (int32_t)pan256) >> 8);
}
