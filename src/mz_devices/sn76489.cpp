#include <cstring>
#include <cmath>
#include "sn76489.hpp"
#include "common.hpp"
#include "i2s_audio.hpp"

REGISTER_MZ_DEVICE(SN76489Device)

// Core0: Audio (DMA, PIO, I2S)
// Core1: Bus monitoring (port writes)

// Volume table: 2dB steps, scaled so 4 channels at max sum near full-scale (32767)
const int16_t SN76489Device::volumeTable[16] = {
    8192, 6508, 5172, 4108, 3264, 2592, 2060, 1636,
    1300, 1032, 820, 652, 516, 412, 328, 0
};

SN76489Device::SN76489Device() {
    writeMappings[0].fn = SN76489Device::writeData;
    readPortCount = 0;
    writePortCount = 1;
    
    for (int i = 0; i < 3; i++) {
        toneChannels[i].frequency = 0;
        toneChannels[i].counter = 0x400;
        toneChannels[i].volume = 15;
        toneChannels[i].output = false;
    }
    
    noiseChannel.frequency = 0x10;
    noiseChannel.counter = 0x10;
    noiseChannel.lfsr = 0x8000;
    noiseChannel.volume = 15;
    noiseChannel.mode = 0;
    noiseChannel.shift_rate = 0;
    noiseChannel.output = false;
    
    latchedChannel = 0;
    latchedIsVolume = false;
    
    channelPan[0] = 20;
    channelPan[1] = 80;
    channelPan[2] = 50;
    channelPan[3] = 50;
    masterVolume = 100;
    for (int i = 0; i < 4; i++) {
        pan256[i] = (uint16_t)((channelPan[i] * 256) / 100);
    }
    
    writeQueueHead = 0;
    writeQueueTail = 0;
    chipCycleResid = 0;
}

SN76489Device::~SN76489Device() {
    i2s_audio_unregister_source(this);
}

int SN76489Device::init() {
    return i2s_audio_register_source(this);
}

int SN76489Device::readConfig(dictionary *ini) {
    if (!ini) return -1;
    
    const std::string section = getDevID();
    auto get_int = [&](const char* key, int default_value) {
        std::string full_key = section + ":" + key;
        return iniparser_getint(ini, full_key.c_str(), default_value);
    };

    channelPan[0] = get_int("tone0_pan", 20);
    channelPan[1] = get_int("tone1_pan", 80);
    channelPan[2] = get_int("tone2_pan", 40);
    channelPan[3] = get_int("noise_pan", 60);
    masterVolume = get_int("volume", 20);
    
    if (masterVolume > 100) masterVolume = 100;
    
    for (int i = 0; i < 4; i++) {
        if (channelPan[i] > 100) channelPan[i] = 100;
        pan256[i] = (uint16_t)((channelPan[i] * 256) / 100);
    }
    
    return 0;
}

int SN76489Device::flush() {
    return 0;
}


RAM_FUNC int SN76489Device::writeData(MZDevice* self, uint8_t port, uint8_t dt, uint8_t high_addr) {
    auto* sn = static_cast<SN76489Device*>(self);
    
    uint8_t head = sn->writeQueueHead;
    uint8_t next_head = (head + 1) & (WRITE_QUEUE_SIZE - 1);
    
    if (next_head != sn->writeQueueTail) {
        sn->writeQueue[head] = dt;
        sn->writeQueueHead = next_head;
    }
    
    return 0;
}

void SN76489Device::processWrites() {
    uint8_t tail = writeQueueTail;
    uint8_t head = writeQueueHead;

    while (tail != head) {
        writeRegister(writeQueue[tail]);
        tail = (tail + 1) & (WRITE_QUEUE_SIZE - 1);
    }

    writeQueueTail = tail;
}

