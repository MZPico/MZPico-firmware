#pragma once

#include <stdint.h>
#include "mz_devices.hpp"
#include "i2s_audio.hpp"
#include "common.hpp"

constexpr uint8_t CTC_PORT_BASE = 0xD0;
constexpr const char CTC_ID[] = "ctc";
constexpr bool CTC_EXWAIT = false;

constexpr uint32_t CTC_INPUT_CLOCK = 1100000; // 1.1 MHz
constexpr int16_t CTC_BASE_AMPLITUDE = 20000;

class CTCDevice final : public MZDevice, public I2SAudioSource {
public:
    CTCDevice();
    ~CTCDevice();

    int init() override;
    int isInterrupt() override { return 0; }
    bool needsExwait() const override { return CTC_EXWAIT; }
    uint8_t getDefaultBasePort() const override { return CTC_PORT_BASE; }
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

    volatile bool gateEnabled;
    volatile bool pcAllowsOutput;
    volatile uint32_t reloadValue;

    uint32_t counter;
    bool outputHigh;
    uint32_t cycleResid;
    uint8_t volume;

    LoadMode loadMode;
    bool waitingMsb;
    uint8_t latchedLsb;
};
