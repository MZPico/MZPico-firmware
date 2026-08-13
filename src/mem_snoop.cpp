#include "common.hpp"

#ifdef BOARD_DELUXE

#include "mem_snoop.hpp"
#include "hardware/dma.h"

// Ring must be size-aligned for the DMA address wrap
static uint32_t __attribute__((aligned(MEM_SNOOP_RING_WORDS * 4))) ring[MEM_SNOOP_RING_WORDS];

static int dma_ch = -1;

static void arm(void) {
    // Largest possible transfer count; mem_snoop_service() re-arms long
    // before wraparound matters. The write address ring keeps the target
    // inside the buffer forever.
    dma_channel_set_trans_count(dma_ch, 0xFFFFFFFFu, true);
}

void mem_snoop_start(PIO pio, uint sm) {
    dma_ch = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true /* write */, __builtin_ctz(MEM_SNOOP_RING_WORDS * 4));
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));

    dma_channel_configure(dma_ch, &c, ring, &pio->rxf[sm], 0, false);
    arm();
}

uint32_t mem_snoop_cursor(void) {
    uint32_t wr = dma_hw->ch[dma_ch].write_addr;
    return (wr - (uint32_t)ring) / 4;
}

uint32_t mem_snoop_read(uint32_t idx) {
    return ring[idx & (MEM_SNOOP_RING_WORDS - 1)];
}

void mem_snoop_service(void) {
    if (dma_ch >= 0 && !dma_channel_is_busy(dma_ch)) {
        arm();
    }
}

#endif
