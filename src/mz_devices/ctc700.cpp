#include "common.hpp"

#ifdef BOARD_DELUXE

#include "ctc700.hpp"
#include "mem_snoop.hpp"
#include "ctc_diag.hpp"

REGISTER_MZ_DEVICE(CTC700Device)

// Write-listener port order (positional match with writeMappings)
static constexpr uint8_t PORT_BANK_E1 = 0xE1;  // DRAM over D000-FFFF: peripherals unmapped
static constexpr uint8_t PORT_BANK_E3 = 0xE3;  // peripherals + upper ROM mapped back
static constexpr uint8_t PORT_BANK_E4 = 0xE4;  // power-on map: peripherals mapped, lock cleared
static constexpr uint8_t PORT_BANK_E5 = 0xE5;  // prohibit upper area
static constexpr uint8_t PORT_BANK_E6 = 0xE6;  // return (unlock) upper area

CTC700Device::CTC700Device() {
    for (uint8_t i = 0; i < 5; ++i) {
        writeMappings[i].fn = CTC700Device::writeBankPort;   // E1/E3/E4/E5/E6
    }

    auto readPorts = getReadPorts();
    auto writePorts = getWritePorts();
    initializePortMappings(readPorts, writePorts);

    // The peripheral window is mapped in the power-on bank configuration
    periMapped = true;
    periLocked = false;
    forceActive = false;
    bankLogHead = 0;
    bankLogTail = 0;

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
// The handler only logs (ring position, port); the consumer applies the
// semantics in stream order. The log is SPSC: fields first, head last.

RAM_FUNC int CTC700Device::writeBankPort(MZDevice* self, uint8_t port, uint8_t, uint8_t) {
    auto* dev = static_cast<CTC700Device*>(self);
    uint32_t h = dev->bankLogHead;
    dev->bankLog[h & (BANK_LOG_SIZE - 1)].idx = mem_snoop_cursor_inline();
    dev->bankLog[h & (BANK_LOG_SIZE - 1)].port = port;
    dev->bankLogHead = h + 1;
    return 0;
}

// ---- core0: snoop ring consumer ----

void CTC700Device::applyBankEvent(uint8_t port) {
    switch (port) {
        case 0xE1: periMapped = false; g_ctc_diag.bank[0]++; break;
        case 0xE3: periMapped = true; periLocked = false; g_ctc_diag.bank[1]++; break;
        case 0xE4: periMapped = true; periLocked = false; g_ctc_diag.bank[2]++; break;
        case 0xE5: periLocked = true; g_ctc_diag.bank[3]++; break;
        case 0xE6: periLocked = false; g_ctc_diag.bank[4]++; break;
        default: break;
    }
}

void CTC700Device::processWrites() {
    mem_snoop_service();

    uint32_t now = mem_snoop_cursor();
    uint32_t head = bankLogHead;
    if (!cursorValid) {
        // First scan: skip whatever accumulated before we were ready, but
        // apply the bank switches so the state is current
        cursor = now;
        cursorValid = true;
        while (bankLogTail != head) {
            applyBankEvent(bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].port);
            bankLogTail++;
        }
        return;
    }

    const uint32_t mask = MEM_SNOOP_RING_WORDS - 1;
    uint32_t start = cursor;
    uint32_t pending = (now - start) & mask;

    uint32_t newestTs = pending ? mem_snoop_ts((start + pending - 1) & mask) : 0;
    timeline.beginBuffer(newestTs, pending != 0);

    // Drain stale bank-log entries (recorded at positions long past -
    // possible when a scan raced the log write); half a ring behind is
    // unambiguously the past
    while (bankLogTail != head &&
           ((bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].idx - start) & mask) > MEM_SNOOP_RING_WORDS / 2) {
        applyBankEvent(bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].port);
        bankLogTail++;
    }

    for (uint32_t d = 0; d < pending; d++) {
        uint32_t idx = cursor;
        uint32_t ts = mem_snoop_ts(idx);
        // Events beyond this buffer's window stay in the ring for the next
        // buffer - consuming them early would clump their edges
        if (!timeline.accepts(ts)) break;

        // Replay bank switches recorded at or before this stream position
        while (bankLogTail != head &&
               ((bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].idx - start) & mask) <= d) {
            applyBankEvent(bankLog[bankLogTail & (BANK_LOG_SIZE - 1)].port);
            bankLogTail++;
        }

        uint32_t w = mem_snoop_read(idx);
        cursor = (cursor + 1) & mask;

        g_ctc_diag.memTotal++;
        uint8_t high = (w >> 16) & 0xFF;
        if (high != 0xE0) continue;
        g_ctc_diag.memE0Page++;
        uint8_t low = (w >> 8) & 0xFF;
        if (low < 16) g_ctc_diag.memLow[low]++;
        if (low < 0x04 || low > 0x08) continue;
        if (!snoopActive()) {
            g_ctc_diag.memRejected++;
            continue;
        }

        uint8_t data = (w >> 24) & 0xFF;
        uint32_t lp = (g_ctc_diag.lastMemPos++ & 31) * 2;
        g_ctc_diag.lastMem[lp] = low;
        g_ctc_diag.lastMem[lp + 1] = data;

        if (!timeline.push(ts, low, data)) {
            handlePeripheralWrite(low, data);   // overflow: coarse > dropped
        }
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
    // Apply due writes at their cycle offset WITHIN the sample - pulses
    // narrower than one sample must contribute their area, not collapse
    // to the final state. Not gated on the bank state: unmapping the
    // window only blocks WRITES (handled in processWrites); a tone
    // already playing keeps sounding on real hardware.
    uint32_t cycles = tone.beginSample();
    timeline.applyDue(cycles, [this](uint8_t low, uint8_t data, uint32_t cyc) {
        tone.advanceTo(cyc);
        handlePeripheralWrite(low, data);
    });

    int32_t amplitude = (CTC700_BASE_AMPLITUDE * volume) / 100;
    int32_t sample = tone.finishSample(amplitude);

    left = static_cast<int16_t>((sample * (int32_t)(256 - pan256)) >> 8);
    right = static_cast<int16_t>((sample * (int32_t)pan256) >> 8);
}

#endif // BOARD_DELUXE
