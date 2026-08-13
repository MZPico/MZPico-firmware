#include "common.hpp"

#ifdef BOARD_DELUXE

#include "mem_snoop.hpp"

// Ring must be size-aligned for the DMA address wrap
static uint32_t __attribute__((aligned(MEM_SNOOP_RING_WORDS * 4))) ring[MEM_SNOOP_RING_WORDS];

int g_mem_snoop_dma_ch = -1;   // valid only after mem_snoop_start()
uint32_t g_mem_snoop_ring_base = (uint32_t)ring;

static void arm(void) {
    // Largest possible transfer count; mem_snoop_service() re-arms long
    // before wraparound matters. The write address ring keeps the target
    // inside the buffer forever.
    dma_channel_set_trans_count(g_mem_snoop_dma_ch, 0xFFFFFFFFu, true);
}

void mem_snoop_start(PIO pio, uint sm) {
    g_mem_snoop_dma_ch = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(g_mem_snoop_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true /* write */, __builtin_ctz(MEM_SNOOP_RING_WORDS * 4));
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));

    dma_channel_configure(g_mem_snoop_dma_ch, &c, ring, &pio->rxf[sm], 0, false);
    arm();
}

uint32_t mem_snoop_cursor(void) {
    return mem_snoop_cursor_inline();
}

uint32_t mem_snoop_read(uint32_t idx) {
    return ring[idx & (MEM_SNOOP_RING_WORDS - 1)];
}

void mem_snoop_service(void) {
    if (g_mem_snoop_dma_ch >= 0 && !dma_channel_is_busy(g_mem_snoop_dma_ch)) {
        arm();
    }
}

#endif
