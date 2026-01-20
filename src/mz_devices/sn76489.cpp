#include <cstring>
#include <cmath>
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include "i2s_audio.pio.h"
#include "sn76489.hpp"
#include "common.hpp"

REGISTER_MZ_DEVICE(SN76489Device)

// Core0: Audio (DMA, PIO, I2S)
// Core1: Bus monitoring (port writes)

// Volume table: 2dB steps, scaled so 4 channels at max sum near full-scale (32767)
const int16_t SN76489Device::volumeTable[16] = {
    8192, 6508, 5172, 4108, 3264, 2592, 2060, 1636,
    1300, 1032, 820, 652, 516, 412, 328, 0
};

SN76489Device* SN76489Device::activeInstance = nullptr;

SN76489Device::SN76489Device() {
    writeMappings[0].fn = SN76489Device::writeData;
    readPortCount = 0;
    writePortCount = 1;
    
    for (int i = 0; i < 3; i++) {
        toneChannels[i].frequency = 0;
        toneChannels[i].counter = 0;
        toneChannels[i].volume = 15;
        toneChannels[i].output = false;
    }
    
    noiseChannel.frequency = 0x10;
    noiseChannel.counter = 0x10;
    noiseChannel.lfsr = 0x4000;
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
    
    memset(audioBuffer, 0, sizeof(audioBuffer));
    currentBuffer = 0;
    bufferFillPos = 0;
    bufferReady[0] = false;
    bufferReady[1] = false;
    audioInitialized = false;
    
    writeQueueHead = 0;
    writeQueueTail = 0;
    chipCycleResid = 0;
    
    audioPIO = nullptr;
    audioSM = 0;
    audioDMA = 0;
}

SN76489Device::~SN76489Device() {
    stopAudio();
    if (activeInstance == this) {
        activeInstance = nullptr;
    }
}

int SN76489Device::init() {
    return 0;
}

