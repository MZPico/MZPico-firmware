#pragma once

// MZ-700 memory-write snoop stream (Deluxe only): the snoop PIO SM pushes
// one packed word per Z80 memory write - (data<<24)|(high<<16)|(low<<8) -
// and a pair of chained DMA channels drain it: the event channel copies
// the FIFO word into the event ring, then chain-triggers the timestamp
// channel, which snapshots the free-running 1 MHz timer into a parallel
// ring and chain-triggers the event channel back. Every captured write
// thus carries a ~1us-accurate timestamp at zero CPU cost - 1-bit beeper
// engines live entirely in edge timing, so consumers need it to place
// events at their correct sample offsets.
//
// The DMA must always keep draining (a stalled snoop SM would corrupt the
// shared transceiver gate state); consumers are lossy readers.

#include <stdint.h>
#include "hardware/pio.h"
#include "hardware/dma.h"

#ifdef BOARD_DELUXE

constexpr uint32_t MEM_SNOOP_RING_WORDS = 2048;   // x2 rings = 16 KB, ~7-20 ms of headroom

// Timestamp (pair-completion) DMA channel; exposed so RAM_FUNC bus
// handlers can read the producer cursor inline without calling into flash
extern int g_mem_snoop_ts_dma_ch;
extern uint32_t g_mem_snoop_ts_ring_base;

// Producer ring index of COMPLETE (event, timestamp) pairs, safe from
// RAM_FUNC handlers (pure MMIO read)
static inline uint32_t mem_snoop_cursor_inline(void) {
    return (dma_hw->ch[g_mem_snoop_ts_dma_ch].write_addr - g_mem_snoop_ts_ring_base) / 4;
}

// Claim two DMA channels and start streaming. Call once from core0 after
// bus_mem_snoop_init().
void mem_snoop_start(PIO pio, uint sm);

// Current producer position (complete pairs), ring index [0, RING_WORDS)
uint32_t mem_snoop_cursor(void);

// Event word / microsecond timestamp at index
uint32_t mem_snoop_read(uint32_t idx);
uint32_t mem_snoop_ts(uint32_t idx);

// Restart the chain if it ever stops. Cheap; call from the consumer's
// periodic scan.
void mem_snoop_service(void);

#endif
