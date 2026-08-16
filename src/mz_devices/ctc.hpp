#pragma once

#include <stdint.h>
#include "mz_devices.hpp"
#include "i2s_audio.hpp"
#include "pit8253_tone.hpp"
#include "common.hpp"

constexpr uint8_t CTC_PORT_BASE = 0xD0;
constexpr const char CTC_ID[] = "ctc";
constexpr bool CTC_EXWAIT = false;

constexpr int16_t CTC_BASE_AMPLITUDE = 20000;

// The MZ-800 beeper: 8253 counter 0 + E008 GATE0 latch + 8255 PC0 audio
// mask, fed from BOTH access paths into one tone model:
// - I/O-mapped (0xD0-0xD7, MZ-800 mode): timestamped by the core1 write
//   handler into an SPSC queue.
// - memory-mapped (0xE004-0xE008, both machine modes - the peripheral
//   window overlays the ROM whenever the default bank is mapped): consumed
//   from the hardware-timestamped memory-write snoop (see mem_snoop.hpp),
//   gated by bank tracking (I/O 0xE1 unmaps DRAM over the window,
//   0xE3/0xE4 map it back, 0xE5/0xE6 lock/unlock) merged by CAPTURE
//   TIMESTAMP with the memory events they classify. Time is absolute:
//   ring-position replay was tried first and corrupted the bank state
//   whenever the consumer backlog crossed half the ring or the ring
//   lapped between scans (field failure: BEEP's E3..E008..E1 bracket
//   collapsed to net-closed, gate-opens discarded, beeper dead).
// Both streams merge by timestamp onto a jitter-free playhead and apply at
// sub-sample offsets during rendering. Purely a watcher on the memory
// side; nothing is ever driven onto the bus. Deluxe only.
class CTCDevice final : public MZDevice, public I2SAudioSource {
public:
    CTCDevice();
    ~CTCDevice();

    int init() override;
    int isInterrupt() override { return 0; }
    bool needsExwait() const override { return CTC_EXWAIT; }
    // Needs the Deluxe board's I2S output and memory-write snoop SM
    bool supportedOnBoard() const override {
        #ifdef BOARD_DELUXE
        return true;
        #else
        return false;
        #endif
    }
    std::vector<uint8_t> getReadPorts() const override;
    std::vector<uint8_t> getWritePorts() const override;
    // Fixed machine addresses; base_port remapping makes no sense here
    std::pair<std::vector<uint8_t>, std::vector<uint8_t>> applyBasePort(uint8_t basePort) const override;
    int readConfig(dictionary *ini) override;
    int flush() override { return 0; }
    // Unlike the 8253/PSG (no reset pins), the memory MAPPER resets to the
    // power-on map on /RESET - mirror exactly that. Tone state, queues and
    // the playhead deliberately stay untouched. The actual state lives on
    // core 0, so this only raises a flag consumed by processWrites().
    void softReset() override { bankResetPending = true; }
    static std::string getDevType() { return CTC_ID; }

    RAM_FUNC static int writePort(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeBankPort(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);

    void processWrites() override;
    void renderSample(int16_t& left, int16_t& right) override;

private:
    // a is 0xD0-0xD7 (I/O port) or 0x04-0x08 (memory E00x low byte);
    // the value spaces are disjoint
    void applyWrite(uint8_t a, uint8_t dt);
    void applyBankEvent(uint8_t port, uint8_t data);

    // ---- I/O path staging (core1 producer, core0 consumer) ----
    // Sized for pulse-train engines: a tight Z80 OUT loop can emit up to
    // ~100k writes/s, ~300 per audio buffer - 512 keeps full headroom
    // while saving 4 KB on the RAM-tight W builds
    struct IoWrite {
        uint32_t ts;
        uint8_t port;
        uint8_t data;
    };
    static constexpr uint32_t IOQ_SIZE = 512;   // power of two
    volatile IoWrite ioq[IOQ_SIZE];
    volatile uint32_t ioqHead;   // core1
    uint32_t ioqTail;            // core0

    // ---- memory path staging ----
    // Bank switches must be applied to the snooped stream at the MOMENT
    // they occurred: BASIC banks RAM over the peripheral window around
    // every E008 access, so consuming stale flags misclassifies both ways.
    // Ordering is by capture timestamp (core1 stamps the OUT within ~1us;
    // the Z80 spaces the OUT and the bracketed memory write by >=~10us, and
    // an EXWAIT-frozen Z80 can't emit either, so stamps can't misorder).
    struct BankEvent {
        uint32_t ts;     // time_us_32() at capture
        uint8_t port;
        uint8_t data;    // only the GDG DMD write (0xCE) consumes it
    };
    static constexpr uint32_t BANK_LOG_SIZE = 32;   // power of two
    volatile BankEvent bankLog[BANK_LOG_SIZE];
    volatile uint32_t bankLogHead;   // core1
    uint32_t bankLogTail;            // core0

    // Consumer-side bank + machine-mode state (core0 only).
    // GATE0 truth (per mz800emu ctc82530_on_regDMD_changed): the E008
    // latch drives the gate ONLY in MZ-700 mode; native MZ-800 mode forces
    // it OPEN. The mapped peripherals at E000-E00F likewise exist only in
    // 700 mode - E00x memory writes in native mode reach nothing.
    bool periMapped;
    bool periLocked;
    bool mode700;      // GDG DMD bit 3 (port 0xCE); reset state = 700 mode
    bool gateLatch;    // E008 bit 0 - retained across mode switches
    bool snoopActive() const { return mode700 && periMapped && !periLocked; }
    // Z80 soft reset: core1 raises, core0 applies at the next scan
    volatile bool bankResetPending;

    // Snoop ring read cursor + the last consumed memory timestamp (the
    // stream is time-ordered by construction; a backward step means the
    // ring lapped us / the mod arithmetic aliased -> resync)
    bool cursorValid;
    uint32_t cursor;
    uint32_t lastMemTs;

    ToneTimeline timeline;
    Pit8253Tone tone;

    uint8_t volume;
    uint8_t pan;
    uint16_t pan256;
};
