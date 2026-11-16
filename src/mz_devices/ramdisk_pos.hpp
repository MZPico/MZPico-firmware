#pragma once

#include <stdint.h>

// The MR1R18 data-port address counter: bits 31..16 the page (always a
// page of the disk: the page register is taken modulo the size), bits
// 15..0 the address inside it. A data access advances the address and
// wraps it inside the same 64 KB page, so the position stays below the
// disk size. (Until 2026-11 the wrap was a no-op and the counter ran on
// into the next page, past the end of the disk on its last one.)
static inline uint32_t mr1r18_next_pos(uint32_t pos) {
    return (pos & 0xFFFF0000u) | ((pos + 1) & 0xFFFFu);
}
