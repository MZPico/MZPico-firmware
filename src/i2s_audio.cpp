#include <cstring>
#include <stdio.h>

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include "pico/multicore.h"

#include "i2s_audio.pio.h"
#include "i2s_audio.hpp"
#include "common.hpp"

static I2SAudioSource* g_sources[MAX_AUDIO_SOURCES] = {nullptr};
static uint8_t g_source_count = 0;

static PIO g_audio_pio = nullptr;
static int g_audio_sm = -1;
static int g_audio_dma = -1;
static int32_t g_audio_buffer[2][AUDIO_BUFFER_SIZE];
static uint8_t g_current_buffer = 0;
static uint32_t g_buffer_fill_pos = 0;
static bool g_buffer_ready[2] = {false, false};
static bool g_audio_initialized = false;

static void start_audio();
static void stop_audio();
static bool continue_fill_buffer(uint32_t chunk_size = 32);
static void dma_irq_handler();

int i2s_audio_register_source(I2SAudioSource* source) {
    if (!source) return -1;

    for (uint8_t i = 0; i < g_source_count; i++) {
        if (g_sources[i] == source) return 0;
    }

    if (g_source_count >= MAX_AUDIO_SOURCES) return -2;

    g_sources[g_source_count++] = source;
    return 0;
}

void i2s_audio_unregister_source(I2SAudioSource* source) {
    if (!source || g_source_count == 0) return;

    for (uint8_t i = 0; i < g_source_count; i++) {
        if (g_sources[i] == source) {
            for (uint8_t j = i + 1; j < g_source_count; j++) {
                g_sources[j - 1] = g_sources[j];
            }
            g_sources[g_source_count - 1] = nullptr;
            g_source_count--;
            break;
        }
    }
}

bool i2s_audio_has_sources() {
    return g_source_count > 0;
}

bool i2s_audio_is_ready() {
    return g_audio_initialized;
}

int i2s_audio_init_on_core0() {
    if (get_core_num() != 0) return -100;
    if (g_audio_initialized) return 0;
    if (g_source_count == 0) return -101;

    g_audio_pio = pio0;
    if (!pio_can_add_program(g_audio_pio, &i2s_audio_program)) {
        g_audio_pio = pio1;
        if (!pio_can_add_program(g_audio_pio, &i2s_audio_program)) {
            return -1;
        }
    }

    g_audio_sm = pio_claim_unused_sm(g_audio_pio, false);
    if (g_audio_sm < 0) return -2;

    i2s_audio_program_init(g_audio_pio, g_audio_sm, I2S_DATA_PIN, I2S_BCLK_PIN, I2S_LRCLK_PIN);

    g_audio_dma = dma_claim_unused_channel(false);
    if (g_audio_dma < 0) {
        pio_sm_set_enabled(g_audio_pio, g_audio_sm, false);
        pio_sm_unclaim(g_audio_pio, g_audio_sm);
        g_audio_sm = -1;
        return -3;
    }

    dma_channel_config dma_config = dma_channel_get_default_config(g_audio_dma);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_dreq(&dma_config, pio_get_dreq(g_audio_pio, g_audio_sm, true));

    dma_channel_configure(g_audio_dma, &dma_config, &g_audio_pio->txf[g_audio_sm],
                          g_audio_buffer[0], AUDIO_BUFFER_SIZE, false);

    dma_channel_set_irq0_enabled(g_audio_dma, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);

    g_audio_initialized = true;

    start_audio();

    return 0;
}

void i2s_audio_shutdown() {
    stop_audio();
    g_audio_initialized = false;
}

void i2s_audio_poll() {
    if (!g_audio_initialized) return;

    uint8_t next_buffer = 1 - g_current_buffer;

    if (g_buffer_fill_pos == 0 && !g_buffer_ready[next_buffer]) {
        for (uint8_t i = 0; i < g_source_count; i++) {
            if (g_sources[i]) {
                g_sources[i]->processWrites();
            }
        }
    }

    while (!g_buffer_ready[next_buffer] && g_buffer_fill_pos < AUDIO_BUFFER_SIZE / 2) {
        continue_fill_buffer(AUDIO_BUFFER_SIZE / 2);
    }
}

