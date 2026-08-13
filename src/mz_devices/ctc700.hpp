#pragma once

#include <stdint.h>
#include "mz_devices.hpp"
#include "i2s_audio.hpp"
#include "common.hpp"

// NOTE: ini section names map to device types with trailing digits
// stripped ([fdc1] -> fdc), so this type name must not end in a digit -
// "ctc700" would be unreachable (strips to "ctc").
constexpr const char CTC700_ID[] = "melody";

constexpr uint32_t CTC700_INPUT_CLOCK = 1108590; // counter 0 clock, 1.10859 MHz
constexpr int16_t CTC700_BASE_AMPLITUDE = 20000;

// Memory-mapped melody: the 8253 at 0xE004-0xE007 and the melody gate
// latch at 0xE008 (1Z-013B MLDST/MLDSP protocol) are memory-mapped in
// BOTH machine modes whenever the default bank is in place - the 9Z-504M
// ROM pads 0xE000-0xE00F with filler because the peripheral window
// overlays it. Memory writes never assert IORQ, so this device consumes
// the memory-write snoop stream (see mem_snoop.hpp) on core0 and renders
// the tone, watching only; nothing is ever driven onto the bus. Its I/O
// write listeners track the bank map (0xE1 unmaps DRAM over the window,
// 0xE3/0xE4 map it back, 0xE5/0xE6 lock/unlock) so RAM writes to the same
// addresses don't beep. Deluxe only.
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

    void processWrites() override;   // core0: scan the snoop ring
    void renderSample(int16_t& left, int16_t& right) override;

private:
    void handlePeripheralWrite(uint8_t low, uint8_t data);
    void writeCounterCtrl(uint8_t dt);
    void writeCounter0(uint8_t dt);
    void applyReload(uint16_t value);

    // Bank state, written by core1 listeners
    volatile bool periMapped;
    volatile bool periLocked;
    bool forceActive;   // ini "force": bypass bank gating (diagnostic)
    bool snoopActive() const { return forceActive || (periMapped && !periLocked); }

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

    uint8_t volume;
    uint8_t pan;
    uint16_t pan256;
};
