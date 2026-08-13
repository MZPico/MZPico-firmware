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
//   0xE3/0xE4 map it back, 0xE5/0xE6 lock/unlock) replayed at stream
//   positions so stale flags never misclassify.
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
    std::vector<uint8_t> getReadPorts() const override;
    std::vector<uint8_t> getWritePorts() const override;
    // Fixed machine addresses; base_port remapping makes no sense here
    std::pair<std::vector<uint8_t>, std::vector<uint8_t>> applyBasePort(uint8_t basePort) const override;
    int readConfig(dictionary *ini) override;
    int flush() override { return 0; }
    static std::string getDevType() { return CTC_ID; }

    RAM_FUNC static int writePort(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeBankPort(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);

    void processWrites() override;
    void renderSample(int16_t& left, int16_t& right) override;

private:
    // a is 0xD0-0xD7 (I/O port) or 0x04-0x08 (memory E00x low byte);
    // the value spaces are disjoint
    void applyWrite(uint8_t a, uint8_t dt);
    void applyBankEvent(uint8_t port);

    // ---- I/O path staging (core1 producer, core0 consumer) ----
    // Sized for pulse-train engines: a tight Z80 OUT loop can emit up to
    // ~100k writes/s, ~300 per audio buffer
    struct IoWrite {
        uint32_t ts;
        uint8_t port;
        uint8_t data;
    };
    static constexpr uint32_t IOQ_SIZE = 1024;   // power of two
    volatile IoWrite ioq[IOQ_SIZE];
    volatile uint32_t ioqHead;   // core1
    uint32_t ioqTail;            // core0

    // ---- memory path staging ----
    // Bank switches must be applied to the snooped stream AT THE POSITION
    // they occurred: S-BASIC banks RAM over the peripheral window around
    // every access, so consuming stale flags misclassifies both ways.
    struct BankEvent {
        uint32_t idx;
        uint8_t port;
    };
    static constexpr uint32_t BANK_LOG_SIZE = 32;   // power of two
    volatile BankEvent bankLog[BANK_LOG_SIZE];
    volatile uint32_t bankLogHead;   // core1
    uint32_t bankLogTail;            // core0

    // Consumer-side bank state (core0 only)
    bool periMapped;
    bool periLocked;
    bool snoopActive() const { return periMapped && !periLocked; }

    // Snoop ring read cursor
    bool cursorValid;
    uint32_t cursor;

    ToneTimeline timeline;
    Pit8253Tone tone;

    uint8_t volume;
    uint8_t pan;
    uint16_t pan256;
};