static void start_audio() {
    printf("I2S: Starting audio output...\n");

    g_current_buffer = 0;
    g_buffer_ready[0] = false;
    g_buffer_ready[1] = false;
    g_buffer_fill_pos = 0;
    while (!continue_fill_buffer(AUDIO_BUFFER_SIZE / 2)) {}
    g_buffer_fill_pos = 0;
    g_current_buffer = 1;
    while (!continue_fill_buffer(AUDIO_BUFFER_SIZE / 2)) {}
    g_current_buffer = 0;
    g_buffer_ready[0] = true;
    g_buffer_ready[1] = true;

    dma_channel_start(g_audio_dma);
    pio_sm_set_enabled(g_audio_pio, g_audio_sm, true);
}

static void stop_audio() {
    if (g_audio_dma >= 0) {
        dma_channel_abort(g_audio_dma);
        irq_set_enabled(DMA_IRQ_0, false);
        dma_channel_unclaim(g_audio_dma);
        g_audio_dma = -1;
    }

    if (g_audio_pio && g_audio_sm >= 0) {
        pio_sm_set_enabled(g_audio_pio, g_audio_sm, false);
        pio_sm_unclaim(g_audio_pio, g_audio_sm);
        g_audio_sm = -1;
    }
}

static void dma_irq_handler() {
    if (g_audio_dma < 0) return;

    dma_hw->ints0 = 1u << g_audio_dma;

    uint8_t next_buffer = 1 - g_current_buffer;

    if (!g_buffer_ready[next_buffer]) {
        dma_channel_set_read_addr(g_audio_dma, g_audio_buffer[g_current_buffer], true);
        return;
    }

    g_current_buffer = next_buffer;
    g_buffer_ready[next_buffer] = false;
    g_buffer_fill_pos = 0;
    dma_channel_set_read_addr(g_audio_dma, g_audio_buffer[next_buffer], true);
}

static bool continue_fill_buffer(uint32_t chunk_size) {
    uint8_t fill_buffer = 1 - g_current_buffer;

    uint32_t samples_to_fill = chunk_size;
    if (g_buffer_fill_pos + samples_to_fill > AUDIO_BUFFER_SIZE / 2) {
        samples_to_fill = (AUDIO_BUFFER_SIZE / 2) - g_buffer_fill_pos;
    }

    uint8_t active_sources = 0;
    for (uint8_t s = 0; s < g_source_count; s++) {
        if (g_sources[s]) active_sources++;
    }

    if (active_sources == 0) {
        for (uint32_t i = 0; i < samples_to_fill; i++) {
            uint32_t buf_idx = g_buffer_fill_pos * 2;
            g_audio_buffer[fill_buffer][buf_idx] = 0;
            g_audio_buffer[fill_buffer][buf_idx + 1] = 0;
            g_buffer_fill_pos++;
        }
    } else {
        for (uint32_t i = 0; i < samples_to_fill; i++) {
        int32_t mix_left = 0;
        int32_t mix_right = 0;

        for (uint8_t s = 0; s < g_source_count; s++) {
            if (!g_sources[s]) continue;
            int16_t left = 0;
            int16_t right = 0;
            g_sources[s]->renderSample(left, right);
            mix_left += left;
            mix_right += right;
        }

            mix_left /= active_sources;
            mix_right /= active_sources;

        if (mix_left > 32767) mix_left = 32767;
        if (mix_left < -32768) mix_left = -32768;
        if (mix_right > 32767) mix_right = 32767;
        if (mix_right < -32768) mix_right = -32768;

        int32_t left32 = mix_left << 16;
        int32_t right32 = mix_right << 16;

        uint32_t buf_idx = g_buffer_fill_pos * 2;
        g_audio_buffer[fill_buffer][buf_idx] = left32;
        g_audio_buffer[fill_buffer][buf_idx + 1] = right32;
            g_buffer_fill_pos++;
        }
    }

    if (g_buffer_fill_pos >= AUDIO_BUFFER_SIZE / 2) {
        g_buffer_ready[fill_buffer] = true;
        return true;
    }

    return false;
}