void SN76489Device::writeRegister(uint8_t data) {
    if (data & 0x80) {
        latchedChannel = (data >> 5) & 0x03;
        latchedIsVolume = (data >> 4) & 0x01;
        
        if (latchedIsVolume) {
            uint8_t volume = data & 0x0F;
            if (latchedChannel < 3) {
                toneChannels[latchedChannel].volume = volume;
            } else {
                noiseChannel.volume = volume;
            }
        } else {
            if (latchedChannel < 3) {
                toneChannels[latchedChannel].frequency = 
                    (toneChannels[latchedChannel].frequency & 0x3F0) | (data & 0x0F);
            } else {
                noiseChannel.lfsr = 0x4000;
                noiseChannel.shift_rate = data & 0x03;
                noiseChannel.mode = (data >> 2) & 0x01;
            }
        }
    } else {
        if (!latchedIsVolume && latchedChannel < 3) {
            toneChannels[latchedChannel].frequency = 
                (toneChannels[latchedChannel].frequency & 0x00F) | ((data & 0x3F) << 4);
        }
    }
}

void SN76489Device::updateToneChannel(Channel& ch) {
    if (ch.frequency == 0 || ch.frequency == 1) {
        ch.output = true;
        return;
    }
    
    if (--ch.counter == 0) {
        ch.counter = ch.frequency;
        ch.output = !ch.output;
    }
}

void SN76489Device::updateNoiseChannel() {
    if (noiseChannel.shift_rate == 3 && toneChannels[2].frequency < 2) {
        noiseChannel.output = true;
        return;
    }
    
    uint16_t freq;
    switch (noiseChannel.shift_rate) {
        case 0: freq = 0x10; break;
        case 1: freq = 0x20; break;
        case 2: freq = 0x40; break;
        case 3: freq = toneChannels[2].frequency << 1; break;
    }
    
    if (--noiseChannel.counter == 0) {
        noiseChannel.counter = freq;
        
        uint16_t feedback = noiseChannel.mode
            ? (noiseChannel.lfsr & 0x01) ^ ((noiseChannel.lfsr >> 1) & 0x01)
            : (noiseChannel.lfsr & 0x01);
        
        noiseChannel.lfsr = (noiseChannel.lfsr >> 1) | (feedback << 14);
        if (noiseChannel.lfsr == 0) noiseChannel.lfsr = 1;
        noiseChannel.output = (noiseChannel.lfsr & 0x01);
    }
}

void SN76489Device::clockTick() {
    for (int i = 0; i < 3; i++) {
        updateToneChannel(toneChannels[i]);
    }
    updateNoiseChannel();
}

void SN76489Device::captureStereoSample(int16_t& left, int16_t& right) {
    int32_t leftSample = 0;
    int32_t rightSample = 0;
    
    for (int i = 0; i < 3; i++) {
        if (toneChannels[i].volume < 15) {
            int16_t amplitude = toneChannels[i].output
                ? volumeTable[toneChannels[i].volume]
                : -volumeTable[toneChannels[i].volume];
            
            leftSample += (amplitude * (int32_t)(256 - pan256[i])) >> 8;
            rightSample += (amplitude * (int32_t)pan256[i]) >> 8;
        }
    }
    
    if (noiseChannel.volume < 15) {
        int16_t amplitude = noiseChannel.output
            ? volumeTable[noiseChannel.volume]
            : -volumeTable[noiseChannel.volume];
        
        leftSample += (amplitude * (int32_t)(256 - pan256[3])) >> 8;
        rightSample += (amplitude * (int32_t)pan256[3]) >> 8;
    }
    
    if (leftSample > 32767) leftSample = 32767;
    if (leftSample < -32768) leftSample = -32768;
    if (rightSample > 32767) rightSample = 32767;
    if (rightSample < -32768) rightSample = -32768;
    
    // Apply master volume scaling
    leftSample = (leftSample * (int32_t)masterVolume) / 100;
    rightSample = (rightSample * (int32_t)masterVolume) / 100;
    
    left = (int16_t)leftSample;
    right = (int16_t)rightSample;
}

void SN76489Device::renderSample(int16_t& left, int16_t& right) {
    chipCycleResid += SN76489_CLOCK;
    uint32_t cyclesToRun = 0;
    while (chipCycleResid >= AUDIO_SAMPLE_RATE) {
        chipCycleResid -= AUDIO_SAMPLE_RATE;
        cyclesToRun++;
    }

    for (uint32_t c = 0; c < cyclesToRun; c++) {
        clockTick();
    }

    captureStereoSample(left, right);
}

