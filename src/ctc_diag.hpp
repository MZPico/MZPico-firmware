#pragma once

#include <stdint.h>

// Live counters for diagnosing CTC/melody capture: incremented by the
// sound devices on core0, read out over the REST API (/api/ctcdiag) so a
// failing game's actual write technique can be observed WHILE it runs,
// without touching the Z80 side. Temporary instrumentation - cheap enough
// to leave in, but not part of any contract.
struct CtcDiag {
    // memory snoop (melody device)
    uint32_t memTotal;      // snooped memory writes consumed
    uint32_t memE0Page;     // of those, high byte == 0xE0
    uint32_t memLow[16];    // per-address counts for E000-E00F
    uint32_t memRejected;   // E004-E008 writes rejected by bank gating
    uint32_t bank[5];       // applied bank events: E1, E3, E4, E5, E6
    uint8_t lastMem[64];    // last 32 accepted (low, data) pairs, ring
    uint32_t lastMemPos;

    // I/O path (ctc device)
    uint32_t ioTotal;       // writes drained from the D0-D7 queue
    uint32_t ioPort[8];     // per-port counts
    uint8_t lastIo[64];     // last 32 (port, data) pairs, ring
    uint32_t lastIoPos;
};

extern CtcDiag g_ctc_diag;
