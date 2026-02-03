#pragma once

#include <stdint.h>

constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr uint32_t AUDIO_BUFFER_SIZE = 256; // Total samples in buffer (stereo: 128 L + 128 R)
constexpr uint8_t I2S_DATA_PIN = 13;
constexpr uint8_t I2S_BCLK_PIN = 12;
constexpr uint8_t I2S_LRCLK_PIN = 14;
constexpr uint8_t MAX_AUDIO_SOURCES = 4;

class I2SAudioSource {
public:
    virtual ~I2SAudioSource() = default;
    virtual void renderSample(int16_t& left, int16_t& right) = 0;
    virtual void processWrites() {}
};

int i2s_audio_register_source(I2SAudioSource* source);
void i2s_audio_unregister_source(I2SAudioSource* source);
int i2s_audio_init_on_core0();
void i2s_audio_shutdown();
bool i2s_audio_is_ready();
bool i2s_audio_has_sources();
void i2s_audio_poll();
