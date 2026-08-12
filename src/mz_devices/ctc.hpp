#pragma once

#include <stdint.h>
#include "mz_devices.hpp"
#include "i2s_audio.hpp"
#include "common.hpp"

constexpr uint8_t CTC_PORT_BASE = 0xD0;
constexpr const char CTC_ID[] = "ctc";
constexpr bool CTC_EXWAIT = false;

constexpr uint32_t CTC_INPUT_CLOCK = 1108590; // counter 0 clock, 1.10859 MHz
constexpr int16_t CTC_BASE_AMPLITUDE = 20000;

// Emulates the MZ-800 beeper: 8253 counter 0 (square wave, mode 3) gated by
// 8255 PC0. Only reachable in MZ-800 mode, where the 8255/8253 are
// I/O-mapped at 0xD0-0xD7; in MZ-700 mode they are memory-mapped at 0xE00x
// and invisible to the bus capture.
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

    RAM_FUNC static int writePortC(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writePortCtrl(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeCounter0(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeCounterCtrl(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);

    void renderSample(int16_t& left, int16_t& right) override;

private:
    enum class LoadMode : uint8_t {
        None = 0,
        LSB,
        MSB,
        LSB_MSB
    };

    void applyReload(uint16_t value);

    // 8255 PC0 = sound gate; one bit, written via BSR commands on the 8255
    // control port (0xD3) or a direct port C write (0xD2)
    volatile bool gateOpen;
    // A control word halts counter 0 until a full count is loaded (the ROM
    // uses "control word + gate" with no count as its silencing idiom)
    volatile bool counterRunning;
    volatile bool squareWave;      // counter 0 programmed to mode 3
    volatile uint32_t reloadValue;

    uint32_t counter;
    bool outputHigh;
    uint32_t cycleResid;
    uint8_t volume;
    uint8_t pan;
    uint16_t pan256;

    LoadMode loadMode;
    bool waitingMsb;
    uint8_t latchedLsb;
};
