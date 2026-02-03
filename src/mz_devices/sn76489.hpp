#pragma once

#include <stdint.h>
#include "mz_devices.hpp"
#include "common.hpp"
#include "i2s_audio.hpp"

constexpr uint8_t SN76489_PORT = 0xf2;
constexpr const char SN76489_ID[] = "psg";
constexpr bool SN76489_EXWAIT = false;

constexpr uint8_t WRITE_QUEUE_SIZE = 64;     // Must be power of 2 for fast wrapping

// SN76489 chip constants
// Run the PSG at 1/16th of the nominal PAL clock to reduce cycle overhead
constexpr uint32_t SN76489_CLOCK = 3546900 / 16;  // 3.5469 MHz / 16
constexpr uint8_t SN76489_CHANNELS = 4;      // 3 tone + 1 noise

class SN76489Device final : public MZDevice, public I2SAudioSource {
public:
    SN76489Device();
    ~SN76489Device();
    
    int init() override;
    int isInterrupt() override { return 0; }
    bool needsExwait() const override { return SN76489_EXWAIT; }
    uint8_t getDefaultBasePort() const override { return SN76489_PORT; }
    int readConfig(dictionary *ini) override;
    int flush() override;
    static std::string getDevType() { return SN76489_ID; }

    // Port write handler
    RAM_FUNC static int writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr);
    
    // Audio source implementation
    void renderSample(int16_t& left, int16_t& right) override;
    void processWrites() override;
    
private:
    // SN76489 internal state
    struct Channel {
        uint16_t frequency;      // 10-bit frequency value
        uint16_t counter;        // Countdown timer
        uint8_t volume;          // 4-bit attenuation (0=max, 15=mute)
        bool output;             // Current output state
    };
    
    struct NoiseChannel {
        uint16_t frequency;      // Derived from tone channel or preset
        uint16_t counter;        // Countdown timer
        uint16_t lfsr;           // Linear feedback shift register (15-bit)
        uint8_t volume;          // 4-bit attenuation (0=max, 15=mute)
        uint8_t mode;            // 0=periodic, 1=white noise
        uint8_t shift_rate;      // Noise frequency control (0-3)
        bool output;             // Current LFSR bit 0
    };
    
    Channel toneChannels[3];
    NoiseChannel noiseChannel;
    uint8_t latchedChannel;         // Currently latched channel (0-3)
    bool latchedIsVolume;           // true=volume, false=frequency
    uint8_t channelPan[4];          // Panning 0-100 for each channel (0=left, 50=center, 100=right)
    uint16_t pan256[4];             // Panning scaled to 0-256 for fast mixing
    uint8_t masterVolume;           // Master volume 0-100 (default 100)
    
    // Lock-free write queue (core1 → core0)
    volatile uint8_t writeQueue[WRITE_QUEUE_SIZE];
    volatile uint8_t writeQueueHead;            // Written by core1
    volatile uint8_t writeQueueTail;            // Written by core0
    
    // Chip clock tracking (integer residual, no floats)
    uint32_t chipCycleResid;                    // Residual chip cycles between output samples
    
    // Volume table (4-bit attenuation → linear amplitude, pre-scaled for 4-channel mix)
    static const int16_t volumeTable[16];
    
    // SN76489 register processing
    void writeRegister(uint8_t data);
    
    // Chip emulation
    void clockTick();                           // Advance all channels by 1 chip cycle
    void captureStereoSample(int16_t& left, int16_t& right);  // Mix channels with panning
    void updateToneChannel(Channel& ch);
    void updateNoiseChannel();
    
    // Buffer management
};
