#include "common.hpp"

#ifdef BOARD_DELUXE

#include "ctc.hpp"
#include "mem_snoop.hpp"
#include "pico/time.h"

REGISTER_MZ_DEVICE(CTCDevice)

// Write-listener port order (positional match with writeMappings):
// D0-D7, then the bank ports
static constexpr uint8_t PORT_BANK_E1 = 0xE1;  // DRAM over D000-FFFF: peripherals unmapped
static constexpr uint8_t PORT_BANK_E3 = 0xE3;  // peripherals + upper ROM mapped back
static constexpr uint8_t PORT_BANK_E4 = 0xE4;  // power-on map: peripherals mapped, lock cleared
static constexpr uint8_t PORT_BANK_E5 = 0xE5;  // prohibit upper area
static constexpr uint8_t PORT_BANK_E6 = 0xE6;  // return (unlock) upper area

CTCDevice::CTCDevice() {
    for (int i = 0; i < 8; i++) {
        writeMappings[i].fn = nullptr;
    }
    writeMappings[2].fn = CTCDevice::writePort;   // D2: 8255 port C data
    writeMappings[3].fn = CTCDevice::writePort;   // D3: 8255 control (BSR/mode)
    writeMappings[4].fn = CTCDevice::writePort;   // D4: 8253 counter 0
    writeMappings[7].fn = CTCDevice::writePort;   // D7: 8253 control
    for (int i = 8; i < 13; i++) {
        writeMappings[i].fn = CTCDevice::writeBankPort;   // E1/E3/E4/E5/E6
    }

    // Initialize port mappings with defaults
    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);

    ioqHead = 0;
    ioqTail = 0;
    bankLogHead = 0;
    bankLogTail = 0;

    // The peripheral window is mapped in the power-on bank configuration
    periMapped = true;
    periLocked = false;

    cursorValid = false;
    cursor = 0;

    volume = 20;
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
    ports.push_back(PORT_BANK_E1);
    ports.push_back(PORT_BANK_E3);
    ports.push_back(PORT_BANK_E4);
    ports.push_back(PORT_BANK_E5);
    ports.push_back(PORT_BANK_E6);
    return ports;
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> CTCDevice::applyBasePort(uint8_t) const {
    return {getReadPorts(), getWritePorts()};
}

