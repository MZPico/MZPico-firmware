#pragma once

#include <stdint.h>
#include "mz_devices.hpp"
#include "i2s_audio.hpp"
#include "common.hpp"

constexpr const char CTC700_ID[] = "ctc700";

constexpr uint32_t CTC700_INPUT_CLOCK = 1108590; // counter 0 clock, 1.10859 MHz
constexpr int16_t CTC700_BASE_AMPLITUDE = 20000;

// MZ-700-mode melody: in MZ-700 mode the 8253 is memory-mapped at
// 0xE004-0xE007 with the melody gate latch at 0xE008 (1Z-013B MLDST/MLDSP)
// - invisible to I/O capture. This device consumes the memory-write snoop
// stream (see mem_snoop.hpp) on core0 and renders the tone, watching only;
// nothing is ever driven onto the bus. Its I/O write listeners track the
// bank map (0xE1/E3/E4/E5/E6) and display mode (0xCE DMD) so RAM writes to
// the same addresses in other configurations don't beep. Deluxe only.
class CTC700Device final : public MZDevice, public I2SAudioSource {
public:
    CTC700Device();
    ~CTC700Device();

    int init() override;
    int isInterrupt() override { return 0; }
    bool needsExwait() const override { return false; }
    std::vector<uint8_t> getReadPorts() const override;
    std::vector<uint8_t> getWritePorts() const override;
    // Fixed system ports; base_port remapping makes no sense here
    std::pair<std::vector<uint8_t>, std::vector<uint8_t>> applyBasePort(uint8_t basePort) const override;
    int readConfig(dictionary *ini) override;
    int flush() override { return 0; }
    static std::string getDevType() { return CTC700_ID; }

    RAM_FUNC static int writeBankUnmap(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeBankMap(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeBankLock(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeBankUnlock(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    RAM_FUNC static int writeDmd(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);

    // Diagnostic reads (0xDE/0xDF): snoop DMA cursor and E0-page event
    // count, so a Z80-side test can localize a dead capture stage
    RAM_FUNC static int readDiagCursor(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);
    RAM_FUNC static int readDiagEvents(MZDevice* self, uint8_t port, uint8_t* dt, uint8_t high_addr);

    void processWrites() override;   // core0: scan the snoop ring
    void renderSample(int16_t& left, int16_t& right) override;

private:
    void handlePeripheralWrite(uint8_t low, uint8_t data);
    void writeCounterCtrl(uint8_t dt);
    void writeCounter0(uint8_t dt);
    void applyReload(uint16_t value);

    // Bank/mode state, written by core1 listeners
    volatile bool mz700Mode;
    volatile bool periMapped;
    volatile bool periLocked;
    bool forceActive;   // ini "force": bypass mode/bank gating (diagnostic)
    bool snoopActive() const { return forceActive || (mz700Mode && periMapped && !periLocked); }

    // 8253 counter 0 model (same semantics as the I/O-mode CTC device);
    // written and read on core0 only, so no cross-core concerns
    enum class LoadMode : uint8_t { None = 0, LSB, MSB, LSB_MSB };
    bool gateOpen;          // E008 latch bit 0
    bool counterRunning;
    bool squareWave;
    uint32_t reloadValue;
    uint32_t counter;
    bool outputHigh;
    uint32_t cycleResid;
    LoadMode loadMode;
    bool waitingMsb;
    uint8_t latchedLsb;

    // Snoop ring read cursor
    bool cursorValid;
    uint32_t cursor;

    // Diagnostics
    int snoopDmaCh;
    volatile uint32_t e0Events;

    uint8_t volume;
    uint8_t pan;
    uint16_t pan256;
};
