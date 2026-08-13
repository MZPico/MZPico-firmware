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

// Emulates the MZ-800 beeper over the I/O-mapped path: 8253 counter 0
// (0xD4/0xD7) gated by 8255 PC0 (BSR on 0xD3, direct port C on 0xD2),
// reachable in MZ-800 mode where the 8255/8253 are I/O-mapped at
// 0xD0-0xD7. The shared Pit8253Tone model also renders 1-bit beeper
// engines (control-word/gate toggling, mode-0 PWM). The memory-mapped
// path (both modes, 0xE00x) is covered by the melody device.
class CTCDevice final : public MZDevice, public I2SAudioSource {
public:
    CTCDevice();
    ~CTCDevice();

    int init() override;
    int isInterrupt() override { return 0; }
    bool needsExwait() const override { return CTC_EXWAIT; }
    std::vector<uint8_t> getReadPorts() const override;
    std::vector<uint8_t> getWritePorts() const override;
    int readConfig(dictionary *ini) override;
    int flush() override { return 0; }
    static std::string getDevType() { return CTC_ID; }

    RAM_FUNC static int writePort(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);

    void processWrites() override;
    void renderSample(int16_t& left, int16_t& right) override;

private:
    void applyWrite(uint8_t port, uint8_t dt);

    // Timestamped SPSC write queue (core1 producer, core0 consumer): I/O
    // writes are scheduled onto exact sample positions via ToneTimeline,
    // same as the memory-snoop path - applying them at receive-vs-render
    // time collapses intra-buffer edges and 1-bit engines degrade to
    // popping.
    struct IoWrite {
        uint32_t ts;
        uint8_t port;
        uint8_t data;
    };
    static constexpr uint32_t IOQ_SIZE = 256;   // power of two
    volatile IoWrite ioq[IOQ_SIZE];
    volatile uint32_t ioqHead;   // core1
    uint32_t ioqTail;            // core0

    ToneTimeline timeline;
    Pit8253Tone tone;

    uint8_t volume;
    uint8_t pan;
    uint16_t pan256;
};
