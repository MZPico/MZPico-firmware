#include "common.hpp"

#ifdef BOARD_DELUXE

#include "mem_snoop.hpp"
#include "hardware/timer.h"

// Rings must be size-aligned for the DMA address wrap
static uint32_t __attribute__((aligned(MEM_SNOOP_RING_WORDS * 4))) event_ring[MEM_SNOOP_RING_WORDS];
static uint32_t __attribute__((aligned(MEM_SNOOP_RING_WORDS * 4))) ts_ring[MEM_SNOOP_RING_WORDS];

static int ev_ch = -1;
int g_mem_snoop_ts_dma_ch = -1;
uint32_t g_mem_snoop_ts_ring_base = (uint32_t)ts_ring;

void mem_snoop_start(PIO pio, uint sm) {
    ev_ch = dma_claim_unused_channel(true);
    g_mem_snoop_ts_dma_ch = dma_claim_unused_channel(true);

    // Event channel: one FIFO word per trigger, paced by the PIO DREQ,
    // then chain to the timestamp channel
    dma_channel_config ce = dma_channel_get_default_config(ev_ch);
    channel_config_set_transfer_data_size(&ce, DMA_SIZE_32);
    channel_config_set_read_increment(&ce, false);
    channel_config_set_write_increment(&ce, true);
    channel_config_set_ring(&ce, true /* write */, __builtin_ctz(MEM_SNOOP_RING_WORDS * 4));
    channel_config_set_dreq(&ce, pio_get_dreq(pio, sm, false));
    channel_config_set_chain_to(&ce, g_mem_snoop_ts_dma_ch);
    dma_channel_configure(ev_ch, &ce, event_ring, &pio->rxf[sm], 1, false);

    // Timestamp channel: unpaced snapshot of the free-running 1 MHz timer,
    // chains straight back to the event channel. Its write pointer trails
    // the event channel's by design, so it doubles as the "complete pairs"
    // cursor.
    dma_channel_config ct = dma_channel_get_default_config(g_mem_snoop_ts_dma_ch);
    channel_config_set_transfer_data_size(&ct, DMA_SIZE_32);
    channel_config_set_read_increment(&ct, false);
    channel_config_set_write_increment(&ct, true);
    channel_config_set_ring(&ct, true /* write */, __builtin_ctz(MEM_SNOOP_RING_WORDS * 4));
    channel_config_set_chain_to(&ct, ev_ch);
    dma_channel_configure(g_mem_snoop_ts_dma_ch, &ct, ts_ring, &timer_hw->timerawl, 1, false);

    dma_channel_start(ev_ch);
}

uint32_t mem_snoop_cursor(void) {
    return mem_snoop_cursor_inline();
}

uint32_t mem_snoop_read(uint32_t idx) {
    return event_ring[idx & (MEM_SNOOP_RING_WORDS - 1)];
}

uint32_t mem_snoop_ts(uint32_t idx) {
    return ts_ring[idx & (MEM_SNOOP_RING_WORDS - 1)];
}

void mem_snoop_service(void) {
    // The ping-pong chain re-arms itself forever (count reloads to 1 on
    // every chain trigger); if both channels ever idle, the chain died -
    // restart it
    if (ev_ch >= 0 &&
        !dma_channel_is_busy(ev_ch) &&
        !dma_channel_is_busy(g_mem_snoop_ts_dma_ch)) {
        dma_channel_start(ev_ch);
    }
}

#endif
