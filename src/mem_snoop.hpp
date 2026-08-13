#pragma once

// MZ-700 memory-write snoop stream (Deluxe only): the snoop PIO SM pushes
// one packed word per Z80 memory write - (data<<24)|(high<<16)|(low<<8) -
// and a free-running DMA channel drains it into a RAM ring buffer. The DMA
// must always keep draining (a stalled snoop SM would corrupt the shared
// transceiver gate state); consumers are lossy readers of the ring.

#include <stdint.h>
#include "hardware/pio.h"
#include "hardware/dma.h"

#ifdef BOARD_DELUXE

constexpr uint32_t MEM_SNOOP_RING_WORDS = 2048;   // 8 KB, ~7-20 ms at worst-case write density

// Claimed DMA channel; exposed so RAM_FUNC bus handlers can read the
// producer cursor inline without calling into flash
extern int g_mem_snoop_dma_ch;
extern uint32_t g_mem_snoop_ring_base;

// Producer ring index, safe from RAM_FUNC handlers (pure MMIO read)
static inline uint32_t mem_snoop_cursor_inline(void) {
    return (dma_hw->ch[g_mem_snoop_dma_ch].write_addr - g_mem_snoop_ring_base) / 4;
}

// Claim a DMA channel and start streaming the snoop SM's RX FIFO into the
// ring. Call once from core0 after bus_mem_snoop_init().
void mem_snoop_start(PIO pio, uint sm);

// Current producer position as a monotonically meaningless ring index
// [0, MEM_SNOOP_RING_WORDS). Words behind it (mod ring size) are valid.
uint32_t mem_snoop_cursor(void);

// Ring word at index (caller keeps its own read cursor).
uint32_t mem_snoop_read(uint32_t idx);

// Re-arm the DMA channel if its (huge) transfer count ever runs out.
// Cheap; call from the consumer's periodic scan.
void mem_snoop_service(void);

#endif