int CTCDevice::readConfig(dictionary *ini) {
    if (!ini) return -1;

    // Default matches the PSG's masterVolume default, so a bare ini has
    // both sound sources balanced out of the box
    int vol = iniparser_getint(ini, (getDevID() + ":volume").c_str(), 20);
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

// ---- core1: enqueue with a 1MHz-timer timestamp; decoding and the tone
// ---- model run on core0 at the write's scheduled sample position

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

// The bank handler only logs (ring position, port); the consumer applies
// the semantics in stream order. SPSC: fields first, head last.
RAM_FUNC int CTCDevice::writeBankPort(MZDevice* self, uint8_t port, uint8_t, uint8_t) {
    auto* dev = static_cast<CTCDevice*>(self);
    uint32_t h = dev->bankLogHead;
    dev->bankLog[h & (BANK_LOG_SIZE - 1)].idx = mem_snoop_cursor_inline();
    dev->bankLog[h & (BANK_LOG_SIZE - 1)].port = port;
    dev->bankLogHead = h + 1;
    return 0;
}

// ---- core0: consumption and rendering ----

void CTCDevice::applyBankEvent(uint8_t port) {
    switch (port) {
        case PORT_BANK_E1: periMapped = false; break;
        case PORT_BANK_E3:
        case PORT_BANK_E4: periMapped = true; periLocked = false; break;
        case PORT_BANK_E5: periLocked = true; break;
        case PORT_BANK_E6: periLocked = false; break;
        default: break;
    }
}

void CTCDevice::applyWrite(uint8_t a, uint8_t dt) {
    switch (a) {
        // I/O path (0xD0-0xD7)
        case 0xD2:   // direct 8255 port C write - PC0 is the audio mask
            tone.setAudioMask((dt & 0x01) != 0);
            break;
        case 0xD3:   // 8255 control: mode-set (bit 7) resets port C ->
                     // mask closes; BSR touches it only when it selects PC0
            if (dt & 0x80) {
                tone.setAudioMask(false);
            } else if (((dt >> 1) & 0x07) == 0) {
                tone.setAudioMask((dt & 0x01) != 0);
            }
            break;
        case 0xD4: tone.countWrite(dt); break;
        case 0xD7: tone.ctrlWrite(dt); break;

        // memory path (E00x low byte). The 8255 mask is deliberately not
        // tracked here (E002/E003): the monitor's 8255 init races the snoop
        // consumer coming up and mask state derived from partial history
        // killed the power-on beep; no observed software drives the mask
        // memory-mapped.
        case 0x04: tone.countWrite(dt); break;              // 8253 counter 0
        case 0x07: tone.ctrlWrite(dt); break;               // 8253 control word
        case 0x08: tone.setGate0((dt & 0x01) != 0); break;  // E008 latch -> GATE0 pin
        default: break;
    }
}

void CTCDevice::processWrites() {
    mem_snoop_service();

    uint32_t ioHead = ioqHead;   // snapshot; core1 keeps appending
    uint32_t bankHead = bankLogHead;
    uint32_t now = mem_snoop_cursor();

    if (!cursorValid) {
        // First scan: skip whatever accumulated before we were ready, but
        // apply the bank switches so the state is current
        cursor = now;
        cursorValid = true;
        while (bankLogTail != bankHead) {
            applyBankEvent(bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].port);
            bankLogTail++;
        }
        return;
    }

    const uint32_t mask = MEM_SNOOP_RING_WORDS - 1;
    uint32_t start = cursor;
    uint32_t memPending = (now - start) & mask;
    uint32_t memDone = 0;

    // Newest pending timestamp across both streams anchors the playhead
    bool haveIo = ioHead != ioqTail;
    bool haveMem = memPending != 0;
    uint32_t newestTs = 0;
    if (haveIo) newestTs = ioq[(ioHead - 1) & (IOQ_SIZE - 1)].ts;
    if (haveMem) {
        uint32_t t = mem_snoop_ts((start + memPending - 1) & mask);
        if (!haveIo || (int32_t)(t - newestTs) > 0) newestTs = t;
    }
    timeline.beginBuffer(newestTs, haveIo || haveMem);

    // Drain stale bank-log entries (recorded at positions long past);
    // half a ring behind is unambiguously the past
    while (bankLogTail != bankHead &&
           ((bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].idx - start) & mask) > MEM_SNOOP_RING_WORDS / 2) {
        applyBankEvent(bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].port);
        bankLogTail++;
    }

    // Merge both streams by timestamp onto the timeline; events beyond
    // this buffer's window stay in their queues for the next buffer
    while (true) {
        bool io = ioqTail != ioHead;
        bool mem = memDone < memPending;
        if (!io && !mem) break;

        uint32_t tsIo = io ? ioq[ioqTail & (IOQ_SIZE - 1)].ts : 0;
        uint32_t tsMem = mem ? mem_snoop_ts(cursor) : 0;
        bool takeMem = mem && (!io || (int32_t)(tsMem - tsIo) <= 0);
        uint32_t ts = takeMem ? tsMem : tsIo;

        if (!timeline.accepts(ts)) break;

        if (takeMem) {
            // Replay bank switches recorded at or before this position
            while (bankLogTail != bankHead &&
                   ((bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].idx - start) & mask) <= memDone) {
                applyBankEvent(bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].port);
                bankLogTail++;
            }

            uint32_t w = mem_snoop_read(cursor);
            cursor = (cursor + 1) & mask;
            memDone++;

            uint8_t high = (w >> 16) & 0xFF;
            if (high != 0xE0) continue;
            uint8_t low = (w >> 8) & 0xFF;
            if (low < 0x04 || low > 0x08) continue;
            if (!snoopActive()) continue;

            if (!timeline.push(ts, low, (w >> 24) & 0xFF)) {
                applyWrite(low, (w >> 24) & 0xFF);   // overflow: coarse > dropped
            }
        } else {
            uint32_t i = ioqTail & (IOQ_SIZE - 1);
            if (!timeline.push(ts, ioq[i].port, ioq[i].data)) {
                applyWrite(ioq[i].port, ioq[i].data);
            }
            ioqTail++;
        }
    }
}

void CTCDevice::renderSample(int16_t& left, int16_t& right) {
    // Apply due writes at their cycle offset WITHIN the sample - pulses
    // narrower than one sample must contribute their area, not collapse
    uint32_t cycles = tone.beginSample();
    timeline.applyDue(cycles, [this](uint8_t a, uint8_t dt, uint32_t cyc) {
        tone.advanceTo(cyc);
        applyWrite(a, dt);
    });

    int32_t amplitude = (CTC_BASE_AMPLITUDE * volume) / 100;
    int32_t sample = tone.finishSample(amplitude);

    left = static_cast<int16_t>((sample * (int32_t)(256 - pan256)) >> 8);
    right = static_cast<int16_t>((sample * (int32_t)pan256) >> 8);
}

#endif // BOARD_DELUXE
