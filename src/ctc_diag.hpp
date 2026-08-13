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

    // D2 (PC0 gate) pulse statistics, from event timestamps: width of the
    // last 8 "1" phases and the last 8 rise-to-rise periods, microseconds
    // clamped to 16 bits
    uint32_t d2RiseTs;
    bool d2High;
    uint16_t d2Width[8];
    uint32_t d2WidthPos;
    uint16_t d2Period[8];
    uint32_t d2PeriodPos;

    // Tone model state snapshots (packed: bit0 gate, bit1 out, bit2
    // running, bits 4-6 mode) + reload value, refreshed each buffer
    uint8_t ioToneState;
    uint32_t ioToneReload;
    uint8_t memToneState;
    uint32_t memToneReload;

    // Timeline placement (I/O path): total events pushed, how many were
    // clamped to the window start (collapsed edges!), the playhead backlog
    // and the last raw rel offsets in us
    uint32_t ioPushed;
    uint32_t ioNegClamped;
    int32_t ioBacklogUs;
    uint16_t ioLastRel[8];
};

extern CtcDiag g_ctc_diag;