int SN76489Device::readConfig(dictionary *ini) {
    if (!ini) return -1;
    
    channelPan[0] = iniparser_getint(ini, "psg:tone0_pan", 20);
    channelPan[1] = iniparser_getint(ini, "psg:tone1_pan", 80);
    channelPan[2] = iniparser_getint(ini, "psg:tone2_pan", 40);
    channelPan[3] = iniparser_getint(ini, "psg:noise_pan", 60);
    masterVolume = iniparser_getint(ini, "psg:volume", 20);
    
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

int SN76489Device::initAudioOnCore0() {
    if (get_core_num() != 0) return -100;
    if (audioInitialized) return 0;
    
    audioPIO = pio0;
    if (!pio_can_add_program(audioPIO, &i2s_audio_program)) {
        audioPIO = pio1;
        if (!pio_can_add_program(audioPIO, &i2s_audio_program)) {
            return -1;
        }
    }
    
    audioSM = pio_claim_unused_sm(audioPIO, false);
    if (audioSM == -1) return -2;
    
    i2s_audio_program_init(audioPIO, audioSM, I2S_DATA_PIN, I2S_BCLK_PIN, I2S_LRCLK_PIN);

    audioDMA = dma_claim_unused_channel(false);
    if (audioDMA == -1) {
        pio_sm_set_enabled(audioPIO, audioSM, false);
        pio_sm_unclaim(audioPIO, audioSM);
        return -3;
    }
    
    dma_channel_config dma_config = dma_channel_get_default_config(audioDMA);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_dreq(&dma_config, pio_get_dreq(audioPIO, audioSM, true));
    
    dma_channel_configure(audioDMA, &dma_config, &audioPIO->txf[audioSM],
                         audioBuffer[0], AUDIO_BUFFER_SIZE, false);
    
    dma_channel_set_irq0_enabled(audioDMA, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dmaIRQHandler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);
    
    activeInstance = this;
    audioInitialized = true;
    
    startAudio();
    
    return 0;
}

void SN76489Device::startAudio() {
    printf("SN76489: Starting audio output...\n");
    
    for (int i = 0; i < 3; i++) {
        toneChannels[i].frequency = 0;
        toneChannels[i].volume = 15;
        toneChannels[i].counter = 0x400;
        toneChannels[i].output = 0;
    }
    
    noiseChannel.volume = 15;
    noiseChannel.mode = 0;
    noiseChannel.shift_rate = 0;
    noiseChannel.lfsr = 0x8000;
    noiseChannel.output = 0;
    
    writeQueueHead = 0;
    writeQueueTail = 0;
    chipCycleResid = 0;
    
    bufferFillPos = 0;
    while (!continueFillBuffer(AUDIO_BUFFER_SIZE / 2)) {}
    bufferFillPos = 0;
    currentBuffer = 1;
    while (!continueFillBuffer(AUDIO_BUFFER_SIZE / 2)) {}
    currentBuffer = 0;
    bufferReady[0] = true;
    bufferReady[1] = true;
    
    dma_channel_start(audioDMA);
    pio_sm_set_enabled(audioPIO, audioSM, true);
}

void SN76489Device::stopAudio() {
    if (audioDMA) {
        dma_channel_abort(audioDMA);
        irq_set_enabled(DMA_IRQ_0, false);
        dma_channel_unclaim(audioDMA);
    }
    
    if (audioPIO && audioSM) {
        pio_sm_set_enabled(audioPIO, audioSM, false);
        pio_sm_unclaim(audioPIO, audioSM);
    }
}

void SN76489Device::dmaIRQHandler() {
    if (!activeInstance) return;
    
    dma_hw->ints0 = 1u << activeInstance->audioDMA;
    
    uint8_t nextBuffer = 1 - activeInstance->currentBuffer;

    if (!activeInstance->bufferReady[nextBuffer]) {
        // Underrun: repeat current buffer instead of stopping
        dma_channel_set_read_addr(activeInstance->audioDMA, 
                                   activeInstance->audioBuffer[activeInstance->currentBuffer], true);
        return;
    }

    activeInstance->currentBuffer = nextBuffer;
    activeInstance->bufferReady[nextBuffer] = false;
    activeInstance->bufferFillPos = 0;
    dma_channel_set_read_addr(activeInstance->audioDMA, 
                               activeInstance->audioBuffer[nextBuffer], true);
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

void SN76489Device::processWritesFromMainLoop() {
    if (!audioInitialized) return;

    uint8_t nextBuffer = 1 - currentBuffer;

    if (bufferFillPos == 0 && !bufferReady[nextBuffer]) {
        uint8_t tail = writeQueueTail;
        uint8_t head = writeQueueHead;
        
        while (tail != head) {
            writeRegister(writeQueue[tail]);
            tail = (tail + 1) & (WRITE_QUEUE_SIZE - 1);
        }
        
        writeQueueTail = tail;
    }

    while (!bufferReady[nextBuffer] && bufferFillPos < AUDIO_BUFFER_SIZE / 2) {
        continueFillBuffer(AUDIO_BUFFER_SIZE / 2);
    }
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

bool SN76489Device::continueFillBuffer(uint32_t chunkSize) {
    if (!audioInitialized) return false;
    
    uint8_t fillBuffer = 1 - currentBuffer;
    
    uint32_t samplesToFill = chunkSize;
    if (bufferFillPos + samplesToFill > AUDIO_BUFFER_SIZE / 2) {
        samplesToFill = (AUDIO_BUFFER_SIZE / 2) - bufferFillPos;
    }
    
    for (uint32_t i = 0; i < samplesToFill; i++) {
        chipCycleResid += SN76489_CLOCK;
        uint32_t cyclesToRun = 0;
        while (chipCycleResid >= AUDIO_SAMPLE_RATE) {
            chipCycleResid -= AUDIO_SAMPLE_RATE;
            cyclesToRun++;
        }
        
        for (uint32_t c = 0; c < cyclesToRun; c++) {
            clockTick();
        }
        
        int16_t leftSample, rightSample;
        captureStereoSample(leftSample, rightSample);
        
        int32_t left32 = ((int32_t)leftSample) << 16;
        int32_t right32 = ((int32_t)rightSample) << 16;
        
        uint32_t bufIdx = bufferFillPos * 2;
        audioBuffer[fillBuffer][bufIdx] = left32;
        audioBuffer[fillBuffer][bufIdx + 1] = right32;
        
        bufferFillPos++;
    }
    
    if (bufferFillPos >= AUDIO_BUFFER_SIZE / 2) {
        bufferReady[fillBuffer] = true;
        return true;
    }
    
    return false;
}

